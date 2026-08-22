#define _POSIX_C_SOURCE 200809L

#include "cedar_vencoder_abi.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_WIDTH 1280U
#define TEST_HEIGHT 720U
#define TEST_BPP 4U
#define TEST_STRIDE (TEST_WIDTH * TEST_BPP)
#define TEST_FRAME_BYTES (TEST_STRIDE * TEST_HEIGHT)
#define TEST_QUALITY 82

typedef struct {
    CedarVencPixelFormat format;
    const char *name;
} RgbFormat;

typedef struct {
    void *library;
    void *jpeg_library;
    CedarVencCreate create;
    CedarVencDestroy destroy;
    CedarVencInit init;
    CedarVencAllocInputBuffer alloc_input;
    CedarVencGetInputBuffer get_input;
    CedarVencFlushInputBuffer flush_input;
    CedarVencReturnInputBuffer return_input;
    CedarVencAddInputBuffer add_input;
    CedarVencEncode encode;
    CedarVencUsedInputBuffer used_input;
    CedarVencGetOutputBuffer get_output;
    CedarVencFreeOutputBuffer free_output;
    CedarVencSetParameter set_parameter;
    CedarVencGetParameter get_parameter;
} EncoderApi;

typedef struct {
    uint64_t prepare_us;
    uint64_t submit_us;
    uint64_t hardware_encode_us;
    uint64_t acquire_us;
    uint64_t release_us;
    uint64_t total_us;
    uint64_t max_prepare_us;
    uint64_t max_submit_us;
    uint64_t max_hardware_encode_us;
    uint64_t max_acquire_us;
    uint64_t max_release_us;
    uint64_t max_total_us;
    uint64_t output_bytes;
    uint64_t frames;
} TimingStats;

static const RgbFormat RGB_FORMATS[] = {
    {CEDAR_VENC_PIXEL_ARGB, "ARGB"},
    {CEDAR_VENC_PIXEL_RGBA, "RGBA"},
    {CEDAR_VENC_PIXEL_ABGR, "ABGR"},
    {CEDAR_VENC_PIXEL_BGRA, "BGRA"},
};

static uint64_t now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0U;
    }
    return (uint64_t)timestamp.tv_sec * 1000000U +
           (uint64_t)timestamp.tv_nsec / 1000U;
}

static bool load_symbol(void *library, void *target, size_t target_size,
                        const char *name)
{
    void *symbol;

    symbol = dlsym(library, name);
    if (symbol == NULL || target_size != sizeof(symbol)) {
        fprintf(stderr, "missing symbol %s: %s\n", name, dlerror());
        return false;
    }
    memcpy(target, &symbol, target_size);
    return true;
}

static bool load_optional_symbol(void *library, void *target,
                                 size_t target_size, const char *name)
{
    void *symbol = dlsym(library, name);

    if (symbol == NULL) {
        memset(target, 0, target_size);
        return false;
    }
    memcpy(target, &symbol, target_size);
    return true;
}

static bool load_api(EncoderApi *api)
{
    memset(api, 0, sizeof(*api));
    api->library = dlopen("libvencoder.so", RTLD_NOW | RTLD_LOCAL);
    if (api->library == NULL) {
        fprintf(stderr, "dlopen libvencoder.so failed: %s\n", dlerror());
        return false;
    }
    api->jpeg_library = dlopen("libvenc_jpeg.so", RTLD_NOW | RTLD_LOCAL);
    if (api->jpeg_library == NULL) {
        fprintf(stderr, "dlopen libvenc_jpeg.so failed: %s\n", dlerror());
        return false;
    }
#define LOAD(member, symbol)                                                   \
    do {                                                                        \
        if (!load_symbol(api->library, &api->member, sizeof(api->member),       \
                         symbol)) {                                            \
            return false;                                                       \
        }                                                                       \
    } while (0)
    LOAD(create, "VideoEncCreate");
    LOAD(destroy, "VideoEncDestroy");
    LOAD(init, "VideoEncInit");
    LOAD(alloc_input, "AllocInputBuffer");
    LOAD(get_input, "GetOneAllocInputBuffer");
    LOAD(flush_input, "FlushCacheAllocInputBuffer");
    LOAD(return_input, "ReturnOneAllocInputBuffer");
    LOAD(add_input, "AddOneInputBuffer");
    LOAD(encode, "VideoEncodeOneFrame");
    LOAD(used_input, "AlreadyUsedInputBuffer");
    LOAD(get_output, "GetOneBitstreamFrame");
    LOAD(free_output, "FreeOneBitStreamFrame");
    LOAD(set_parameter, "VideoEncSetParameter");
#undef LOAD
    (void)load_optional_symbol(api->library, &api->get_parameter,
                               sizeof(api->get_parameter),
                               "VideoEncGetParameter");
    return true;
}

