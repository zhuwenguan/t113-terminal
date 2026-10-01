#ifndef T113_LOGGER_H
#define T113_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>

#include "can_receiver.h"
#include "lappro.h"
#include <race/vehicle_state.h>

#define LOGGER_MOUNT_POINT "/mnt/usb/sda"
#define LOGGER_LOG_DIRECTORY "/mnt/usb/sda/log"
#define LOGGER_QUEUE_CAPACITY 1024U
#define LOGGER_BATCH_SIZE 32U
#define LOGGER_FLUSH_INTERVAL_MS 1000U
/* Keep user-space buffers current, but avoid forcing the USB device per batch. */
#define LOGGER_DURABILITY_INTERVAL_MS 5000U
/* Keep room for the lower-rate records while raw CAN traffic is busy. */
#define LOGGER_CRITICAL_QUEUE_RESERVE LOGGER_BATCH_SIZE

typedef enum {
    LOG_RECORD_TYPE_CAN = 0,
    LOG_RECORD_TYPE_LAPPRO,
    LOG_RECORD_TYPE_VEHICLE_STATE,
    LOG_RECORD_TYPE_COUNT
} LogRecordType;

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_us;
    LogRecordType type;
    union {
        CanMessage can;
        LapProState lappro;
        VehicleState vehicle_state;
    } payload;
} LogRecord;

typedef struct {
    uint64_t enqueued;
    uint64_t written;
    uint64_t dropped;
    uint64_t write_errors;
} LoggerTypeStats;

typedef struct {
    uint64_t enqueued_records;
    uint64_t written_records;
    uint64_t dropped_records;
    uint64_t write_errors;
    uint64_t flushes;
    uint64_t durability_syncs;
    uint64_t durability_sync_deferred;
    uint64_t can_reserve_drops;
    size_t queue_high_watermark;
    LoggerTypeStats by_type[LOG_RECORD_TYPE_COUNT];
    bool file_failed[LOG_RECORD_TYPE_COUNT];
} LoggerStats;

typedef struct {
    pthread_mutex_t mutex;
    pthread_mutex_t file_mutex;
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
    uint64_t next_sequence;
    uint64_t last_durability_sync_us;
    bool sync_pending[LOG_RECORD_TYPE_COUNT];
    bool file_mutex_initialized;
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

/* Hook for a future power-good/lv-off interrupt. It stops the writer after
 * draining the queue and synchronizes all files. */
int logger_force_flush(Logger *logger);

bool logger_is_healthy(const Logger *logger);

const char *logger_get_path(const Logger *logger);

const char *logger_get_path_for_type(const Logger *logger,
                                     LogRecordType type);

const char *logger_get_session_directory(const Logger *logger);

void logger_print_stats(const Logger *logger);

#endif
