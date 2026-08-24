#define _POSIX_C_SOURCE 200809L

#include "video_recorder.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t read_be32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | value[3];
}

int main(void)
{
    char path[160];
    VideoRecorder recorder;
    VideoFrame frame;
    FILE *file;
    uint8_t data[1024 * 1024];
    size_t size;
    size_t index;
    const uint32_t expected_duration[] = { 4500U, 9000U, 3000U };
    bool found = false;

    memset(&recorder, 0, sizeof(recorder));
    memset(&frame, 0, sizeof(frame));
    (void)snprintf(path, sizeof(path), "/tmp/t113_recorder_%ld.mp4",
                   (long)getpid());
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
    video_recorder_close(&recorder);
    video_frame_close(&frame);

    file = fopen(path, "rb");
    assert(file != NULL);
    size = fread(data, 1U, sizeof(data), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
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
    assert(unlink(path) == 0);
    return 0;
}