static void unload_api(EncoderApi *api)
{
    if (api->jpeg_library != NULL) {
        dlclose(api->jpeg_library);
    }
    if (api->library != NULL) {
        dlclose(api->library);
    }
    memset(api, 0, sizeof(*api));
}

static void put_pixel(uint8_t *pixel, CedarVencPixelFormat format,
                      uint8_t red, uint8_t green, uint8_t blue)
{
    switch (format) {
    case CEDAR_VENC_PIXEL_ARGB:
        pixel[0] = blue;
        pixel[1] = green;
        pixel[2] = red;
        pixel[3] = 0xffU;
        break;
    case CEDAR_VENC_PIXEL_RGBA:
        pixel[0] = 0xffU;
        pixel[1] = blue;
        pixel[2] = green;
        pixel[3] = red;
        break;
    case CEDAR_VENC_PIXEL_ABGR:
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
        pixel[3] = 0xffU;
        break;
    case CEDAR_VENC_PIXEL_BGRA:
        pixel[0] = 0xffU;
        pixel[1] = red;
        pixel[2] = green;
        pixel[3] = blue;
        break;
    default:
        memset(pixel, 0, TEST_BPP);
        break;
    }
}

static void fill_rgb_pattern(uint8_t *buffer, CedarVencPixelFormat format,
                             unsigned int frame)
{
    unsigned int row;
    unsigned int column;
    unsigned int phase = frame % 32U;

    for (row = 0U; row < TEST_HEIGHT; ++row) {
        for (column = 0U; column < TEST_WIDTH; ++column) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t *pixel = buffer + (size_t)row * TEST_STRIDE +
                             (size_t)column * TEST_BPP;

            if (row < TEST_HEIGHT / 2U && column < TEST_WIDTH / 2U) {
                red = 0xffU;
                green = 0x00U;
                blue = 0x00U;
            } else if (row < TEST_HEIGHT / 2U) {
                red = 0x00U;
                green = 0xffU;
                blue = 0x00U;
            } else if (column < TEST_WIDTH / 2U) {
                red = 0x00U;
                green = 0x00U;
                blue = 0xffU;
            } else {
                red = 0xffU;
                green = 0xffU;
                blue = 0xffU;
            }
            if (row > 300U && row < 420U && column > 500U &&
                column < 780U) {
                red = (uint8_t)(32U + (column + phase) % 224U);
                green = (uint8_t)(32U + (row + phase) % 224U);
                blue = (uint8_t)(255U - (column + row + phase) % 224U);
            }
            put_pixel(pixel, format, red, green, blue);
        }
    }
}

static bool write_jpeg(const char *path, const CedarVencOutputBuffer *output)
{
    FILE *file;
    bool success;

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return false;
    }
    success = fwrite(output->pData0, 1U, output->nSize0, file) ==
              output->nSize0;
    if (success && output->nSize1 > 0U) {
        success = output->pData1 != NULL &&
                  fwrite(output->pData1, 1U, output->nSize1, file) ==
                  output->nSize1;
    }
    if (fclose(file) != 0) {
        success = false;
    }
    return success;
}

static void query_color_format(const EncoderApi *api,
                               CedarVideoEncoder *encoder,
                               const RgbFormat *format)
{
    CedarVencCheckColorFormat check;
    int result;

    if (api->get_parameter == NULL) {
        printf("capability[%s]: VideoEncGetParameter unavailable\n",
               format->name);
        return;
    }
    memset(&check, 0, sizeof(check));
    check.index = (int)format->format;
    check.color_format = format->format;
    result = api->get_parameter(encoder,
                                CEDAR_VENC_INDEX_PARAM_CHECK_COLOR_FORMAT,
                                &check);
    printf("capability[%s]: query_index=0x%x request=%d result=%d "
           "returned_index=%d returned_format=%d\n", format->name,
           CEDAR_VENC_INDEX_PARAM_CHECK_COLOR_FORMAT, format->format, result,
           check.index, check.color_format);
}

