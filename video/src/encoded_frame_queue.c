#include "encoded_frame_queue.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int encoded_frame_queue_init(EncodedFrameQueue *queue, size_t capacity,
                             size_t slot_capacity)
{
    size_t index;
    int result;

    if (queue == NULL || capacity == 0U ||
        capacity > VIDEO_WRITER_QUEUE_CAPACITY || slot_capacity == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(queue, 0, sizeof(*queue));
    result = pthread_mutex_init(&queue->mutex, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    queue->mutex_initialized = true;
    result = pthread_cond_init(&queue->ready_condition, NULL);
    if (result != 0) {
        pthread_mutex_destroy(&queue->mutex);
        memset(queue, 0, sizeof(*queue));
        errno = result;
        return -1;
    }
    queue->condition_initialized = true;
    queue->capacity = capacity;
    queue->slot_capacity = slot_capacity;
    for (index = 0U; index < capacity; ++index) {
        queue->slots[index].storage = malloc(slot_capacity);
        if (queue->slots[index].storage == NULL) {
            encoded_frame_queue_destroy(queue);
            return -1;
        }
        queue->slots[index].state = ENCODED_FRAME_SLOT_FREE;
        queue->free_indices[index] = index;
    }
    queue->free_count = capacity;
    queue->initialized = true;
    return 0;
}

EncodedFrameQueuePushResult encoded_frame_queue_try_push(
    EncodedFrameQueue *queue, const EncodedFrame *frame)
{
    EncodedFrameSlot *slot;
    size_t slot_index;
    size_t depth;

    if (queue == NULL || frame == NULL || frame->data == NULL ||
        frame->size == 0U || !queue->initialized) {
        return ENCODED_FRAME_QUEUE_PUSH_ERROR;
    }
    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return ENCODED_FRAME_QUEUE_PUSH_ERROR;
    }
    if (queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return ENCODED_FRAME_QUEUE_PUSH_CLOSED;
    }
    if (frame->size > queue->slot_capacity) {
        queue->oversized_frames++;
        queue->dropped_frames++;
        pthread_mutex_unlock(&queue->mutex);
        return ENCODED_FRAME_QUEUE_PUSH_TOO_LARGE;
    }
    if (queue->free_count == 0U) {
        queue->full_events++;
        queue->dropped_frames++;
        pthread_mutex_unlock(&queue->mutex);
        return ENCODED_FRAME_QUEUE_PUSH_FULL;
    }
    slot_index = queue->free_indices[--queue->free_count];
    slot = &queue->slots[slot_index];
    memcpy(slot->storage, frame->data, frame->size);
    slot->frame = *frame;
    slot->frame.data = slot->storage;
    slot->frame.slot_index = slot_index;
    slot->state = ENCODED_FRAME_SLOT_READY;
    queue->ready_indices[queue->ready_tail] = slot_index;
    queue->ready_tail = (queue->ready_tail + 1U) % queue->capacity;
    queue->ready_count++;
    queue->pushed_frames++;
    queue->pushed_bytes += frame->size;
    depth = queue->capacity - queue->free_count;
    if (depth > queue->max_depth) {
        queue->max_depth = depth;
    }
    queue->depth_sample_total += depth;
    queue->depth_samples++;
    pthread_cond_signal(&queue->ready_condition);
    pthread_mutex_unlock(&queue->mutex);
    return ENCODED_FRAME_QUEUE_PUSH_OK;
}

bool encoded_frame_queue_pop(EncodedFrameQueue *queue, EncodedFrame *frame)
{
    EncodedFrameSlot *slot;
    size_t slot_index;
    int result;

    if (queue == NULL || frame == NULL || !queue->initialized ||
        pthread_mutex_lock(&queue->mutex) != 0) {
        return false;
    }
    while (queue->ready_count == 0U && !queue->closed) {
        result = pthread_cond_wait(&queue->ready_condition, &queue->mutex);
        if (result != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return false;
        }
    }
    if (queue->ready_count == 0U) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    slot_index = queue->ready_indices[queue->ready_head];
    queue->ready_head = (queue->ready_head + 1U) % queue->capacity;
    queue->ready_count--;
    slot = &queue->slots[slot_index];
    slot->state = ENCODED_FRAME_SLOT_IN_USE;
    *frame = slot->frame;
    queue->popped_frames++;
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

void encoded_frame_queue_release(EncodedFrameQueue *queue,
                                 const EncodedFrame *frame)
{
    EncodedFrameSlot *slot;

    if (queue == NULL || frame == NULL || !queue->initialized ||
        frame->slot_index >= queue->capacity ||
        pthread_mutex_lock(&queue->mutex) != 0) {
        return;
    }
    slot = &queue->slots[frame->slot_index];
    if (slot->state == ENCODED_FRAME_SLOT_IN_USE &&
        slot->storage == frame->data && queue->free_count < queue->capacity) {
        memset(&slot->frame, 0, sizeof(slot->frame));
        slot->state = ENCODED_FRAME_SLOT_FREE;
        queue->free_indices[queue->free_count++] = frame->slot_index;
    }
    pthread_mutex_unlock(&queue->mutex);
}

void encoded_frame_queue_close(EncodedFrameQueue *queue)
{
    if (queue == NULL || !queue->initialized ||
        pthread_mutex_lock(&queue->mutex) != 0) {
        return;
    }
    queue->closed = true;
    pthread_cond_broadcast(&queue->ready_condition);
    pthread_mutex_unlock(&queue->mutex);
}

void encoded_frame_queue_get_stats(EncodedFrameQueue *queue,
                                   EncodedFrameQueueStats *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (queue == NULL || !queue->initialized ||
        pthread_mutex_lock(&queue->mutex) != 0) {
        return;
    }
    stats->current_depth = queue->capacity - queue->free_count;
    stats->max_depth = queue->max_depth;
    stats->capacity = queue->capacity;
    stats->slot_capacity = queue->slot_capacity;
    stats->pushed_frames = queue->pushed_frames;
    stats->popped_frames = queue->popped_frames;
    stats->full_events = queue->full_events;
    stats->dropped_frames = queue->dropped_frames;
    stats->oversized_frames = queue->oversized_frames;
    stats->pushed_bytes = queue->pushed_bytes;
    stats->depth_sample_total = queue->depth_sample_total;
    stats->depth_samples = queue->depth_samples;
    stats->closed = queue->closed;
    pthread_mutex_unlock(&queue->mutex);
}

void encoded_frame_queue_destroy(EncodedFrameQueue *queue)
{
    size_t index;

    if (queue == NULL) {
        return;
    }
    if (queue->condition_initialized) {
        pthread_cond_destroy(&queue->ready_condition);
    }
    if (queue->mutex_initialized) {
        pthread_mutex_destroy(&queue->mutex);
    }
    for (index = 0U; index < VIDEO_WRITER_QUEUE_CAPACITY; ++index) {
        free(queue->slots[index].storage);
    }
    memset(queue, 0, sizeof(*queue));
}
