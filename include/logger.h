#ifndef T113_LOGGER_H
#define T113_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>

#include "can_receiver.h"
#include "lappro.h"
#include "vehicle_state.h"

#define LOGGER_MOUNT_POINT "/mnt/usb/sda"
#define LOGGER_LOG_DIRECTORY "/mnt/usb/sda/log"
#define LOGGER_QUEUE_CAPACITY 256U
#define LOGGER_BATCH_SIZE 32U
#define LOGGER_FLUSH_INTERVAL_MS 1000U

typedef enum {
    LOG_RECORD_TYPE_CAN = 0,
    LOG_RECORD_TYPE_LAPPRO,
    LOG_RECORD_TYPE_VEHICLE_STATE,
    LOG_RECORD_TYPE_COUNT
} LogRecordType;

typedef struct {
    uint64_t timestamp_us;
    LogRecordType type;
    union {
        CanMessage can;
        LapProState lappro;
        VehicleState vehicle_state;
    } payload;
} LogRecord;

typedef struct {
    uint64_t enqueued_records;
    uint64_t written_records;
    uint64_t dropped_records;
    uint64_t write_errors;
    uint64_t flushes;
    size_t queue_high_watermark;
    bool file_failed[LOG_RECORD_TYPE_COUNT];
} LoggerStats;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    FILE *file[LOG_RECORD_TYPE_COUNT];
    char session_directory[256];
    char log_path[LOG_RECORD_TYPE_COUNT][256];
    LogRecord queue[LOGGER_QUEUE_CAPACITY];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    LoggerStats stats;
    bool initialized;
    bool started;
    bool stop_requested;
} Logger;

int logger_init(Logger *logger);

int logger_start(Logger *logger);

void logger_message_sink(const CanMessage *message, void *context);

void logger_record_sink(const LogRecord *record, void *context);

void logger_lappro_sink(uint64_t timestamp_us, const LapProState *state,
                        void *context);

void logger_vehicle_state_sink(uint64_t timestamp_us,
                               const VehicleState *state, void *context);

void logger_stop(Logger *logger);

const char *logger_get_path(const Logger *logger);

const char *logger_get_path_for_type(const Logger *logger,
                                     LogRecordType type);

const char *logger_get_session_directory(const Logger *logger);

void logger_print_stats(const Logger *logger);

#endif