static bool encode_frames(const EncoderApi *api, CedarVideoEncoder *encoder,
                          const RgbFormat *format, unsigned int frames,
                          const char *output_prefix, TimingStats *stats)
{
    unsigned int frame;
    bool success = true;

    for (frame = 0U; frame < frames; ++frame) {
        CedarVencInputBuffer input;
        CedarVencOutputBuffer output;
        uint64_t start = now_us();
        uint64_t stage;
        uint64_t prepare_us;
        uint64_t submit_us;
        uint64_t hardware_encode_us;
        uint64_t acquire_us;
        uint64_t release_us;
        uint64_t total_us;
        char path[512];
        const char *output_path = NULL;
        bool output_acquired = false;

        memset(&input, 0, sizeof(input));
        if (api->get_input(encoder, &input) != 0 || input.pAddrVirY == NULL) {
            fprintf(stderr, "[%s] input buffer unavailable at frame %u\n",
                    format->name, frame);
            return false;
        }
        stage = now_us();
        fill_rgb_pattern(input.pAddrVirY, format->format, frame);
        prepare_us = now_us() - stage;
        stats->prepare_us += prepare_us;
        if (frame == 0U) {
            printf("[%s] buffer: format=%d width=%u height=%u stride=%u "
                   "bytes=%u vir=%p phy=%p fd=%d\n", format->name,
                   format->format, TEST_WIDTH, TEST_HEIGHT, TEST_STRIDE,
                   TEST_FRAME_BYTES, (void *)input.pAddrVirY,
                   (void *)input.pAddrPhyY, input.nShareBufFd);
        }
        input.nWidth = (int)TEST_WIDTH;
        input.nHeight = (int)TEST_HEIGHT;
        input.nAlign = (int)TEST_STRIDE;
        input.nPts = (int64_t)start;
        stage = now_us();
        if (api->flush_input(encoder, &input) != 0 ||
            api->add_input(encoder, &input) != 0) {
            (void)api->return_input(encoder, &input);
            fprintf(stderr, "[%s] submit failed at frame %u\n",
                    format->name, frame);
            return false;
        }
        submit_us = now_us() - stage;
        stats->submit_us += submit_us;
        stage = now_us();
        if (api->encode(encoder) < 0) {
            (void)api->used_input(encoder, &input);
            (void)api->return_input(encoder, &input);
            fprintf(stderr, "[%s] encode failed at frame %u\n",
                    format->name, frame);
            return false;
        }
        hardware_encode_us = now_us() - stage;
        stats->hardware_encode_us += hardware_encode_us;
        (void)api->used_input(encoder, &input);
        (void)api->return_input(encoder, &input);

        memset(&output, 0, sizeof(output));
        stage = now_us();
        if (api->get_output(encoder, &output) != 0 || output.pData0 == NULL ||
            output.nSize0 == 0U) {
            fprintf(stderr, "[%s] output unavailable at frame %u\n",
                    format->name, frame);
            return false;
        }
        output_acquired = true;
        acquire_us = now_us() - stage;
        stats->acquire_us += acquire_us;
        stats->output_bytes += (uint64_t)output.nSize0 + output.nSize1;
        if (frames == 1U || frame == 0U || frame + 1U == frames) {
            int written = snprintf(path, sizeof(path), "%s_%s_%s.jpg",
                                   output_prefix, format->name,
                                   frames == 1U ? "single" :
                                   frame == 0U ? "first" : "last");
            if (written < 0 || (size_t)written >= sizeof(path)) {
                fprintf(stderr, "output path too long\n");
                goto frame_fail;
            }
            output_path = path;
            if (!write_jpeg(output_path, &output)) {
                goto frame_fail;
            }
        }
        stage = now_us();
        (void)api->free_output(encoder, &output);
        output_acquired = false;
        release_us = now_us() - stage;
        stats->release_us += release_us;
        total_us = now_us() - start;
        stats->total_us += total_us;
        if (prepare_us > stats->max_prepare_us) {
            stats->max_prepare_us = prepare_us;
        }
        if (submit_us > stats->max_submit_us) {
            stats->max_submit_us = submit_us;
        }
        if (hardware_encode_us > stats->max_hardware_encode_us) {
            stats->max_hardware_encode_us = hardware_encode_us;
        }
        if (acquire_us > stats->max_acquire_us) {
            stats->max_acquire_us = acquire_us;
        }
        if (release_us > stats->max_release_us) {
            stats->max_release_us = release_us;
        }
        if (total_us > stats->max_total_us) {
            stats->max_total_us = total_us;
        }
        stats->frames++;
        if (frames == 1U || frame < 2U || frame + 1U == frames ||
            frame % 100U == 0U) {
            printf("[%s] frame %u: JPEG=%u+%u bytes submit=%.2fms "
                   "encode=%.2fms acquire=%.2fms release=%.2fms "
                   "total=%.2fms\n", format->name, frame, output.nSize0,
                   output.nSize1, submit_us / 1000.0,
                   hardware_encode_us / 1000.0, acquire_us / 1000.0,
                   release_us / 1000.0, total_us / 1000.0);
        }
        continue;

frame_fail:
        if (output_acquired) {
            (void)api->free_output(encoder, &output);
        }
        success = false;
        break;
    }
    return success;
}

