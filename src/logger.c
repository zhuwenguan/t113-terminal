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

static int logger_make_paths(Logger *logger)
{
    static const char *const names[LOG_RECORD_TYPE_COUNT] = {
        "can", "lappro", "vehicle_state"
    };
    time_t current_time;
    struct tm local_time;
    char timestamp[32];
    unsigned int type;

    current_time = time(NULL);
    if (current_time == (time_t)-1 ||
        localtime_r(&current_time, &local_time) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S",
                 &local_time) == 0U) {
        errno = EINVAL;
        return -1;
    }

    for (type = 0; type < LOG_RECORD_TYPE_COUNT; ++type) {
        if (snprintf(logger->log_path[type],
                     sizeof(logger->log_path[type]),
                     "%s/%s_%s.csv", LOGGER_LOG_DIRECTORY, names[type],
                     timestamp) >=
            (int)sizeof(logger->log_path[type])) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    return 0;
}

static int logger_write_header(Logger *logger, LogRecordType type)
{
    struct stat file_status;

    if (fstat(fileno(logger->file[type]), &file_status) < 0) {
        return -1;
    }
    if (file_status.st_size != 0) {
        return 0;
    }

    if (type == LOG_RECORD_TYPE_CAN) {
        if (fprintf(logger->file[type],
                    "# t113 CAN log v1\n"
                    "# timestamp_us,bus,id,flags,dlc,data\n") < 0) {
            return -1;
        }
    } else if (type == LOG_RECORD_TYPE_LAPPRO) {
        if (fprintf(logger->file[type],
                    "# t113 LapPro parsed log v1\n"
                    "# timestamp_us,gps_time,connected,authenticated,"
                    "streaming,fix_type,num_sv,speed_kmh,heading_deg,hacc_m,"
                    "altitude,longitude,latitude,acc_x,acc_y,acc_z\n") < 0) {
            return -1;
        }
    } else if (type == LOG_RECORD_TYPE_VEHICLE_STATE) {
        if (fprintf(logger->file[type],
                    "# t113 Vehicle State log v1\n"
                    "# timestamp_us,valid_mask,lap_number,lap_valid,"
                    "lap_time_s,current_lap_time_s,last_lap_time_s,"
                    "best_lap_time_s,delta_time_s,estimated_lap_time_s,"
                    "track_distance_m,"
                    "sector_number,can_valid,gps_valid,gps_fix_type,gps_num_sv,"
                    "speed_kmh,heading_deg,hacc_m,altitude,longitude,latitude,"
                    "acc_x,acc_y,acc_z,"
                    "motor_rpm,torque_nm,hv_voltage_v,hv_current_a,hv_power_w,"
                    "hv_soc_percent,lv_voltage_v,event_mode,accel_distance_m,"
                    "accel_time_s,max_acceleration\n") < 0) {
            return -1;
        }
    } else {
        errno = EINVAL;
        return -1;
    }

    if (fflush(logger->file[type]) < 0) {
        return -1;
    }

    return 0;
}

static void logger_close_files(Logger *logger)
{
    unsigned int type;

    for (type = 0; type < LOG_RECORD_TYPE_COUNT; ++type) {
        if (logger->file[type] != NULL) {
            fclose(logger->file[type]);
            logger->file[type] = NULL;
        }
    }
}

