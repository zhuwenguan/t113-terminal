#define _POSIX_C_SOURCE 200809L

#include <race/video_ipc.h>

#include "console_log.h"
#include "jpeg_codec.h"
#include "video_frame.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static int parse_uint(const char *text, unsigned int *value)
{
    char *end;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > 1000U) {
        return -1;
    }
    *value = (unsigned int)parsed;
    return 0;
}

static int load_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file;
    long length;
    uint8_t *buffer;

    if (path == NULL || data == NULL || size == NULL) {
        errno = EINVAL;
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return -1;
    }
    length = ftell(file);
    if (length <= 0L || (uint64_t)length > RACE_VIDEO_IPC_MAX_JPEG_SIZE ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        errno = EOVERFLOW;
        return -1;
    }
    buffer = malloc((size_t)length);
    if (buffer == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(buffer, 1U, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(buffer);
        return -1;
    }
    if (fclose(file) != 0) {
        free(buffer);
        return -1;
    }
    if ((size_t)length < 4U || buffer[0] != 0xffU || buffer[1] != 0xd8U ||
        buffer[(size_t)length - 2U] != 0xffU ||
        buffer[(size_t)length - 1U] != 0xd9U) {
        free(buffer);
        errno = EPROTO;
        return -1;
    }
    *data = buffer;
    *size = (size_t)length;
    return 0;
}

static int generate_test_jpeg(uint32_t width, uint32_t height,
                              uint8_t **data, size_t *size)
{
    VideoFrame frame;
    JpegEncoder encoder;
    const uint8_t *encoded;
    uint32_t row;
    int result = -1;

    memset(&frame, 0, sizeof(frame));
    memset(&encoder, 0, sizeof(encoder));
    if (video_frame_init(&frame, width, height) < 0 ||
        jpeg_encoder_init(&encoder, width, RACE_VIDEO_IPC_MAX_JPEG_SIZE) < 0) {
        video_frame_close(&frame);
        return -1;
    }
    for (row = 0U; row < height; ++row) {
        uint32_t column;
        for (column = 0U; column < width; ++column) {
            uint8_t *pixel = frame.pixels + (size_t)row * frame.stride +
                             (size_t)column * VIDEO_FRAME_CHANNELS;
            pixel[0] = (uint8_t)((column * 255U) / width);
            pixel[1] = (uint8_t)((row * 255U) / height);
            pixel[2] = (uint8_t)(((column / 32U + row / 32U) & 1U) != 0U ?
                                 220U : 45U);
            pixel[3] = 255U;
        }
    }
    if (jpeg_encoder_encode_rgba(&encoder, &frame, 75, &encoded, size)) {
        *data = malloc(*size);
        if (*data != NULL) {
            memcpy(*data, encoded, *size);
            result = 0;
        }
    }
    jpeg_encoder_close(&encoder);
    video_frame_close(&frame);
    return result;
}

