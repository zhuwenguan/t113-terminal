#ifndef T113_LOGGER_H
#define T113_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>

#include "can_receiver.h"

#define LOGGER_MOUNT_POINT "/mnt/usb"
#define LOGGER_LOG_DIRECTORY "/mnt/usb/log"
#define LOGGER_QUEUE_CAPACITY 256U
#define LOGGER_BATCH_SIZE 32U
#define LOGGER_FLUSH_INTERVAL_MS 1000U

typedef struct {
    uint64_t enqueued_frames;
    uint64_t written_frames;
    uint64_t dropped_frames;
    uint64_t write_errors;
    uint64_t flushes;
    size_t queue_high_watermark;
    bool file_failed;
} LoggerStats;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    FILE *file;
    char log_path[256];
    CanMessage queue[LOGGER_QUEUE_CAPACITY];
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

void logger_stop(Logger *logger);

const char *logger_get_path(const Logger *logger);

void logger_print_stats(const Logger *logger);

#endif
