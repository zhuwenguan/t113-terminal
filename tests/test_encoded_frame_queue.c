#define _POSIX_C_SOURCE 200809L

#include "encoded_frame_queue.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    EncodedFrameQueue *queue;
    bool popped;
} PopThreadContext;

static void *pop_until_closed(void *opaque)
{
    PopThreadContext *context = opaque;
    EncodedFrame frame;

    memset(&frame, 0, sizeof(frame));
    context->popped = encoded_frame_queue_pop(context->queue, &frame);
    return NULL;
}

static void test_order_metadata_and_owned_copy(void)
{
    EncodedFrameQueue queue;
    EncodedFrame source;
    EncodedFrame output;
    uint8_t first[] = { 1U, 2U, 3U, 4U };
    uint8_t second[] = { 5U, 6U, 7U };

    memset(&queue, 0, sizeof(queue));
    memset(&source, 0, sizeof(source));
    assert(encoded_frame_queue_init(&queue, 2U, 16U) == 0);
    source.data = first;
    source.size = sizeof(first);
    source.pts_us = 1000U;
    source.capture_ts_us = 900U;
    source.v4l2_sequence = 41U;
    source.frame_index = 7U;
    source.keyframe = true;
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_OK);
    memset(first, 0xff, sizeof(first));
    source.data = second;
    source.size = sizeof(second);
    source.pts_us = 2000U;
    source.v4l2_sequence = 42U;
    source.frame_index = 8U;
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_OK);

    memset(&output, 0, sizeof(output));
    assert(encoded_frame_queue_pop(&queue, &output));
    assert(output.size == 4U && output.data[0] == 1U && output.data[3] == 4U);
    assert(output.pts_us == 1000U && output.capture_ts_us == 900U);
    assert(output.v4l2_sequence == 41U && output.frame_index == 7U);
    assert(output.keyframe);
    encoded_frame_queue_release(&queue, &output);
    assert(encoded_frame_queue_pop(&queue, &output));
    assert(output.size == 3U && memcmp(output.data, second, 3U) == 0);
    assert(output.pts_us == 2000U && output.v4l2_sequence == 42U);
    encoded_frame_queue_release(&queue, &output);
    encoded_frame_queue_close(&queue);
    assert(!encoded_frame_queue_pop(&queue, &output));
    encoded_frame_queue_destroy(&queue);
}

static void test_bounded_drop_and_reuse(void)
{
    EncodedFrameQueue queue;
    EncodedFrame source;
    EncodedFrame output;
    EncodedFrameQueueStats stats;
    uint8_t data[9] = { 0U };

    memset(&queue, 0, sizeof(queue));
    memset(&source, 0, sizeof(source));
    assert(encoded_frame_queue_init(&queue, 2U, 8U) == 0);
    source.data = data;
    source.size = 8U;
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_OK);
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_OK);
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_FULL);
    source.size = sizeof(data);
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_TOO_LARGE);
    encoded_frame_queue_get_stats(&queue, &stats);
    assert(stats.current_depth == 2U && stats.max_depth == 2U);
    assert(stats.capacity == 2U && stats.slot_capacity == 8U);
    assert(stats.full_events == 1U && stats.oversized_frames == 1U);
    assert(stats.dropped_frames == 2U);
    assert(encoded_frame_queue_pop(&queue, &output));
    encoded_frame_queue_release(&queue, &output);
    source.size = 8U;
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_OK);
    encoded_frame_queue_close(&queue);
    assert(encoded_frame_queue_try_push(&queue, &source) ==
           ENCODED_FRAME_QUEUE_PUSH_CLOSED);
    while (encoded_frame_queue_pop(&queue, &output)) {
        encoded_frame_queue_release(&queue, &output);
    }
    encoded_frame_queue_destroy(&queue);
}

static void test_close_wakes_waiter(void)
{
    EncodedFrameQueue queue;
    PopThreadContext context;
    pthread_t thread;

    memset(&queue, 0, sizeof(queue));
    memset(&context, 0, sizeof(context));
    assert(encoded_frame_queue_init(&queue, 1U, 8U) == 0);
    context.queue = &queue;
    assert(pthread_create(&thread, NULL, pop_until_closed, &context) == 0);
    encoded_frame_queue_close(&queue);
    assert(pthread_join(thread, NULL) == 0);
    assert(!context.popped);
    encoded_frame_queue_destroy(&queue);
}

int main(void)
{
    test_order_metadata_and_owned_copy();
    test_bounded_drop_and_reuse();
    test_close_wakes_waiter();
    return 0;
}
