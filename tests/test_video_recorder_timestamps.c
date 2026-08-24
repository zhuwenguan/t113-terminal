#define _POSIX_C_SOURCE 200809L

#include "video_recorder.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_sentinel(const char *path, const char *value)
{
    FILE *file = fopen(path, "wb");

    assert(file != NULL);
    assert(fwrite(value, 1U, strlen(value), file) == strlen(value));
    assert(fclose(file) == 0);
}

static void assert_sentinel(const char *path, const char *value)
{
    char buffer[32] = { 0 };
    FILE *file = fopen(path, "rb");

    assert(file != NULL);
    assert(fread(buffer, 1U, sizeof(buffer), file) == strlen(value));
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(memcmp(buffer, value, strlen(value)) == 0);
}

static uint32_t read_be32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | value[3];
}

static uint64_t read_be64(const uint8_t *value)
{
    return ((uint64_t)read_be32(value) << 32) | read_be32(value + 4U);
}

int main(void)
{
    char path[160];
    VideoRecorder recorder;
    VideoRecorderStats recorder_stats;
    EncodedFrameQueueStats queue_stats;
    VideoFrame frame;
    FILE *file;
    uint8_t data[1024 * 1024];
    size_t size;
    size_t index;
    const uint32_t expected_duration[] = { 4500U, 9000U, 3000U };
    bool found = false;
    char index_path[sizeof(path) + 4U];

    memset(&recorder, 0, sizeof(recorder));
    memset(&frame, 0, sizeof(frame));
    (void)snprintf(path, sizeof(path), "/tmp/t113_recorder_%ld.mp4",
                   (long)getpid());
    assert(snprintf(index_path, sizeof(index_path), "%s.idx", path) > 0);

    write_sentinel(path, "existing-mp4");
    assert(video_recorder_init(&recorder, path, 16U, 16U, 30U) < 0);
    assert_sentinel(path, "existing-mp4");
    assert(access(index_path, F_OK) < 0 && errno == ENOENT);
    assert(unlink(path) == 0);

    write_sentinel(index_path, "existing-index");
    assert(video_recorder_init(&recorder, path, 16U, 16U, 30U) < 0);
    assert(access(path, F_OK) < 0 && errno == ENOENT);
    assert_sentinel(index_path, "existing-index");
    assert(unlink(index_path) == 0);

    assert(video_frame_init(&frame, 16U, 16U) == 0);
    memset(frame.pixels, 0x40, frame.capacity);
    assert(video_recorder_init(&recorder, path, 16U, 16U, 30U) == 0);
    frame.timestamp_us = 1000000U;
    assert(video_recorder_write_frame(&recorder, &frame));
    frame.timestamp_us = 1050000U;
    assert(video_recorder_write_frame(&recorder, &frame));
    frame.timestamp_us = 1150000U;
    assert(video_recorder_write_frame(&recorder, &frame));
    assert(video_recorder_finalize(&recorder) == 0);
    video_recorder_get_stats(&recorder, &recorder_stats);
    video_recorder_get_queue_stats(&recorder, &queue_stats);
    assert(recorder_stats.encoded_frames == 3U);
    assert(recorder_stats.enqueued_frames == 3U);
    assert(recorder_stats.written_frames == 3U);
    assert(recorder_stats.dropped_frames == 0U);
    assert(!recorder_stats.writer_failed);
    assert(recorder_stats.write_latency.samples == 3U);
    assert(recorder_stats.writer_write_calls == 5U);
    assert(recorder_stats.writer_write_call_bytes >
           recorder_stats.bytes_written);
    assert(queue_stats.current_depth == 0U);
    assert(queue_stats.pushed_frames == 3U && queue_stats.popped_frames == 3U);
    video_recorder_close(&recorder);
    video_frame_close(&frame);

    file = fopen(path, "rb");
    assert(file != NULL);
    size = fread(data, 1U, sizeof(data), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(size > 40U);
    assert(read_be32(data + 24U) == 1U);
    assert(memcmp(data + 28U, "mdat", 4U) == 0);
    assert(read_be64(data + 32U) > 16U);
    for (index = 4U; index + 36U <= size; ++index) {
        size_t entry;

        if (memcmp(data + index, "stts", 4U) != 0 ||
            read_be32(data + index + 8U) != 3U) {
            continue;
        }
        for (entry = 0U; entry < 3U; ++entry) {
            assert(read_be32(data + index + 12U + entry * 8U) == 1U);
            assert(read_be32(data + index + 16U + entry * 8U) ==
                   expected_duration[entry]);
        }
        found = true;
        break;
    }
    assert(found);
    assert(access(index_path, F_OK) < 0 && errno == ENOENT);
    assert(unlink(path) == 0);
    return 0;
}
