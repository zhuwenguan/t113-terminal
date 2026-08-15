#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "logger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static bool logger_mount_is_writable(const char *mount_point)
{
    FILE *mounts;
    char device[128];
    char mounted_path[256];
    char filesystem[64];
    char options[512];
    int dump_frequency;
    int fsck_order;

    mounts = fopen("/proc/mounts", "r");
    if (mounts == NULL) {
        return false;
    }

    while (fscanf(mounts, "%127s %255s %63s %511s %d %d", device,
                  mounted_path, filesystem, options, &dump_frequency,
                  &fsck_order) == 6) {
        char *option;
        char *save_pointer = NULL;
        bool read_only = false;

        if (strcmp(mounted_path, mount_point) != 0) {
            continue;
        }

        option = strtok_r(options, ",", &save_pointer);
        while (option != NULL) {
            if (strcmp(option, "ro") == 0) {
                read_only = true;
                break;
            }
            option = strtok_r(NULL, ",", &save_pointer);
        }

        fclose(mounts);
        return !read_only;
    }

    fclose(mounts);
    return false;
}

static int logger_prepare_storage(void)
{
    struct stat directory_status;
    struct statvfs filesystem_status;

    if (!logger_mount_is_writable(LOGGER_MOUNT_POINT)) {
        fprintf(stderr, "Logger mount is missing or read-only: %s\n",
                LOGGER_MOUNT_POINT);
        errno = ENODEV;
        return -1;
    }

    if (statvfs(LOGGER_MOUNT_POINT, &filesystem_status) < 0) {
        fprintf(stderr, "Cannot stat logger mount %s: %s\n",
                LOGGER_MOUNT_POINT, strerror(errno));
        return -1;
    }
    if ((filesystem_status.f_flag & ST_RDONLY) != 0) {
        fprintf(stderr, "Logger mount is read-only: %s\n",
                LOGGER_MOUNT_POINT);
        errno = EROFS;
        return -1;
    }

    if (mkdir(LOGGER_LOG_DIRECTORY, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "Cannot create logger directory %s: %s\n",
                LOGGER_LOG_DIRECTORY, strerror(errno));
        return -1;
    }
    if (stat(LOGGER_LOG_DIRECTORY, &directory_status) < 0 ||
        !S_ISDIR(directory_status.st_mode)) {
        fprintf(stderr, "Logger path is not a directory: %s\n",
                LOGGER_LOG_DIRECTORY);
        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

static int logger_make_path(Logger *logger)
{
    time_t current_time;
    struct tm local_time;
    char timestamp[32];

    current_time = time(NULL);
    if (localtime_r(&current_time, &local_time) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S",
                 &local_time) == 0) {
        snprintf(timestamp, sizeof(timestamp), "unknown_time");
    }

    if (snprintf(logger->log_path, sizeof(logger->log_path),
                 "%s/can_%s.csv", LOGGER_LOG_DIRECTORY, timestamp) >=
        (int)sizeof(logger->log_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int logger_write_header(Logger *logger)
{
    struct stat file_status;

    if (fstat(fileno(logger->file), &file_status) < 0) {
        return -1;
    }
    if (file_status.st_size != 0) {
        return 0;
    }

    if (fprintf(logger->file,
                "# t113 CAN log v1\n"
                "# timestamp_us,bus,can_id,dlc,data\n") < 0 ||
        fflush(logger->file) < 0) {
        return -1;
    }

    return 0;
}

int logger_init(Logger *logger)
{
    int condition_result;

    if (logger == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(logger, 0, sizeof(*logger));
    if (logger_prepare_storage() < 0 || logger_make_path(logger) < 0) {
        return -1;
    }

    logger->file = fopen(logger->log_path, "a");
    if (logger->file == NULL) {
        fprintf(stderr, "Cannot open logger file %s: %s\n",
                logger->log_path, strerror(errno));
        return -1;
    }
    setvbuf(logger->file, NULL, _IOFBF, 16U * 1024U);
    if (logger_write_header(logger) < 0) {
        fprintf(stderr, "Cannot write logger header %s: %s\n",
                logger->log_path, strerror(errno));
        fclose(logger->file);
        logger->file = NULL;
        return -1;
    }

    if (pthread_mutex_init(&logger->mutex, NULL) != 0) {
        fprintf(stderr, "Cannot initialize logger mutex\n");
        fclose(logger->file);
        logger->file = NULL;
        return -1;
    }
    condition_result = pthread_cond_init(&logger->condition, NULL);
    if (condition_result != 0) {
        fprintf(stderr, "Cannot initialize logger condition\n");
        pthread_mutex_destroy(&logger->mutex);
        fclose(logger->file);
        logger->file = NULL;
        return -1;
    }

    logger->initialized = true;
    return 0;
}

static void logger_write_batch(Logger *logger, const CanMessage *batch,
                               size_t count)
{
    size_t index;

    if (logger->stats.file_failed) {
        logger->stats.dropped_frames += count;
        return;
    }

    for (index = 0; index < count; ++index) {
        const CanMessage *message = &batch[index];
        unsigned int data_index;

        if (fprintf(logger->file, "%llu,%u,0x%03x,%u,",
                    (unsigned long long)message->timestamp_us,
                    (unsigned int)message->bus,
                    (unsigned int)message->can_id,
                    (unsigned int)message->dlc) < 0) {
            logger->stats.write_errors++;
            logger->stats.file_failed = true;
            logger->stats.dropped_frames += count - index;
            return;
        }

        for (data_index = 0; data_index < message->dlc; ++data_index) {
            if (fprintf(logger->file, "%s%02x",
                        data_index == 0 ? "" : " ",
                        message->data[data_index]) < 0) {
                logger->stats.write_errors++;
                logger->stats.file_failed = true;
                logger->stats.dropped_frames += count - index;
                return;
            }
        }
        if (fputc('\n', logger->file) == EOF) {
            logger->stats.write_errors++;
            logger->stats.file_failed = true;
            logger->stats.dropped_frames += count - index;
            return;
        }
        logger->stats.written_frames++;
    }

    if (fflush(logger->file) < 0) {
        logger->stats.write_errors++;
        logger->stats.file_failed = true;
        return;
    }
    logger->stats.flushes++;
}

static void logger_make_deadline(struct timespec *deadline)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += LOGGER_FLUSH_INTERVAL_MS / 1000U;
    deadline->tv_nsec += (long)(LOGGER_FLUSH_INTERVAL_MS % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void *logger_thread_main(void *context)
{
    Logger *logger = context;

    for (;;) {
        CanMessage batch[LOGGER_BATCH_SIZE];
        size_t batch_count = 0;
        bool should_exit;

        pthread_mutex_lock(&logger->mutex);
        while (logger->queue_count < LOGGER_BATCH_SIZE &&
               !logger->stop_requested) {
            struct timespec deadline;
            int wait_result;

            logger_make_deadline(&deadline);
            wait_result = pthread_cond_timedwait(&logger->condition,
                                                 &logger->mutex, &deadline);
            if (wait_result != 0 && wait_result != ETIMEDOUT) {
                fprintf(stderr, "Logger condition wait failed: %s\n",
                        strerror(wait_result));
                logger->stop_requested = true;
                break;
            }
        }

        while (batch_count < LOGGER_BATCH_SIZE && logger->queue_count > 0) {
            batch[batch_count++] = logger->queue[logger->queue_head];
            logger->queue_head =
                (logger->queue_head + 1U) % LOGGER_QUEUE_CAPACITY;
            logger->queue_count--;
        }
        should_exit = logger->stop_requested && logger->queue_count == 0;
        pthread_mutex_unlock(&logger->mutex);

        if (batch_count > 0) {
            logger_write_batch(logger, batch, batch_count);
        }
        if (should_exit) {
            break;
        }
    }

    return NULL;
}

int logger_start(Logger *logger)
{
    int result;

    if (logger == NULL || !logger->initialized || logger->started) {
        errno = EINVAL;
        return -1;
    }

    result = pthread_create(&logger->thread, NULL, logger_thread_main, logger);
    if (result != 0) {
        fprintf(stderr, "Cannot start logger thread: %s\n",
                strerror(result));
        return -1;
    }
    logger->started = true;
    return 0;
}

void logger_message_sink(const CanMessage *message, void *context)
{
    Logger *logger = context;

    if (logger == NULL || message == NULL || !logger->initialized ||
        !logger->started) {
        return;
    }

    if (pthread_mutex_lock(&logger->mutex) != 0) {
        return;
    }

    if (logger->stop_requested || logger->queue_count >= LOGGER_QUEUE_CAPACITY) {
        logger->stats.dropped_frames++;
        pthread_mutex_unlock(&logger->mutex);
        return;
    }

    logger->queue[logger->queue_tail] = *message;
    logger->queue_tail =
        (logger->queue_tail + 1U) % LOGGER_QUEUE_CAPACITY;
    logger->queue_count++;
    logger->stats.enqueued_frames++;
    if (logger->queue_count > logger->stats.queue_high_watermark) {
        logger->stats.queue_high_watermark = logger->queue_count;
    }
    pthread_cond_signal(&logger->condition);
    pthread_mutex_unlock(&logger->mutex);
}

void logger_stop(Logger *logger)
{
    if (logger == NULL || !logger->initialized) {
        return;
    }

    if (logger->started) {
        pthread_mutex_lock(&logger->mutex);
        logger->stop_requested = true;
        pthread_cond_signal(&logger->condition);
        pthread_mutex_unlock(&logger->mutex);
        pthread_join(logger->thread, NULL);
        logger->started = false;
    }

    if (logger->file != NULL) {
        if (!logger->stats.file_failed && fflush(logger->file) < 0) {
            logger->stats.write_errors++;
            logger->stats.file_failed = true;
        }
        fclose(logger->file);
        logger->file = NULL;
    }

    pthread_cond_destroy(&logger->condition);
    pthread_mutex_destroy(&logger->mutex);
    logger->initialized = false;
}

const char *logger_get_path(const Logger *logger)
{
    if (logger == NULL) {
        return NULL;
    }
    return logger->log_path;
}

void logger_print_stats(const Logger *logger)
{
    if (logger == NULL) {
        return;
    }

    printf("Logger statistics:\n");
    printf("  path=%s\n", logger->log_path);
    printf("  enqueued_frames=%llu written_frames=%llu "
           "dropped_frames=%llu write_errors=%llu flushes=%llu "
           "queue_high_watermark=%zu file_failed=%s\n",
           (unsigned long long)logger->stats.enqueued_frames,
           (unsigned long long)logger->stats.written_frames,
           (unsigned long long)logger->stats.dropped_frames,
           (unsigned long long)logger->stats.write_errors,
           (unsigned long long)logger->stats.flushes,
           logger->stats.queue_high_watermark,
           logger->stats.file_failed ? "yes" : "no");
}
