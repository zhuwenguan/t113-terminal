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
#define TEST_STRIDE TEST_WIDTH
#define TEST_ALIGNED_HEIGHT TEST_HEIGHT
#define TEST_OVERLAY_WIDTH 208U
#define TEST_OVERLAY_HEIGHT 128U
#define TEST_OVERLAY_X 96U
#define TEST_OVERLAY_Y 96U
#define TEST_QUALITY 82

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
} EncoderApi;

typedef struct {
    uint64_t overlay_update_us;
    uint64_t submit_us;
    uint64_t hardware_encode_us;
    uint64_t acquire_us;
    uint64_t release_us;
    uint64_t total_us;
    uint64_t output_bytes;
} TimingStats;

static uint64_t now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0U;
    }
    return (uint64_t)timestamp.tv_sec * 1000000U +
           (uint64_t)timestamp.tv_nsec / 1000U;
}

static uint64_t max_u64(uint64_t left, uint64_t right)
{
    return left > right ? left : right;
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

static void fill_gray_yuv(uint8_t *y, uint8_t *c)
{
    memset(y, 128, TEST_STRIDE * TEST_ALIGNED_HEIGHT);
    memset(c, 128, TEST_STRIDE * TEST_ALIGNED_HEIGHT / 2U);
}

static void fill_overlay(uint8_t *bitmap, unsigned int frame)
{
    unsigned int row;
    unsigned int column;
    uint8_t red = (frame & 1U) == 0U ? 0xffU : 0x00U;
    uint8_t green = (frame & 1U) == 0U ? 0x00U : 0xffU;

    for (row = 0U; row < TEST_OVERLAY_HEIGHT; ++row) {
        for (column = 0U; column < TEST_OVERLAY_WIDTH; ++column) {
            uint8_t *pixel = bitmap +
                ((size_t)row * TEST_OVERLAY_WIDTH + column) * 4U;

            /* Little-endian ARGB8888: B, G, R, A in memory. */
            pixel[0] = 0x00U;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = 0x80U;
        }
    }
}

static void prepare_overlay(CedarVencOverlayInfo *info, uint8_t *bitmap,
                            unsigned int frame)
{
    unsigned int x_mb = TEST_OVERLAY_X / 16U + (frame % 4U);
    unsigned int y_mb = TEST_OVERLAY_Y / 16U;
    CedarVencOverlayHeader *header = &info->overlay_header_list[0];

    memset(info, 0, sizeof(*info));
    info->blk_num = 1U;
    info->argb_type = CEDAR_VENC_OVERLAY_ARGB8888;
    header->start_mb_x = (uint16_t)x_mb;
    header->end_mb_x = (uint16_t)(x_mb + TEST_OVERLAY_WIDTH / 16U - 1U);
    header->start_mb_y = (uint16_t)y_mb;
    header->end_mb_y = (uint16_t)(y_mb + TEST_OVERLAY_HEIGHT / 16U - 1U);
    header->extra_alpha_flag = 0U;
    header->extra_alpha = 0U;
    header->overlay_type = CEDAR_VENC_NORMAL_OVERLAY;
    header->overlay_blk_addr = bitmap;
    header->bitmap_size = TEST_OVERLAY_WIDTH * TEST_OVERLAY_HEIGHT * 4U;
}

static bool write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    bool success;

    if (file == NULL) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return false;
    }
    success = fwrite(data, 1U, size, file) == size && fclose(file) == 0;
    if (!success) {
        fprintf(stderr, "write %s failed\n", path);
    }
    return success;
}