static bool test_format(const EncoderApi *api, const RgbFormat *format,
                        unsigned int frames, const char *output_prefix)
{
    CedarVideoEncoder *encoder;
    CedarVencBaseConfig config;
    CedarVencAllocateBufferParam buffer_param;
    TimingStats stats = {0};
    int quality = TEST_QUALITY;

    encoder = api->create(CEDAR_VENC_CODEC_JPEG);
    if (encoder == NULL) {
        fprintf(stderr, "[%s] VideoEncCreate failed\n", format->name);
        return false;
    }
    query_color_format(api, encoder, format);
    memset(&config, 0, sizeof(config));
    config.nInputWidth = TEST_WIDTH;
    config.nInputHeight = TEST_HEIGHT;
    config.nDstWidth = TEST_WIDTH;
    config.nDstHeight = TEST_HEIGHT;
    config.nStride = TEST_STRIDE;
    config.eInputFormat = format->format;
    if (api->set_parameter(encoder, 0x200, &quality) != 0 ||
        api->init(encoder, &config) != 0) {
        fprintf(stderr, "[%s] Init failed for format=%d\n", format->name,
                format->format);
        api->destroy(encoder);
        return false;
    }
    memset(&buffer_param, 0, sizeof(buffer_param));
    buffer_param.nBufferNum = 1U;
    buffer_param.nSizeY = TEST_FRAME_BYTES;
    buffer_param.nSizeC = 0U;
    printf("[%s] init succeeded: format=%d width=%u height=%u stride=%u "
           "buffer_y=%u buffer_c=%u\n", format->name, format->format,
           TEST_WIDTH, TEST_HEIGHT, TEST_STRIDE, buffer_param.nSizeY,
           buffer_param.nSizeC);
    if (api->alloc_input(encoder, &buffer_param) != 0) {
        fprintf(stderr, "[%s] AllocInputBuffer failed\n", format->name);
        api->destroy(encoder);
        return false;
    }
    if (!encode_frames(api, encoder, format, frames, output_prefix, &stats)) {
        api->destroy(encoder);
        return false;
    }
    printf("[%s] PASS: frames=%" PRIu64 " avg_prepare=%.2fms "
           "avg_submit=%.2fms avg_encode=%.2fms avg_acquire=%.2fms "
           "avg_release=%.2fms avg_total=%.2fms avg_jpeg=%.1fKB "
           "max_prepare=%.2fms max_submit=%.2fms max_encode=%.2fms "
           "max_acquire=%.2fms max_release=%.2fms max_total=%.2fms\n",
           format->name, stats.frames,
           (double)stats.prepare_us / stats.frames / 1000.0,
           (double)stats.submit_us / stats.frames / 1000.0,
           (double)stats.hardware_encode_us / stats.frames / 1000.0,
           (double)stats.acquire_us / stats.frames / 1000.0,
           (double)stats.release_us / stats.frames / 1000.0,
           (double)stats.total_us / stats.frames / 1000.0,
           (double)stats.output_bytes / stats.frames / 1024.0,
           stats.max_prepare_us / 1000.0, stats.max_submit_us / 1000.0,
           stats.max_hardware_encode_us / 1000.0,
           stats.max_acquire_us / 1000.0, stats.max_release_us / 1000.0,
           stats.max_total_us / 1000.0);
    api->destroy(encoder);
    return true;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--frames N] [--output-prefix PATH]\n", program);
    printf("  tests ABI-declared ARGB/RGBA/ABGR/BGRA at 1280x720\n");
    printf("  default: one frame per format, outputs under /tmp/rgb_test\n");
}

int main(int argc, char **argv)
{
    const char *output_prefix = "/tmp/rgb_test";
    unsigned int frames = 1U;
    EncoderApi api;
    unsigned int index;
    unsigned int successful_formats = 0U;

    for (index = 1U; index < (unsigned int)argc; ++index) {
        if (strcmp(argv[index], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[index], "--frames") == 0 && index + 1U <
            (unsigned int)argc) {
            frames = (unsigned int)strtoul(argv[++index], NULL, 10);
            if (frames == 0U) {
                fprintf(stderr, "--frames must be positive\n");
                return EXIT_FAILURE;
            }
            continue;
        }
        if (strcmp(argv[index], "--output-prefix") == 0 && index + 1U <
            (unsigned int)argc) {
            output_prefix = argv[++index];
            continue;
        }
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    printf("vencoder RGB test: input=RGB 1280x720 stride=%u bytes=%u "
           "frames=%u\n", TEST_STRIDE, TEST_FRAME_BYTES, frames);
    if (!load_api(&api)) {
        unload_api(&api);
        return EXIT_FAILURE;
    }
    for (index = 0U; index < sizeof(RGB_FORMATS) / sizeof(RGB_FORMATS[0]);
         ++index) {
        if (test_format(&api, &RGB_FORMATS[index], frames, output_prefix)) {
            successful_formats++;
        }
    }
    unload_api(&api);
    printf("summary: %u/%zu RGB formats encoded successfully\n",
           successful_formats, sizeof(RGB_FORMATS) / sizeof(RGB_FORMATS[0]));
    return successful_formats > 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
