#include <race/video_ipc.h>

#include "test_check.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const size_t TEST_JPEG_SIZE = 64U * 1024U;

static void producer(const char *name)
{
    RaceVideoIpcHandle writer;
    uint8_t *frame;
    unsigned int index;

    frame = malloc(TEST_JPEG_SIZE);
    if (frame == NULL) {
        _exit(2);
    }
    memset(&writer, 0, sizeof(writer));
    writer.fd = -1;
    if (race_video_ipc_writer_open(&writer, name) < 0) {
        free(frame);
        _exit(3);
    }
    for (index = 1U; index <= 2000U; ++index) {
        RaceVideoFrameInfo info;
        uint8_t pattern = (uint8_t)((index % 251U) + 1U);

        memset(frame, pattern, TEST_JPEG_SIZE);
        frame[0] = 0xffU;
        frame[1] = 0xd8U;
        frame[TEST_JPEG_SIZE - 2U] = 0xffU;
        frame[TEST_JPEG_SIZE - 1U] = 0xd9U;
        memset(&info, 0, sizeof(info));
        info.pts_us = (uint64_t)index * 33333U;
        info.source_frame_count = index;
        info.dropped_by_rate_limit = index / 2U;
        info.source_sequence = index;
        info.width = 800U;
        info.height = 600U;
        if (race_video_ipc_publish(&writer, frame, TEST_JPEG_SIZE, &info) < 0) {
            race_video_ipc_close(&writer);
            free(frame);
            _exit(4);
        }
    }
    race_video_ipc_close(&writer);
    free(frame);
    _exit(0);
}

int main(void)
{
    char name[64];
    RaceVideoIpcHandle reader;
    uint8_t *frame;
    pid_t child;
    int status;
    unsigned int open_attempts = 0U;
    unsigned int reads = 0U;
    unsigned int valid_reads = 0U;

    snprintf(name, sizeof(name), "/race_video_ipc_%ld", (long)getpid());
    (void)shm_unlink(name);
    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        producer(name);
    }
    memset(&reader, 0, sizeof(reader));
    reader.fd = -1;
    while (race_video_ipc_reader_open(&reader, name) < 0 &&
           open_attempts++ < 3000U) {
        struct timespec delay = {0, 1000000L};

        TEST_CHECK(errno == ENOENT || errno == EPROTO);
        (void)nanosleep(&delay, NULL);
    }
    TEST_CHECK(reader.mapping != NULL);
    frame = malloc(TEST_JPEG_SIZE);
    TEST_CHECK(frame != NULL);
    if (reader.mapping != NULL && frame != NULL) {
        while (reads++ < 10000U) {
            RaceVideoFrameInfo info;
            unsigned int byte;

            if (race_video_ipc_read_latest(&reader, frame, TEST_JPEG_SIZE,
                                           &info) < 0) {
                TEST_CHECK(errno == EAGAIN || errno == ENODATA);
                continue;
            }
            ++valid_reads;
            TEST_CHECK(info.width == 800U && info.height == 600U);
            TEST_CHECK(info.jpeg_size == TEST_JPEG_SIZE);
            TEST_CHECK(info.source_frame_count == info.source_sequence);
            TEST_CHECK(frame[0] == 0xffU && frame[1] == 0xd8U);
            TEST_CHECK(frame[TEST_JPEG_SIZE - 2U] == 0xffU &&
                       frame[TEST_JPEG_SIZE - 1U] == 0xd9U);
            for (byte = 2U; byte < TEST_JPEG_SIZE - 2U; ++byte) {
                TEST_CHECK(frame[byte] ==
                           (uint8_t)((info.source_sequence % 251U) + 1U));
            }
        }
    }
    free(frame);
    if (reader.mapping != NULL) {
        race_video_ipc_close(&reader);
    }
    TEST_CHECK(waitpid(child, &status, 0) == child);
    TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    TEST_CHECK(valid_reads > 100U);
    (void)shm_unlink(name);
    return 0;
}