static bool encode_one(EncoderApi *api, CedarVideoEncoder *encoder,
                       uint8_t *y, uint8_t *c, const char *output_path,
                       uint64_t *overlay_update_us, TimingStats *timing,
                       unsigned int frame)
{
    CedarVencInputBuffer input;
    CedarVencOutputBuffer output;
    CedarVencOverlayInfo overlay;
    uint8_t *bitmap;
    uint64_t start;
    uint64_t stage;
    size_t output_size;
    int set_overlay_result;
    bool output_acquired = false;

    bitmap = aligned_alloc(16U, TEST_OVERLAY_WIDTH * TEST_OVERLAY_HEIGHT * 4U);
    if (bitmap == NULL) {
        fprintf(stderr, "overlay bitmap allocation failed\n");
        return false;
    }
    start = now_us();
    fill_overlay(bitmap, frame);
    prepare_overlay(&overlay, bitmap, frame);
    stage = now_us();
    set_overlay_result = api->set_parameter(
        encoder, CEDAR_VENC_INDEX_PARAM_SET_OVERLAY, &overlay);
    if (set_overlay_result != 0) {
        fprintf(stderr, "SetOverlay failed: result=%d index=0x%x frame=%u "
                "errno=%d (%s)\n", set_overlay_result,
                CEDAR_VENC_INDEX_PARAM_SET_OVERLAY, frame, errno,
                strerror(errno));
        free(bitmap);
        return false;
    }
    *overlay_update_us = now_us() - start;
    printf("overlay[%u]: enable=1 index=0x%x type=%d format=ARGB8888 "
           "bitmap=%ux%u stride=%u bytes=%u x=%u y=%u alpha=128 "
           "blocks=%u buffer=%p fd=-1 physical=unknown update=%" PRIu64
           "us\n", frame, CEDAR_VENC_INDEX_PARAM_SET_OVERLAY,
           overlay.overlay_header_list[0].overlay_type,
           TEST_OVERLAY_WIDTH, TEST_OVERLAY_HEIGHT, TEST_OVERLAY_WIDTH * 4U,
           overlay.overlay_header_list[0].bitmap_size,
           overlay.overlay_header_list[0].start_mb_x * 16U,
           overlay.overlay_header_list[0].start_mb_y * 16U,
           overlay.blk_num, (void *)bitmap, *overlay_update_us);

    memset(&input, 0, sizeof(input));
    if (api->get_input(encoder, &input) != 0 || input.pAddrVirY == NULL ||
        input.pAddrVirC == NULL) {
        fprintf(stderr, "GetOneAllocInputBuffer failed at frame %u\n", frame);
        free(bitmap);
        return false;
    }
    memcpy(input.pAddrVirY, y, TEST_STRIDE * TEST_ALIGNED_HEIGHT);
    memcpy(input.pAddrVirC, c, TEST_STRIDE * TEST_ALIGNED_HEIGHT / 2U);
    input.nWidth = (int)TEST_WIDTH;
    input.nHeight = (int)TEST_HEIGHT;
    input.nAlign = (int)TEST_STRIDE;
    input.nPts = (int64_t)start;
    stage = now_us();
    if (api->flush_input(encoder, &input) != 0 ||
        api->add_input(encoder, &input) != 0) {
        (void)api->return_input(encoder, &input);
        fprintf(stderr, "submit failed at frame %u\n", frame);
        free(bitmap);
        return false;
    }
    timing->submit_us = now_us() - stage;
    stage = now_us();
    if (api->encode(encoder) < 0) {
        (void)api->used_input(encoder, &input);
        (void)api->return_input(encoder, &input);
        fprintf(stderr, "VideoEncodeOneFrame failed at frame %u\n", frame);
        free(bitmap);
        return false;
    }
    timing->hardware_encode_us = now_us() - stage;
    (void)api->used_input(encoder, &input);
    (void)api->return_input(encoder, &input);

    memset(&output, 0, sizeof(output));
    stage = now_us();
    if (api->get_output(encoder, &output) != 0 || output.pData0 == NULL ||
        output.nSize0 == 0U) {
        fprintf(stderr, "GetOneBitstreamFrame failed at frame %u\n", frame);
        free(bitmap);
        return false;
    }
    output_acquired = true;
    timing->acquire_us = now_us() - stage;
    output_size = (size_t)output.nSize0 + output.nSize1;
    if (output.nSize1 > 0U && output.pData1 == NULL) {
        fprintf(stderr, "invalid second bitstream plane at frame %u\n", frame);
        goto fail_output;
    }
    if (output_path != NULL) {
        uint8_t *jpeg = malloc(output_size);
        bool written = false;

        if (jpeg == NULL) {
            fprintf(stderr, "JPEG output allocation failed\n");
            goto fail_output;
        }
        memcpy(jpeg, output.pData0, output.nSize0);
        if (output.nSize1 > 0U) {
            memcpy(jpeg + output.nSize0, output.pData1, output.nSize1);
        }
        written = write_file(output_path, jpeg, output_size);
        free(jpeg);
        if (!written) {
            goto fail_output;
        }
    }
    timing->output_bytes = output_size;
    stage = now_us();
    api->free_output(encoder, &output);
    timing->release_us = now_us() - stage;
    timing->total_us = now_us() - start;
    printf("frame %u: JPEG=%zu bytes submit=%.2fms encode=%.2fms "
           "acquire=%.2fms release=%.2fms total=%.2fms\n", frame,
           output_size, timing->submit_us / 1000.0,
           timing->hardware_encode_us / 1000.0,
           timing->acquire_us / 1000.0, timing->release_us / 1000.0,
           timing->total_us / 1000.0);
    free(bitmap);
    return true;

fail_output:
    if (output_acquired) {
        (void)api->free_output(encoder, &output);
    }
    free(bitmap);
    return false;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--output FILE] [--frames N]\n", program);
    printf("  default: one 1280x720 JPEG at /tmp/overlay_test.jpg\n");
    printf("  frames > 1 writes FILE.first.jpg and FILE.last.jpg\n");
}

