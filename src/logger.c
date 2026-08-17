#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "logger.h"
#include "console_log.h"

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
        LOG_WARN("Logger mount is missing or read-only: %s",
                 LOGGER_MOUNT_POINT);
        errno = ENODEV;
        return -1;
    }

    if (statvfs(LOGGER_MOUNT_POINT, &filesystem_status) < 0) {
        LOG_ERROR("Cannot stat logger mount %s: %s", LOGGER_MOUNT_POINT,
                  strerror(errno));
        return -1;
    }
    if ((filesystem_status.f_flag & ST_RDONLY) != 0) {
        LOG_WARN("Logger mount is read-only: %s", LOGGER_MOUNT_POINT);
        errno = EROFS;
        return -1;
    }

    if (mkdir(LOGGER_LOG_DIRECTORY, 0755) < 0 && errno != EEXIST) {
        LOG_ERROR("Cannot create logger directory %s: %s",
                  LOGGER_LOG_DIRECTORY, strerror(errno));
        return -1;
    }
    if (stat(LOGGER_LOG_DIRECTORY, &directory_status) < 0 ||
        !S_ISDIR(directory_status.st_mode)) {
        LOG_ERROR("Logger path is not a directory: %s",
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
                "# timestamp_us,bus,id,flags,dlc,data\n") < 0 ||
        fflush(logger->file) < 0) {
        return -1;
    }

    return 0;
}

int logger_init(Logger *logger)
{
    int condition_result;
    int attribute_result;
    pthread_condattr_t condition_attributes;

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
        LOG_ERROR("Cannot open logger file %s: %s", logger->log_path,
                  strerror(errno));
        return -1;
    }
    setvbuf(logger->file, NULL, _IOFBF, 16U * 1024U);
    if (logger_write_header(logger) < 0) {
        LOG_ERROR("Cannot write logger header %s: %s", logger->log_path,
                  strerror(errno));
        fclose(logger->file);
        logger->file = NULL;
        return -1;
    }

    if (pthread_mutex_init(&logger->mutex, NULL) != 0) {
        LOG_ERROR("Cannot initialize logger mutex");
        fclose(logger->file);
        logger->file = NULL;
        return -1;
    }
    attribute_result = pthread_condattr_init(&condition_attributes);
    if (attribute_result != 0) {
        LOG_ERROR("Cannot initialize logger condition attributes: %s",
                  strerror(attribute_result));
        pthread_mutex_destroy(&logger->mutex);
        fclose(logger->file);
        logger->file = NULL;
        return -1;
    }
    attribute_result = pthread_condattr_setclock(&condition_attributes,
                                                  CLOCK_MONOTONIC);
    if (attribute_result != 0) {
        LOG_ERROR("Cannot set logger condition clock: %s",
                  strerror(attribute_result));
        pthread_condattr_destroy(&condition_attributes);
        pthread_mutex_destroy(&logger->mutex);
        fclose(logger->file);
        logger->file = NULL;
        return -1;
    }
    condition_result = pthread_cond_init(&logger->condition,
                                         &condition_attributes);
    pthread_condattr_destroy(&condition_attributes);
    if (condition_result != 0) {
        LOG_ERROR("Cannot initialize logger condition: %s",
                  strerror(condition_result));
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

        if (fprintf(logger->file, "%llu,%u,0x%08x,0x%08x,%u,",
                    (unsigned long long)message->timestamp_us,
                    (unsigned int)message->bus,
                    message->id,
                    message->flags,
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

static void logger_make_deadline(uint64_t first_timestamp_us,
                                 struct timespec *deadline)
{
    if (first_timestamp_us == 0U) {
        if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) {
            deadline->tv_sec = 0;
            deadline->tv_nsec = 0;
        }
    } else {
        deadline->tv_sec = (time_t)(first_timestamp_us / 1000000ULL);
        deadline->tv_nsec = (long)(first_timestamp_us % 1000000ULL) * 1000L;
    }
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
        bool deadline_set = false;
        struct timespec deadline;

        pthread_mutex_lock(&logger->mutex);
        while (logger->queue_count < LOGGER_BATCH_SIZE &&
               !logger->stop_requested) {
            int wait_result;

            if (logger->queue_count == 0U) {
                wait_result = pthread_cond_wait(&logger->condition,
                                                &logger->mutex);
            } else {
                if (!deadline_set) {
                    logger_make_deadline(
                        logger->queue[logger->queue_head].timestamp_us,
                        &deadline);
                    deadline_set = true;
                }
                wait_result = pthread_cond_timedwait(&logger->condition,
                                                     &logger->mutex,
                                                     &deadline);
            }
            if (wait_result != 0 && wait_result != ETIMEDOUT) {
                LOG_ERROR("Logger condition wait failed: %s",
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
        LOG_ERROR("Cannot start logger thread: %s", strerror(result));
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
            LOG_ERROR("Cannot flush logger file %s: %s", logger->log_path,
                      strerror(errno));
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

    LOG_INFO("Logger statistics: path=%s enqueued_frames=%llu "
             "written_frames=%llu dropped_frames=%llu write_errors=%llu "
             "flushes=%llu queue_high_watermark=%zu file_failed=%s",
             logger->log_path,
             (unsigned long long)logger->stats.enqueued_frames,
             (unsigned long long)logger->stats.written_frames,
             (unsigned long long)logger->stats.dropped_frames,
             (unsigned long long)logger->stats.write_errors,
             (unsigned long long)logger->stats.flushes,
             logger->stats.queue_high_watermark,
             logger->stats.file_failed ? "yes" : "no");
}
