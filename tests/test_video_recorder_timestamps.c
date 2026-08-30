#define _POSIX_C_SOURCE 200809L

#include "video_recorder.h"

#include "test_check.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_sentinel(const char *path, const char *value)
{
    FILE *file = fopen(path, "wb");

    TEST_CHECK(file != NULL);
    TEST_CHECK(fwrite(value, 1U, strlen(value), file) == strlen(value));
    TEST_CHECK(fclose(file) == 0);
}

static void assert_sentinel(const char *path, const char *value)
{
    char buffer[32] = { 0 };
    FILE *file = fopen(path, "rb");

    TEST_CHECK(file != NULL);
    TEST_CHECK(fread(buffer, 1U, sizeof(buffer), file) == strlen(value));
    TEST_CHECK(!ferror(file));
    TEST_CHECK(fclose(file) == 0);
    TEST_CHECK(memcmp(buffer, value, strlen(value)) == 0);
}

static uint32_t read_be32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | value[3];
}

static uint16_t read_be16(const uint8_t *value)
{
    return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
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
    uint8_t *data;
    const size_t data_capacity = 8U * 1024U * 1024U;
    size_t size;
    size_t index;
    const uint32_t expected_duration[] = { 4500U, 9000U, 3000U };
    bool found = false;
    bool found_square_pixel_pasp = false;
    bool found_tkhd_dimensions = false;
    bool found_mjpg_dimensions = false;
    char index_path[sizeof(path) + 4U];

    memset(&recorder, 0, sizeof(recorder));
    memset(&frame, 0, sizeof(frame));
    data = malloc(data_capacity);
    TEST_CHECK(data != NULL);
    (void)snprintf(path, sizeof(path), "/tmp/t113_recorder_%ld.mp4",
                   (long)getpid());
    TEST_CHECK(snprintf(index_path, sizeof(index_path), "%s.idx", path) > 0);

    write_sentinel(path, "existing-mp4");
    TEST_CHECK(video_recorder_init(&recorder, path, 16U, 16U, 30U) < 0);
    assert_sentinel(path, "existing-mp4");
    TEST_CHECK(access(index_path, F_OK) < 0 && errno == ENOENT);
    TEST_CHECK(unlink(path) == 0);

    write_sentinel(index_path, "existing-index");
    TEST_CHECK(video_recorder_init(&recorder, path, 16U, 16U, 30U) < 0);
    TEST_CHECK(access(path, F_OK) < 0 && errno == ENOENT);
    assert_sentinel(index_path, "existing-index");
    TEST_CHECK(unlink(index_path) == 0);

    TEST_CHECK(video_frame_init(&frame, 800U, 600U) == 0);
    memset(frame.pixels, 0x40, frame.capacity);
    TEST_CHECK(video_recorder_init(&recorder, path, 800U, 600U, 30U) == 0);
    frame.timestamp_us = 1000000U;
    TEST_CHECK(video_recorder_write_frame(&recorder, &frame));
    frame.timestamp_us = 1050000U;
    TEST_CHECK(video_recorder_write_frame(&recorder, &frame));
    frame.timestamp_us = 1150000U;
    TEST_CHECK(video_recorder_write_frame(&recorder, &frame));
    TEST_CHECK(video_recorder_finalize(&recorder) == 0);
    video_recorder_get_stats(&recorder, &recorder_stats);
    video_recorder_get_queue_stats(&recorder, &queue_stats);
    TEST_CHECK(recorder_stats.encoded_frames == 3U);
    TEST_CHECK(recorder_stats.enqueued_frames == 3U);
    TEST_CHECK(recorder_stats.written_frames == 3U);
    TEST_CHECK(recorder_stats.dropped_frames == 0U);
    TEST_CHECK(!recorder_stats.writer_failed);
    TEST_CHECK(recorder_stats.write_latency.samples == 3U);
    TEST_CHECK(recorder_stats.writer_write_calls == 5U);
    TEST_CHECK(recorder_stats.writer_write_call_bytes >
           recorder_stats.bytes_written);
    TEST_CHECK(queue_stats.current_depth == 0U);
    TEST_CHECK(queue_stats.pushed_frames == 3U && queue_stats.popped_frames == 3U);
    video_recorder_close(&recorder);
    video_frame_close(&frame);

    file = fopen(path, "rb");
    TEST_CHECK(file != NULL);
    size = fread(data, 1U, data_capacity, file);
    TEST_CHECK(!ferror(file));
    TEST_CHECK(fclose(file) == 0);
    TEST_CHECK(size > 40U);
    TEST_CHECK(read_be32(data + 24U) == 1U);
    TEST_CHECK(memcmp(data + 28U, "mdat", 4U) == 0);
    TEST_CHECK(read_be64(data + 32U) > 16U);
    for (index = 4U; index + 36U <= size; ++index) {
        size_t entry;

        if (memcmp(data + index, "stts", 4U) != 0 ||
            read_be32(data + index + 8U) != 3U) {
            continue;
        }
        for (entry = 0U; entry < 3U; ++entry) {
            TEST_CHECK(read_be32(data + index + 12U + entry * 8U) == 1U);
            TEST_CHECK(read_be32(data + index + 16U + entry * 8U) ==
                   expected_duration[entry]);
        }
        found = true;
        break;
    }
    TEST_CHECK(found);
    for (index = 4U; index + 84U <= size; ++index) {
        if (memcmp(data + index, "tkhd", 4U) == 0 &&
            read_be32(data + index + 80U) == (800U << 16) &&
            read_be32(data + index + 84U) == (600U << 16)) {
            found_tkhd_dimensions = true;
            break;
        }
    }
    TEST_CHECK(found_tkhd_dimensions);
    for (index = 4U; index + 30U <= size; ++index) {
        if (memcmp(data + index, "mjpg", 4U) == 0 &&
            read_be16(data + index + 28U) == 800U &&
            read_be16(data + index + 30U) == 600U) {
            found_mjpg_dimensions = true;
            break;
        }
    }
    TEST_CHECK(found_mjpg_dimensions);
    for (index = 4U; index + 12U <= size; ++index) {
        if (memcmp(data + index, "pasp", 4U) != 0) {
            continue;
        }
        TEST_CHECK(read_be32(data + index - 4U) == 16U);
        TEST_CHECK(read_be32(data + index + 4U) == 1U);
        TEST_CHECK(read_be32(data + index + 8U) == 1U);
        found_square_pixel_pasp = true;
        break;
    }
    TEST_CHECK(found_square_pixel_pasp);
    TEST_CHECK(access(index_path, F_OK) < 0 && errno == ENOENT);
    TEST_CHECK(unlink(path) == 0);
    free(data);
    return 0;
}