int main(int argc, char **argv)
{
    const char *output_path = "/tmp/overlay_test.jpg";
    unsigned int frames = 1U;
    EncoderApi api;
    CedarVideoEncoder *encoder = NULL;
    CedarVencBaseConfig config;
    CedarVencAllocateBufferParam buffer_param;
    CedarVencInputBuffer input;
    uint8_t *y = NULL;
    uint8_t *c = NULL;
    unsigned int frame;
    int quality = TEST_QUALITY;
    bool success = false;
    TimingStats totals = {0};
    uint64_t max_overlay_update_us = 0U;
    uint64_t max_total_us = 0U;

    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
            continue;
        }
        if (strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
            frames = (unsigned int)strtoul(argv[++index], NULL, 10);
            if (frames == 0U) {
                fprintf(stderr, "--frames must be positive\n");
                return EXIT_FAILURE;
            }
            continue;
        }
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    printf("vencoder overlay test: %ux%u input=YUV420P stride=%u "
           "frames=%u\n", TEST_WIDTH, TEST_HEIGHT, TEST_STRIDE, frames);
    printf("overlay ABI: index=0x%x info=%zu header=%zu header_ptr_offset=%zu "
           "bitmap_offset=%zu max_blocks=%u\n",
           CEDAR_VENC_INDEX_PARAM_SET_OVERLAY, sizeof(CedarVencOverlayInfo),
           sizeof(CedarVencOverlayHeader),
           offsetof(CedarVencOverlayHeader, overlay_blk_addr),
           offsetof(CedarVencOverlayHeader, bitmap_size),
           CEDAR_VENC_MAX_OVERLAY_SIZE);
    if (!load_api(&api)) {
        unload_api(&api);
        return EXIT_FAILURE;
    }
    encoder = api.create(CEDAR_VENC_CODEC_JPEG);
    if (encoder == NULL) {
        fprintf(stderr, "VideoEncCreate(JPEG) failed\n");
        goto cleanup;
    }
    memset(&config, 0, sizeof(config));
    config.nInputWidth = TEST_WIDTH;
    config.nInputHeight = TEST_HEIGHT;
    config.nDstWidth = TEST_WIDTH;
    config.nDstHeight = TEST_HEIGHT;
    config.nStride = TEST_STRIDE;
    config.eInputFormat = CEDAR_VENC_PIXEL_YUV420P;
    if (api.set_parameter(encoder, 0x200, &quality) != 0 ||
        api.init(encoder, &config) != 0) {
        fprintf(stderr, "JPEG encoder init failed\n");
        goto cleanup;
    }
    memset(&buffer_param, 0, sizeof(buffer_param));
    buffer_param.nBufferNum = 1U;
    buffer_param.nSizeY = TEST_STRIDE * TEST_ALIGNED_HEIGHT;
    buffer_param.nSizeC = buffer_param.nSizeY / 2U;
    if (api.alloc_input(encoder, &buffer_param) != 0) {
        fprintf(stderr, "AllocInputBuffer failed\n");
        goto cleanup;
    }
    memset(&input, 0, sizeof(input));
    if (api.get_input(encoder, &input) != 0 || input.pAddrVirY == NULL ||
        input.pAddrVirC == NULL) {
        fprintf(stderr, "initial GetOneAllocInputBuffer failed\n");
        goto cleanup;
    }
    y = malloc(TEST_STRIDE * TEST_ALIGNED_HEIGHT);
    c = malloc(TEST_STRIDE * TEST_ALIGNED_HEIGHT / 2U);
    if (y == NULL || c == NULL) {
        fprintf(stderr, "test buffer allocation failed\n");
        (void)api.return_input(encoder, &input);
        goto cleanup;
    }
    fill_gray_yuv(y, c);
    (void)api.return_input(encoder, &input);

    for (frame = 0U; frame < frames; ++frame) {
        char path[512];
        const char *frame_path = output_path;
        uint64_t overlay_update_us = 0U;
        TimingStats timing = {0};

        if (frames > 1U) {
            int written = snprintf(path, sizeof(path), "%s.%s.jpg",
                                    output_path,
                                    frame == 0U ? "first" : "last");
            if (frame != 0U && frame + 1U != frames) {
                frame_path = NULL;
            } else if (written < 0 || (size_t)written >= sizeof(path)) {
                fprintf(stderr, "output path too long\n");
                goto cleanup;
            } else {
                frame_path = path;
            }
        }
        if (!encode_one(&api, encoder, y, c, frame_path, &overlay_update_us,
                        &timing, frame)) {
            goto cleanup;
        }
        totals.overlay_update_us += overlay_update_us;
        totals.submit_us += timing.submit_us;
        totals.hardware_encode_us += timing.hardware_encode_us;
        totals.acquire_us += timing.acquire_us;
        totals.release_us += timing.release_us;
        totals.total_us += timing.total_us;
        totals.output_bytes += timing.output_bytes;
        max_overlay_update_us = max_u64(max_overlay_update_us,
                                        overlay_update_us);
        max_total_us = max_u64(max_total_us, timing.total_us);
    }
    printf("PASS: %u JPEG frames, overlay set succeeded for every frame\n",
           frames);
    printf("averages: overlay_update=%.2fms submit=%.2fms encode=%.2fms "
           "acquire=%.2fms release=%.2fms total=%.2fms JPEG=%.1fKB "
           "max_overlay_update=%.2fms max_total=%.2fms\n",
           totals.overlay_update_us / (double)frames / 1000.0,
           totals.submit_us / (double)frames / 1000.0,
           totals.hardware_encode_us / (double)frames / 1000.0,
           totals.acquire_us / (double)frames / 1000.0,
           totals.release_us / (double)frames / 1000.0,
           totals.total_us / (double)frames / 1000.0,
           totals.output_bytes / (double)frames / 1024.0,
           max_overlay_update_us / 1000.0, max_total_us / 1000.0);
    success = true;

cleanup:
    free(c);
    free(y);
    if (encoder != NULL) {
        api.destroy(encoder);
    }
    unload_api(&api);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