int logger_init(Logger *logger)
{
    int condition_result;
    int attribute_result;
    pthread_condattr_t condition_attributes;
    unsigned int type;
    unsigned int opened_files = 0;

    if (logger == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(logger, 0, sizeof(*logger));
    if (logger_prepare_storage() < 0 || logger_make_paths(logger) < 0) {
        return -1;
    }

    for (type = 0; type < LOG_RECORD_TYPE_COUNT; ++type) {
        logger->file[type] = fopen(logger->log_path[type], "a");
        if (logger->file[type] == NULL) {
            logger->stats.file_failed[type] = true;
            LOG_ERROR("Cannot open logger file %s: %s",
                      logger->log_path[type], strerror(errno));
            continue;
        }
        setvbuf(logger->file[type], NULL, _IOFBF, 16U * 1024U);
        if (logger_write_header(logger, (LogRecordType)type) < 0) {
            logger->stats.file_failed[type] = true;
            LOG_ERROR("Cannot write logger header %s: %s",
                      logger->log_path[type], strerror(errno));
            fclose(logger->file[type]);
            logger->file[type] = NULL;
            continue;
        }
        opened_files++;
    }
    if (opened_files == 0U) {
        logger_close_files(logger);
        return -1;
    }

    if (pthread_mutex_init(&logger->mutex, NULL) != 0) {
        LOG_ERROR("Cannot initialize logger mutex");
        logger_close_files(logger);
        return -1;
    }
    attribute_result = pthread_condattr_init(&condition_attributes);
    if (attribute_result != 0) {
        LOG_ERROR("Cannot initialize logger condition attributes: %s",
                  strerror(attribute_result));
        pthread_mutex_destroy(&logger->mutex);
        logger_close_files(logger);
        return -1;
    }
    attribute_result = pthread_condattr_setclock(&condition_attributes,
                                                  CLOCK_MONOTONIC);
    if (attribute_result != 0) {
        LOG_ERROR("Cannot set logger condition clock: %s",
                  strerror(attribute_result));
        pthread_condattr_destroy(&condition_attributes);
        pthread_mutex_destroy(&logger->mutex);
        logger_close_files(logger);
        return -1;
    }
    condition_result = pthread_cond_init(&logger->condition,
                                         &condition_attributes);
    pthread_condattr_destroy(&condition_attributes);
    if (condition_result != 0) {
        LOG_ERROR("Cannot initialize logger condition: %s",
                  strerror(condition_result));
        pthread_mutex_destroy(&logger->mutex);
        logger_close_files(logger);
        return -1;
    }

    logger->initialized = true;
    return 0;
}

static int logger_write_hex(FILE *file, const uint8_t *data, size_t length)
{
    size_t index;

    for (index = 0; index < length; ++index) {
        if (fprintf(file, "%s%02x", index == 0U ? "" : " ",
                    data[index]) < 0) {
            return -1;
        }
    }

    return 0;
}

static int logger_write_can_record(FILE *file, const LogRecord *record)
{
    const CanMessage *message = &record->payload.can;

    if (fprintf(file, "%llu,%u,0x%08x,%u,%u,",
                (unsigned long long)record->timestamp_us,
                (unsigned int)message->bus, message->id, message->flags,
                (unsigned int)message->dlc) < 0 ||
        logger_write_hex(file, message->data, message->dlc) < 0 ||
        fputc('\n', file) == EOF) {
        return -1;
    }

    return 0;
}

static int logger_write_lappro_record(FILE *file, const LogRecord *record)
{
    const LapProState *state = &record->payload.lappro;
    char gps_time[32];

    if (state->gps_time_valid) {
        snprintf(gps_time, sizeof(gps_time),
                 "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
                 (unsigned int)state->gps_year,
                 (unsigned int)state->gps_month,
                 (unsigned int)state->gps_day,
                 (unsigned int)state->gps_hour,
                 (unsigned int)state->gps_minute,
                 (unsigned int)state->gps_second,
                 (unsigned int)state->gps_millisecond);
    } else {
        snprintf(gps_time, sizeof(gps_time), "n/a");
    }

    return fprintf(file,
                   "%llu,%s,%u,%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,"
                   "%.8f,%.8f,%.6f,%.6f,%.6f\n",
                   (unsigned long long)record->timestamp_us,
                   gps_time,
                   state->connected ? 1U : 0U,
                   state->authenticated ? 1U : 0U,
                   state->streaming ? 1U : 0U, (unsigned int)state->fix_type,
                   (unsigned int)state->num_sv, state->speed_kmh,
                   state->heading_deg, state->hacc_m, state->altitude,
                   state->longitude, state->latitude, state->acc_x,
                   state->acc_y, state->acc_z) < 0
               ? -1
               : 0;
}

static int logger_write_vehicle_state_record(FILE *file,
                                             const LogRecord *record)
{
    const VehicleState *state = &record->payload.vehicle_state;

    return fprintf(file,
                   "%llu,0x%08x,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,"
                   "%.6f,%.6f,%.6f,%.6f,%.8f,%.8f,%.6f,%.6f,%.6f,"
                   "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%.3f,%.3f,%.3f\n",
                   (unsigned long long)record->timestamp_us,
                   state->valid_mask,
                   (unsigned int)state->lap_number,
                   state->lap_valid ? 1U : 0U, state->lap_time_s,
                   state->current_lap_time_s, state->last_lap_time_s,
                   state->best_lap_time_s, state->delta_time_s,
                   state->estimated_lap_time_s, state->track_distance_m,
                   (unsigned int)state->sector_number,
                   state->can_valid ? 1U : 0U, state->gps_valid ? 1U : 0U,
                   (unsigned int)state->gps_fix_type,
                   (unsigned int)state->gps_num_sv, state->speed_kmh,
                   state->heading_deg, state->hacc_m, state->altitude,
                   state->longitude, state->latitude, state->acc_x, state->acc_y,
                   state->acc_z, state->motor_rpm, state->torque_nm,
                   state->hv_voltage_v, state->hv_current_a, state->hv_power_w,
                   state->hv_soc_percent, state->lv_voltage_v,
                   vehicle_state_event_mode_name(state->event_mode),
                   state->accel_distance_m, state->accel_time_s,
                   state->max_acceleration) < 0
               ? -1
               : 0;
}

static int logger_write_record(Logger *logger, const LogRecord *record)
{
    FILE *file;

    if ((int)record->type < 0 || record->type >= LOG_RECORD_TYPE_COUNT) {
        errno = EINVAL;
        return -1;
    }
    file = logger->file[record->type];

    if (record->type == LOG_RECORD_TYPE_CAN) {
        return logger_write_can_record(file, record);
    }
    if (record->type == LOG_RECORD_TYPE_LAPPRO) {
        return logger_write_lappro_record(file, record);
    }
    if (record->type == LOG_RECORD_TYPE_VEHICLE_STATE) {
        return logger_write_vehicle_state_record(file, record);
    }

    errno = EINVAL;
    return -1;
}

static void logger_write_batch(Logger *logger, const LogRecord *batch,
                               size_t count)
{
    bool touched[LOG_RECORD_TYPE_COUNT] = {false};
    size_t index;

    for (index = 0; index < count; ++index) {
        const LogRecord *record = &batch[index];

        if ((int)record->type < 0 || record->type >= LOG_RECORD_TYPE_COUNT ||
            logger->file[record->type] == NULL ||
            logger->stats.file_failed[record->type]) {
            logger->stats.dropped_records++;
            continue;
        }
        if (logger_write_record(logger, record) < 0) {
            logger->stats.write_errors++;
            logger->stats.file_failed[record->type] = true;
            logger->stats.dropped_records++;
            continue;
        }
        logger->stats.written_records++;
        touched[record->type] = true;
    }

    for (index = 0; index < LOG_RECORD_TYPE_COUNT; ++index) {
        if (!touched[index] || logger->stats.file_failed[index]) {
            continue;
        }
        if (fflush(logger->file[index]) < 0) {
            logger->stats.write_errors++;
            logger->stats.file_failed[index] = true;
        } else {
            logger->stats.flushes++;
        }
    }
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
        LogRecord batch[LOGGER_BATCH_SIZE];
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

void logger_record_sink(const LogRecord *record, void *context)
{
    Logger *logger = context;

    if (logger == NULL || record == NULL || !logger->initialized ||
        !logger->started) {
        return;
    }

    if (pthread_mutex_lock(&logger->mutex) != 0) {
        return;
    }

    if ((int)record->type < 0 || record->type >= LOG_RECORD_TYPE_COUNT ||
        (record->type == LOG_RECORD_TYPE_CAN &&
         record->payload.can.dlc > sizeof(record->payload.can.data)) ||
        logger->stop_requested || logger->queue_count >= LOGGER_QUEUE_CAPACITY) {
        logger->stats.dropped_records++;
        pthread_mutex_unlock(&logger->mutex);
        return;
    }

    logger->queue[logger->queue_tail] = *record;
    logger->queue_tail =
        (logger->queue_tail + 1U) % LOGGER_QUEUE_CAPACITY;
    logger->queue_count++;
    logger->stats.enqueued_records++;
    if (logger->queue_count > logger->stats.queue_high_watermark) {
        logger->stats.queue_high_watermark = logger->queue_count;
    }
    pthread_cond_signal(&logger->condition);
    pthread_mutex_unlock(&logger->mutex);
}

void logger_message_sink(const CanMessage *message, void *context)
{
    LogRecord record;

    if (message == NULL) {
        return;
    }

    memset(&record, 0, sizeof(record));
    record.timestamp_us = message->timestamp_us;
    record.type = LOG_RECORD_TYPE_CAN;
    record.payload.can = *message;
    logger_record_sink(&record, context);
}

void logger_lappro_sink(uint64_t timestamp_us, const LapProState *state,
                        void *context)
{
    LogRecord record;

    if (state == NULL) {
        return;
    }

    memset(&record, 0, sizeof(record));
    record.timestamp_us = timestamp_us;
    record.type = LOG_RECORD_TYPE_LAPPRO;
    record.payload.lappro = *state;
    logger_record_sink(&record, context);
}

void logger_vehicle_state_sink(uint64_t timestamp_us,
                               const VehicleState *state, void *context)
{
    LogRecord record;

    if (state == NULL) {
        return;
    }

    memset(&record, 0, sizeof(record));
    record.timestamp_us = timestamp_us;
    record.type = LOG_RECORD_TYPE_VEHICLE_STATE;
    record.payload.vehicle_state = *state;
    record.payload.vehicle_state.timestamp_us = timestamp_us;
    logger_record_sink(&record, context);
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

    {
        unsigned int type;

        for (type = 0; type < LOG_RECORD_TYPE_COUNT; ++type) {
            if (logger->file[type] == NULL) {
                continue;
            }
            if (!logger->stats.file_failed[type] &&
                fflush(logger->file[type]) < 0) {
                logger->stats.write_errors++;
                logger->stats.file_failed[type] = true;
                LOG_ERROR("Cannot flush logger file %s: %s",
                          logger->log_path[type], strerror(errno));
            }
            fclose(logger->file[type]);
            logger->file[type] = NULL;
        }
    }

    pthread_cond_destroy(&logger->condition);
    pthread_mutex_destroy(&logger->mutex);
    logger->initialized = false;
}

const char *logger_get_path(const Logger *logger)
{
    return logger_get_path_for_type(logger, LOG_RECORD_TYPE_CAN);
}

const char *logger_get_path_for_type(const Logger *logger, LogRecordType type)
{
    if (logger == NULL || (int)type < 0 || type >= LOG_RECORD_TYPE_COUNT) {
        return NULL;
    }
    return logger->log_path[type];
}

void logger_print_stats(const Logger *logger)
{
    if (logger == NULL) {
        return;
    }

    LOG_INFO("Logger statistics: can=%s lappro=%s vehicle_state=%s "
             "enqueued_records=%llu written_records=%llu "
             "dropped_records=%llu write_errors=%llu flushes=%llu "
             "queue_high_watermark=%zu file_failed=%s/%s/%s",
             logger->log_path[LOG_RECORD_TYPE_CAN],
             logger->log_path[LOG_RECORD_TYPE_LAPPRO],
             logger->log_path[LOG_RECORD_TYPE_VEHICLE_STATE],
             (unsigned long long)logger->stats.enqueued_records,
             (unsigned long long)logger->stats.written_records,
             (unsigned long long)logger->stats.dropped_records,
             (unsigned long long)logger->stats.write_errors,
             (unsigned long long)logger->stats.flushes,
             logger->stats.queue_high_watermark,
             logger->stats.file_failed[LOG_RECORD_TYPE_CAN] ? "yes" : "no",
             logger->stats.file_failed[LOG_RECORD_TYPE_LAPPRO] ? "yes" : "no",
             logger->stats.file_failed[LOG_RECORD_TYPE_VEHICLE_STATE]
                 ? "yes"
                 : "no");
}