static void add_interval(struct timespec *deadline, uint64_t interval_us)
{
    deadline->tv_sec += (time_t)(interval_us / 1000000ULL);
    deadline->tv_nsec += (long)((interval_us % 1000000ULL) * 1000ULL);
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void add_rate_budget(uint64_t *budget, uint64_t delta_us,
                            unsigned int publish_fps)
{
    uint64_t increment;

    if (budget == NULL || publish_fps == 0U) {
        return;
    }
    increment = delta_us > UINT64_MAX / publish_fps ? UINT64_MAX :
                delta_us * publish_fps;
    if (*budget >= 2000000ULL || increment >= 2000000ULL - *budget) {
        *budget = 2000000ULL;
    } else {
        *budget += increment;
    }
}

int main(int argc, char **argv)
{
    const char *name = RACE_VIDEO_IPC_NAME;
    const char *jpeg_path = NULL;
    unsigned int source_fps = 30U;
    unsigned int publish_fps = 15U;
    unsigned int width = 800U;
    unsigned int height = 600U;
    unsigned int seconds = 0U;
    bool unlink_on_exit = false;
    uint8_t *jpeg_data = NULL;
    size_t jpeg_size = 0U;
    RaceVideoIpcHandle writer;
    struct sigaction action;
    struct timespec deadline;
    uint64_t rate_budget = 0U;
    uint64_t last_pts_us = 0U;
    uint64_t source_frames = 0U;
    uint64_t dropped = 0U;
    uint32_t source_sequence = 0U;
    uint64_t start_us;
    int index;

    memset(&writer, 0, sizeof(writer));
    writer.fd = -1;
    console_log_init();
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--name") == 0 && index + 1 < argc) {
            name = argv[++index];
        } else if (strcmp(argv[index], "--jpeg") == 0 && index + 1 < argc) {
            jpeg_path = argv[++index];
        } else if (strcmp(argv[index], "--source-fps") == 0 &&
                   index + 1 < argc) {
            if (parse_uint(argv[++index], &source_fps) < 0) {
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--publish-fps") == 0 &&
                   index + 1 < argc) {
            if (parse_uint(argv[++index], &publish_fps) < 0) {
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--width") == 0 && index + 1 < argc) {
            if (parse_uint(argv[++index], &width) < 0) {
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--height") == 0 && index + 1 < argc) {
            if (parse_uint(argv[++index], &height) < 0) {
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--seconds") == 0 &&
                   index + 1 < argc) {
            if (parse_uint(argv[++index], &seconds) < 0) {
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--unlink-on-exit") == 0) {
            unlink_on_exit = true;
        } else if (strcmp(argv[index], "--verbose") == 0 ||
                   strcmp(argv[index], "-v") == 0) {
            console_log_set_level(CONSOLE_LOG_DEBUG);
        } else {
            fprintf(stderr, "Usage: %s [--name SHM] [--jpeg FILE] "
                    "[--source-fps N] [--publish-fps N] [--width N] "
                    "[--height N] [--seconds N] [--unlink-on-exit]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (jpeg_path != NULL ? load_file(jpeg_path, &jpeg_data, &jpeg_size) < 0 :
                            generate_test_jpeg(width, height, &jpeg_data,
                                               &jpeg_size) < 0) {
        fprintf(stderr, "fake_video_producer: cannot prepare JPEG: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    if (race_video_ipc_writer_open(&writer, name) < 0) {
        fprintf(stderr, "fake_video_producer: cannot open %s: %s\n", name,
                strerror(errno));
        free(jpeg_data);
        return EXIT_FAILURE;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        race_video_ipc_close(&writer);
        free(jpeg_data);
        return EXIT_FAILURE;
    }
    start_us = monotonic_us();
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    while (!stop_requested &&
           (seconds == 0U || monotonic_us() < start_us +
                                      (uint64_t)seconds * 1000000ULL)) {
        uint64_t pts_us;
        bool publish;
        RaceVideoFrameInfo info;

        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        pts_us = monotonic_us();
        ++source_frames;
        publish = last_pts_us == 0U;
        if (!publish && pts_us >= last_pts_us) {
            uint64_t delta_us = pts_us - last_pts_us;
            add_rate_budget(&rate_budget, delta_us, publish_fps);
            publish = rate_budget >= 1000000ULL;
        }
        last_pts_us = pts_us;
        if (!publish) {
            ++dropped;
        } else {
            if (rate_budget >= 1000000ULL) {
                rate_budget -= 1000000ULL;
            }
            memset(&info, 0, sizeof(info));
            info.pts_us = pts_us;
            info.source_frame_count = source_frames;
            info.dropped_by_rate_limit = dropped;
            info.source_sequence = source_sequence;
            info.width = width;
            info.height = height;
            if (race_video_ipc_publish(&writer, jpeg_data, jpeg_size,
                                       &info) < 0) {
                LOG_WARN("fake video publish failed: %s", strerror(errno));
            }
        }
        ++source_sequence;
        add_interval(&deadline, 1000000ULL / source_fps);
        if (deadline.tv_sec * 1000000ULL +
                (uint64_t)deadline.tv_nsec / 1000ULL < pts_us) {
            (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
            add_interval(&deadline, 1000000ULL / source_fps);
        }
    }
    race_video_ipc_close(&writer);
    if (unlink_on_exit) {
        (void)shm_unlink(name);
    }
    LOG_INFO("fake video stopped: source=%llu published=%llu dropped=%llu",
             (unsigned long long)source_frames,
             (unsigned long long)(source_frames - dropped),
             (unsigned long long)dropped);
    free(jpeg_data);
    return EXIT_SUCCESS;
}
