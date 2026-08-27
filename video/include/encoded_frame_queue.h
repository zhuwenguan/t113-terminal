#ifndef T113_ENCODED_FRAME_QUEUE_H
#define T113_ENCODED_FRAME_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIDEO_WRITER_QUEUE_CAPACITY 32U
#define VIDEO_WRITER_SLOT_CAPACITY (384U * 1024U)

typedef struct {
    uint8_t *data;
    size_t size;
    uint64_t pts_us;
    uint64_t capture_ts_us;
    uint32_t v4l2_sequence;
    uint64_t frame_index;
    bool keyframe;
    size_t slot_index;
} EncodedFrame;

typedef enum {
    ENCODED_FRAME_QUEUE_PUSH_OK = 0,
    ENCODED_FRAME_QUEUE_PUSH_FULL,
    ENCODED_FRAME_QUEUE_PUSH_CLOSED,
    ENCODED_FRAME_QUEUE_PUSH_TOO_LARGE,
    ENCODED_FRAME_QUEUE_PUSH_ERROR
} EncodedFrameQueuePushResult;

typedef struct {
    size_t current_depth;
    size_t max_depth;
    size_t capacity;
    size_t slot_capacity;
    uint64_t pushed_frames;
    uint64_t popped_frames;
    uint64_t full_events;
    uint64_t dropped_frames;
    uint64_t oversized_frames;
    uint64_t pushed_bytes;
    uint64_t depth_sample_total;
    uint64_t depth_samples;
    bool closed;
} EncodedFrameQueueStats;

typedef enum {
    ENCODED_FRAME_SLOT_FREE = 0,
    ENCODED_FRAME_SLOT_READY,
    ENCODED_FRAME_SLOT_IN_USE
} EncodedFrameSlotState;

typedef struct {
    uint8_t *storage;
    EncodedFrame frame;
    EncodedFrameSlotState state;
} EncodedFrameSlot;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready_condition;
    EncodedFrameSlot slots[VIDEO_WRITER_QUEUE_CAPACITY];
    size_t free_indices[VIDEO_WRITER_QUEUE_CAPACITY];
    size_t ready_indices[VIDEO_WRITER_QUEUE_CAPACITY];
    size_t free_count;
    size_t ready_head;
    size_t ready_tail;
    size_t ready_count;
    size_t capacity;
    size_t slot_capacity;
    size_t max_depth;
    uint64_t pushed_frames;
    uint64_t popped_frames;
    uint64_t full_events;
    uint64_t dropped_frames;
    uint64_t oversized_frames;
    uint64_t pushed_bytes;
    uint64_t depth_sample_total;
    uint64_t depth_samples;
    bool closed;
    bool mutex_initialized;
    bool condition_initialized;
    bool initialized;
} EncodedFrameQueue;

int encoded_frame_queue_init(EncodedFrameQueue *queue, size_t capacity,
                             size_t slot_capacity);
EncodedFrameQueuePushResult encoded_frame_queue_try_push(
    EncodedFrameQueue *queue, const EncodedFrame *frame);
bool encoded_frame_queue_pop(EncodedFrameQueue *queue, EncodedFrame *frame);
void encoded_frame_queue_release(EncodedFrameQueue *queue,
                                 const EncodedFrame *frame);
void encoded_frame_queue_close(EncodedFrameQueue *queue);
void encoded_frame_queue_get_stats(EncodedFrameQueue *queue,
                                   EncodedFrameQueueStats *stats);
void encoded_frame_queue_destroy(EncodedFrameQueue *queue);

#endif
