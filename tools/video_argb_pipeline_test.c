#define _POSIX_C_SOURCE 200809L

/* Standalone E/F pipeline test; production video threads are not involved. */
#define G2D_SMOKE_TEST_NO_MAIN
#include "g2d_smoke_test.c"

#include "cedar_vencoder_abi.h"
#include "hud_renderer.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <sys/resource.h>
#include <time.h>

#define PIPELINE_WIDTH 1280U
#define PIPELINE_HEIGHT 720U
#define PIPELINE_BPP 4U
#define PIPELINE_STRIDE (PIPELINE_WIDTH * PIPELINE_BPP)
#define PIPELINE_ARGB_BYTES (PIPELINE_STRIDE * PIPELINE_HEIGHT)
#define PIPELINE_QUALITY 82
#define PIPELINE_DEFAULT_FRAMES 100U
#define PIPELINE_DEFAULT_MONITOR_INTERVAL 100U
#define PIPELINE_DEFAULT_SAMPLE_INTERVAL 1000U
#define PIPELINE_DEFAULT_PROGRESS_INTERVAL 100U
#define PIPELINE_G2D_DEVICE "/dev/g2d"

typedef enum {
    PIPELINE_MODE_E = 0,
    PIPELINE_MODE_F
} PipelineMode;

typedef struct {
    PipelineMode mode;
    unsigned int frames;
    unsigned int monitor_interval;
    unsigned int sample_interval;
    unsigned int progress_interval;
    unsigned int rounds;
    const char *output_prefix;
    bool benchmark;
    bool zero_copy;
    bool skip_cache_flush;
} PipelineOptions;

typedef struct {
    void *library;
    void *jpeg_library;
    CedarVencCreate create;
    CedarVencDestroy destroy;
    CedarVencInit init;
    CedarVideoEncUnInit uninit;
    CedarVencAllocInputBuffer alloc_input;
    CedarReleaseAllocInputBuffer release_input;
    CedarVencGetInputBuffer get_input;
    CedarVencFlushInputBuffer flush_input;
    CedarVencReturnInputBuffer return_input;
    CedarVencAddInputBuffer add_input;
    CedarVencEncode encode;
    CedarVencUsedInputBuffer used_input;
    CedarVencGetOutputBuffer get_output;
    CedarVencFreeOutputBuffer free_output;
    CedarVencSetParameter set_parameter;
} PipelineEncoderApi;

typedef struct {
    const char *name;
    const char *owner;
    IonBuffer ion;
    uint32_t iommu_addr;
} PipelineBuffer;

typedef struct {
    bool logged;
    size_t size;
    uint32_t stride;
    CedarVencPixelFormat format;
    uint8_t *virtual_address;
    uint8_t *physical_address;
    int dma_buf_fd;
} PipelineCedarInput;

typedef uint32_t (*PipelineIonVir2PhyFn)(void *memory);

typedef enum {
    PIPELINE_STAGE_HUD_CLEAR = 0,
    PIPELINE_STAGE_HUD_DRAW,
    PIPELINE_STAGE_HUD_CACHE_SYNC,
    PIPELINE_STAGE_HUD_TOTAL,
    PIPELINE_STAGE_G2D_YUV_TO_ARGB,
    PIPELINE_STAGE_G2D_BLEND,
    PIPELINE_STAGE_CEDAR_INPUT_ACQUIRE,
    PIPELINE_STAGE_ARGB_MEMCPY,
    PIPELINE_STAGE_CEDAR_CACHE_FLUSH,
    PIPELINE_STAGE_CEDAR_INPUT_PREPARE,
    PIPELINE_STAGE_OTHER_HANDOFF,
    PIPELINE_STAGE_HANDOFF_TOTAL,
    PIPELINE_STAGE_CEDAR_SUBMIT,
    PIPELINE_STAGE_CEDAR_HW_ENCODE,
    PIPELINE_STAGE_CEDAR_ACQUIRE_RELEASE,
    PIPELINE_STAGE_JPEG_WRITE,
    PIPELINE_STAGE_OTHER_UNACCOUNTED,
    PIPELINE_STAGE_TOTAL,
    PIPELINE_STAGE_COUNT
} PipelineStage;

typedef struct {
    uint64_t values[PIPELINE_STAGE_COUNT];
    uint32_t jpeg_bytes;
} PipelineFrameTiming;

typedef struct {
    uint64_t frames;
    uint64_t output_bytes;
    uint64_t *samples[PIPELINE_STAGE_COUNT];
} PipelineTiming;

static const char *const pipeline_stage_names[PIPELINE_STAGE_COUNT] = {
    "hud_clear", "hud_draw", "hud_cache_sync", "hud_total",
    "g2d_yuv_to_argb", "g2d_blend", "cedar_input_acquire", "argb_memcpy",
    "cedar_cache_flush", "cedar_input_prepare", "other_handoff",
    "handoff_total", "cedar_submit", "cedar_hw_encode",
    "cedar_acquire_release", "jpeg_write", "other_unaccounted", "total_frame"
};

static uint64_t pipeline_now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0U;
    }
    return (uint64_t)timestamp.tv_sec * 1000000U +
           (uint64_t)timestamp.tv_nsec / 1000U;
}

static const char *pipeline_mode_name(PipelineMode mode)
{
    return mode == PIPELINE_MODE_F ? "F" : "E";
}

static bool pipeline_parse_mode(const char *value, PipelineMode *mode)
{
    if (strcmp(value, "E") == 0 || strcmp(value, "e") == 0) {
        *mode = PIPELINE_MODE_E;
        return true;
    }
    if (strcmp(value, "F") == 0 || strcmp(value, "f") == 0) {
        *mode = PIPELINE_MODE_F;
        return true;
    }
    return false;
}

static bool pipeline_parse_uint(const char *text, unsigned int *value,
                                bool allow_zero)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed > UINT32_MAX || (!allow_zero && parsed == 0UL)) {
        return false;
    }
    *value = (unsigned int)parsed;
    return true;
}

static void pipeline_usage(const char *program)
{
    printf("Usage: %s [--mode E|F] [--frames N] [--rounds N] "
           "[--benchmark] [--monitor-interval N] [--sample-interval N] "
           "[--progress-interval N] [--output-prefix PATH]\n", program);
    printf("  E: persistent ARGB camera + ARGB HUD -> BLD_H -> "
           "Cedar ARGB8888 JPEG\n");
    printf("  F: persistent YUV420P -> BITBLT_H NONE_H -> ARGB camera "
           "+ ARGB HUD -> BLD_H -> Cedar ARGB8888 JPEG\n");
    printf("  --handoff copy|cedar-input (default: copy); cedar-input uses "
           "the Cedar input share fd as G2D BLD_H destination\n");
    printf("  --no-cache-flush skips Cedar input cache flush (only for a "
           "direct-buffer cache experiment)\n");
    printf("  --benchmark disables per-frame full checks and JPEG writes; "
           "sample frames remain checked/written\n");
    printf("  defaults: mode E, %u frames, monitor every %u, samples every %u\n",
           PIPELINE_DEFAULT_FRAMES, PIPELINE_DEFAULT_MONITOR_INTERVAL,
           PIPELINE_DEFAULT_SAMPLE_INTERVAL);
}

static bool pipeline_parse_options(int argc, char **argv,
                                   PipelineOptions *options)
{
    int index;

    memset(options, 0, sizeof(*options));
    options->mode = PIPELINE_MODE_E;
    options->frames = PIPELINE_DEFAULT_FRAMES;
    options->monitor_interval = PIPELINE_DEFAULT_MONITOR_INTERVAL;
    options->sample_interval = PIPELINE_DEFAULT_SAMPLE_INTERVAL;
    options->progress_interval = PIPELINE_DEFAULT_PROGRESS_INTERVAL;
    options->rounds = 1U;
    options->output_prefix = "/tmp/video_argb_pipeline";
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 ||
            strcmp(argv[index], "-h") == 0) {
            pipeline_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(argv[index], "--benchmark") == 0) {
            options->benchmark = true;
            continue;
        }
        if (strcmp(argv[index], "--handoff") == 0 && index + 1 < argc) {
            const char *handoff = argv[++index];

            if (strcmp(handoff, "copy") == 0) {
                options->zero_copy = false;
            } else if (strcmp(handoff, "cedar-input") == 0) {
                options->zero_copy = true;
            } else {
                fprintf(stderr, "invalid --handoff value: %s\n", handoff);
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--no-cache-flush") == 0) {
            options->skip_cache_flush = true;
            continue;
        }
        if (strcmp(argv[index], "--mode") == 0 && index + 1 < argc) {
            if (!pipeline_parse_mode(argv[++index], &options->mode)) {
                fprintf(stderr, "invalid --mode value: %s\n", argv[index]);
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
            if (!pipeline_parse_uint(argv[++index], &options->frames, false)) {
                fprintf(stderr, "invalid --frames value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--rounds") == 0 && index + 1 < argc) {
            if (!pipeline_parse_uint(argv[++index], &options->rounds, false)) {
                fprintf(stderr, "invalid --rounds value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--monitor-interval") == 0 &&
            index + 1 < argc) {
            if (!pipeline_parse_uint(argv[++index],
                                     &options->monitor_interval, true)) {
                fprintf(stderr, "invalid --monitor-interval value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--sample-interval") == 0 &&
            index + 1 < argc) {
            if (!pipeline_parse_uint(argv[++index],
                                     &options->sample_interval, true)) {
                fprintf(stderr, "invalid --sample-interval value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--progress-interval") == 0 &&
            index + 1 < argc) {
            if (!pipeline_parse_uint(argv[++index],
                                     &options->progress_interval, true)) {
                fprintf(stderr, "invalid --progress-interval value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--output-prefix") == 0 &&
            index + 1 < argc) {
            options->output_prefix = argv[++index];
            continue;
        }
        fprintf(stderr, "unknown or incomplete option: %s\n", argv[index]);
        return false;
    }
    return true;
}

static bool pipeline_load_symbol(void *library, void *target,
                                 size_t target_size, const char *name)
{
    void *symbol;

    symbol = dlsym(library, name);
    if (symbol == NULL || target_size != sizeof(symbol)) {
        fprintf(stderr, "missing Cedar symbol %s: %s\n", name, dlerror());
        return false;
    }
    memcpy(target, &symbol, target_size);
    return true;
}

static void pipeline_unload_encoder_api(PipelineEncoderApi *api)
{
    if (api->jpeg_library != NULL) {
        dlclose(api->jpeg_library);
    }
    if (api->library != NULL) {
        dlclose(api->library);
    }
    memset(api, 0, sizeof(*api));
}

static bool pipeline_load_encoder_api(PipelineEncoderApi *api)
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
        pipeline_unload_encoder_api(api);
        return false;
    }
#define PIPELINE_LOAD(member, symbol)                                           \
    do {                                                                        \
        if (!pipeline_load_symbol(api->library, &api->member,                 \
                                  sizeof(api->member), symbol)) {               \
            pipeline_unload_encoder_api(api);                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)
    PIPELINE_LOAD(create, "VideoEncCreate");
    PIPELINE_LOAD(destroy, "VideoEncDestroy");
    PIPELINE_LOAD(init, "VideoEncInit");
    PIPELINE_LOAD(uninit, "VideoEncUnInit");
    PIPELINE_LOAD(alloc_input, "AllocInputBuffer");
    PIPELINE_LOAD(release_input, "ReleaseAllocInputBuffer");
    PIPELINE_LOAD(get_input, "GetOneAllocInputBuffer");
    PIPELINE_LOAD(flush_input, "FlushCacheAllocInputBuffer");
    PIPELINE_LOAD(return_input, "ReturnOneAllocInputBuffer");
    PIPELINE_LOAD(add_input, "AddOneInputBuffer");
    PIPELINE_LOAD(encode, "VideoEncodeOneFrame");
    PIPELINE_LOAD(used_input, "AlreadyUsedInputBuffer");
    PIPELINE_LOAD(get_output, "GetOneBitstreamFrame");
    PIPELINE_LOAD(free_output, "FreeOneBitStreamFrame");
    PIPELINE_LOAD(set_parameter, "VideoEncSetParameter");
#undef PIPELINE_LOAD
    return true;
}

static void pipeline_set_mock_state(VehicleState *state, unsigned int frame)
{
    memset(state, 0, sizeof(*state));
    state->timestamp_us = pipeline_now_us();
    state->valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                        VEHICLE_STATE_VALID_MOTOR_RPM |
                        VEHICLE_STATE_VALID_TORQUE_NM |
                        VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                        VEHICLE_STATE_VALID_HV_CURRENT_A |
                        VEHICLE_STATE_VALID_HV_POWER_W |
                        VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                        VEHICLE_STATE_VALID_LV_VOLTAGE_V |
                        VEHICLE_STATE_VALID_ACC_X_MPS2 |
                        VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                        VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                        VEHICLE_STATE_VALID_BRAKE_PERCENT |
                        VEHICLE_STATE_VALID_DRIVE_STATE;
    state->event_mode = RACE_EVENT_CIRCUIT_MODE;
    state->lap_valid = true;
    state->lap_number = frame + 1U;
    state->lap_time_s = 87.25 + (double)(frame % 900U) * 0.01;
    state->best_lap_time_s = 86.91;
    state->delta_time_s = ((int)(frame % 41U) - 20) * 0.006;
    state->speed_kmh = 72.0 + (double)(frame % 120U) * 0.25;
    state->motor_rpm = 2800.0 + (double)(frame % 180U) * 37.0;
    state->torque_nm = 188.0 + (double)(frame % 60U) * 0.75;
    state->hv_voltage_v = 386.0 + (double)(frame % 9U) * 0.1;
    state->hv_current_a = 42.0 + (double)(frame % 50U) * 0.2;
    state->hv_power_w = state->hv_voltage_v * state->hv_current_a;
    state->hv_soc_percent = 78.0 - (double)(frame % 500U) * 0.01;
    state->lv_voltage_v = 13.8;
    state->acc_x = ((int)(frame % 30U) - 15) * 0.08;
    state->acc_y = ((int)(frame % 50U) - 25) * 0.05;
    state->throttle_percent = 34.0 + (double)(frame % 40U) * 0.5;
    state->brake_percent = frame % 75U < 8U ? 18.0 : 0.0;
    state->drive_state = VEHICLE_DRIVE_STATE_DRIVE;
}

static bool pipeline_buffer_alloc(const IonApi *ion, PipelineBuffer *buffer,
                                  const char *name, const char *owner,
                                  size_t size)
{
    buffer->name = name;
    buffer->owner = owner;
    if (!ion_buffer_alloc(ion, &buffer->ion, size)) {
        fprintf(stderr, "buffer alloc failed name=%s size=%zu owner=%s\n",
                name, size, owner);
        return false;
    }
    buffer->iommu_addr = 0U;
    if (ion->library != NULL) {
        PipelineIonVir2PhyFn vir2phy;
        void *symbol;

        symbol = dlsym(ion->library, "sunxi_ion_alloc_vir2phy_cpu");
        vir2phy = NULL;
        if (symbol != NULL) {
            memcpy(&vir2phy, &symbol, sizeof(vir2phy));
            buffer->iommu_addr = vir2phy(buffer->ion.memory);
        }
    }
    printf("buffer alloc name=%s size=%zu ion_handle=%p dma_buf_fd=%d "
           "virtual=%p physical=0x%08" PRIx32 " owner=%s\n", name, size,
           buffer->ion.memory, buffer->ion.fd, buffer->ion.memory,
           buffer->iommu_addr, owner);
    return true;
}

static bool pipeline_fd_is_dma_buf(int fd)
{
    char proc_path[64];
    char target[256];
    ssize_t length;
    int written;

    if (fd <= 0) {
        return false;
    }
    written = snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    if (written < 0 || (size_t)written >= sizeof(proc_path)) {
        return false;
    }
    length = readlink(proc_path, target, sizeof(target) - 1U);
    if (length <= 0) {
        return false;
    }
    target[length] = '\0';
    return strstr(target, "dmabuf") != NULL;
}

static void pipeline_buffer_release(const IonApi *ion, PipelineBuffer *buffer,
                                    ResourceMonitor *monitor, uint64_t frame)
{
    int fd;
    size_t size;

    if (buffer->ion.memory == NULL) {
        return;
    }
    fd = buffer->ion.fd;
    size = buffer->ion.size;
    printf("buffer release begin name=%s size=%zu ion_handle=%p "
           "dma_buf_fd=%d owner=%s\n", buffer->name, size,
           buffer->ion.memory, fd, buffer->owner);
    ion_buffer_free(ion, &buffer->ion);
    printf("buffer release complete name=%s old_dma_buf_fd=%d\n",
           buffer->name, fd);
    if (monitor != NULL) {
        resource_monitor_sample(monitor, frame, true);
    }
}

static void pipeline_log_cedar_input(PipelineCedarInput *info,
                                     const CedarVencInputBuffer *input)
{
    if (info->logged) {
        return;
    }
    info->logged = true;
    info->size = PIPELINE_ARGB_BYTES;
    info->stride = PIPELINE_STRIDE;
    info->format = CEDAR_VENC_PIXEL_ARGB;
    info->virtual_address = input->pAddrVirY;
    info->physical_address = input->pAddrPhyY;
    info->dma_buf_fd = input->nShareBufFd;
    printf("buffer alloc name=cedar_argb_input size=%zu stride=%u "
           "format=ARGB8888 ion_handle=cedar dma_buf_fd=%d virtual=%p "
           "physical=%p owner=Cedar state=FREE\n", info->size, info->stride,
           info->dma_buf_fd, info->virtual_address, info->physical_address);
}

static bool pipeline_acquire_input(const PipelineEncoderApi *api,
                                   CedarVideoEncoder *encoder,
                                   CedarVencInputBuffer *input,
                                   const PipelineOptions *options,
                                   unsigned int frame,
                                   PipelineCedarInput *input_info,
                                   PipelineFrameTiming *timing)
{
    uint64_t stage = pipeline_now_us();

    memset(input, 0, sizeof(*input));
    if (api->get_input(encoder, input) != 0 || input->pAddrVirY == NULL) {
        timing->values[PIPELINE_STAGE_CEDAR_INPUT_ACQUIRE] +=
            pipeline_now_us() - stage;
        fprintf(stderr, "[%s] Cedar input unavailable at frame %u\n",
                pipeline_mode_name(options->mode), frame + 1U);
        return false;
    }
    timing->values[PIPELINE_STAGE_CEDAR_INPUT_ACQUIRE] +=
        pipeline_now_us() - stage;
    pipeline_log_cedar_input(input_info, input);
    if (options->zero_copy &&
        (input->nShareBufFd <= 0 ||
         !pipeline_fd_is_dma_buf(input->nShareBufFd))) {
        (void)api->return_input(encoder, input);
        fprintf(stderr,
                "[%s] zero-copy blocked at frame %u: Cedar input share fd=%d "
                "is not a confirmed dma-buf; physical address fallback is "
                "disabled\n", pipeline_mode_name(options->mode), frame + 1U,
                input->nShareBufFd);
        return false;
    }
    return true;
}

static void pipeline_return_input(const PipelineEncoderApi *api,
                                  CedarVideoEncoder *encoder,
                                  CedarVencInputBuffer *input, bool used)
{
    if (used) {
        (void)api->used_input(encoder, input);
    }
    (void)api->return_input(encoder, input);
}

static void pipeline_close_encoder(const PipelineEncoderApi *api,
                                   CedarVideoEncoder *encoder,
                                   bool input_allocated,
                                   const PipelineCedarInput *input_info)
{
    if (encoder == NULL) {
        return;
    }
    if (input_allocated) {
        printf("buffer release begin name=cedar_argb_input size=%zu "
               "dma_buf_fd=%d owner=Cedar ReleaseAllocInputBuffer\n",
               input_info->size, input_info->dma_buf_fd);
        (void)api->release_input(encoder);
        printf("buffer release complete name=cedar_argb_input owner=Cedar\n");
    }
    /* VideoEncDestroy owns final uninit on this libvencoder build. */
    printf("Cedar destroy owner=VideoEncDestroy explicit_uninit=no\n");
    api->destroy(encoder);
}

static bool pipeline_write_jpeg(const char *path,
                                const CedarVencOutputBuffer *output)
{
    FILE *file;
    bool success;

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot open JPEG %s: %s\n", path, strerror(errno));
        return false;
    }
    success = output->pData0 != NULL && output->nSize0 > 0U &&
              fwrite(output->pData0, 1U, output->nSize0, file) ==
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

static bool pipeline_jpeg_is_valid(const CedarVencOutputBuffer *output)
{
    const uint8_t *last;

    if (output == NULL || output->pData0 == NULL || output->nSize0 < 2U ||
        (output->nSize1 > 0U && output->pData1 == NULL)) {
        return false;
    }
    if (output->pData0[0] != 0xffU || output->pData0[1] != 0xd8U) {
        return false;
    }
    if (output->nSize1 > 0U) {
        last = output->pData1 + output->nSize1 - 1U;
    } else {
        last = output->pData0 + output->nSize0 - 1U;
    }
    return *last == 0xd9U;
}

static bool pipeline_prepare_output_path(char *path, size_t path_size,
                                         const PipelineOptions *options,
                                         unsigned int round,
                                         unsigned int frame)
{
    int written = snprintf(path, path_size, "%s_%s_r%u_f%u.jpg",
                           options->output_prefix,
                           pipeline_mode_name(options->mode), round + 1U,
                           frame + 1U);

    return written >= 0 && (size_t)written < path_size;
}

static bool pipeline_should_sample(const PipelineOptions *options,
                                   unsigned int frame)
{
    unsigned int one_based = frame + 1U;

    if (!options->benchmark) {
        return frame == 0U || one_based == options->frames;
    }
    return frame == 0U || one_based == options->frames ||
           (options->sample_interval != 0U &&
            one_based % options->sample_interval == 0U);
}

static bool pipeline_timing_init(PipelineTiming *stats, unsigned int frames)
{
    size_t index;

    memset(stats, 0, sizeof(*stats));
    for (index = 0U; index < PIPELINE_STAGE_COUNT; ++index) {
        stats->samples[index] = calloc(frames, sizeof(uint64_t));
        if (stats->samples[index] == NULL) {
            size_t release_index;

            for (release_index = 0U; release_index <= index; ++release_index) {
                free(stats->samples[release_index]);
                stats->samples[release_index] = NULL;
            }
            return false;
        }
    }
    return true;
}

static void pipeline_timing_close(PipelineTiming *stats)
{
    size_t index;

    for (index = 0U; index < PIPELINE_STAGE_COUNT; ++index) {
        free(stats->samples[index]);
        stats->samples[index] = NULL;
    }
}

static int pipeline_compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t pipeline_p95(const PipelineTiming *stats, PipelineStage stage)
{
    uint64_t *sorted;
    uint64_t position;
    uint64_t result;

    if (stats->frames == 0U) {
        return 0U;
    }
    sorted = malloc((size_t)stats->frames * sizeof(*sorted));
    if (sorted == NULL) {
        return 0U;
    }
    memcpy(sorted, stats->samples[stage],
           (size_t)stats->frames * sizeof(*sorted));
    qsort(sorted, (size_t)stats->frames, sizeof(*sorted),
          pipeline_compare_u64);
    position = (stats->frames * 95U + 99U) / 100U;
    if (position == 0U) {
        position = 1U;
    }
    result = sorted[position - 1U];
    free(sorted);
    return result;
}

static void pipeline_timing_record(PipelineTiming *stats,
                                    const PipelineFrameTiming *frame)
{
    PipelineStage stage;
    uint64_t index = stats->frames;

    for (stage = 0; stage < PIPELINE_STAGE_COUNT; ++stage) {
        stats->samples[stage][index] = frame->values[stage];
    }
    stats->frames++;
    stats->output_bytes += frame->jpeg_bytes;
}

static void pipeline_print_timing(PipelineMode mode, unsigned int frame,
                                  const PipelineFrameTiming *timing)
{
    printf("[%s] frame=%u hud_clear=%.2fms hud_draw=%.2fms "
           "hud_sync=%.2fms hud_total=%.2fms yuv_to_argb=%.2fms "
           "BLD_H=%.2fms acquire=%.2fms memcpy=%.2fms flush=%.2fms "
           "prepare=%.2fms handoff=%.2fms submit=%.2fms encode=%.2fms "
           "acquire_release=%.2fms jpeg_write=%.2fms other=%.2fms "
           "total=%.2fms JPEG=%u\n",
           pipeline_mode_name(mode), frame,
           timing->values[PIPELINE_STAGE_HUD_CLEAR] / 1000.0,
           timing->values[PIPELINE_STAGE_HUD_DRAW] / 1000.0,
           timing->values[PIPELINE_STAGE_HUD_CACHE_SYNC] / 1000.0,
           timing->values[PIPELINE_STAGE_HUD_TOTAL] / 1000.0,
           timing->values[PIPELINE_STAGE_G2D_YUV_TO_ARGB] / 1000.0,
           timing->values[PIPELINE_STAGE_G2D_BLEND] / 1000.0,
           timing->values[PIPELINE_STAGE_CEDAR_INPUT_ACQUIRE] / 1000.0,
           timing->values[PIPELINE_STAGE_ARGB_MEMCPY] / 1000.0,
           timing->values[PIPELINE_STAGE_CEDAR_CACHE_FLUSH] / 1000.0,
           timing->values[PIPELINE_STAGE_CEDAR_INPUT_PREPARE] / 1000.0,
           timing->values[PIPELINE_STAGE_HANDOFF_TOTAL] / 1000.0,
           timing->values[PIPELINE_STAGE_CEDAR_SUBMIT] / 1000.0,
           timing->values[PIPELINE_STAGE_CEDAR_HW_ENCODE] / 1000.0,
           timing->values[PIPELINE_STAGE_CEDAR_ACQUIRE_RELEASE] / 1000.0,
           timing->values[PIPELINE_STAGE_JPEG_WRITE] / 1000.0,
           timing->values[PIPELINE_STAGE_OTHER_UNACCOUNTED] / 1000.0,
           timing->values[PIPELINE_STAGE_TOTAL] / 1000.0,
           timing->jpeg_bytes);
}

static void pipeline_print_summary(PipelineMode mode,
                                   const PipelineTiming *stats,
                                   uint64_t wall_us, uint64_t cpu_us)
{
    PipelineStage stage;
    double frames = (double)stats->frames;
    double fps = wall_us != 0U ? frames * 1000000.0 / (double)wall_us : 0.0;

    printf("[%s] PASS: frames=%" PRIu64 " wall=%.2fs FPS=%.2f "
           "cpu=%.2fms cpu_util=%.1f%% avg_jpeg=%.1fKB\n",
           pipeline_mode_name(mode), stats->frames, wall_us / 1000000.0,
           fps, cpu_us / 1000.0,
           wall_us != 0U ? (double)cpu_us * 100.0 / (double)wall_us : 0.0,
           stats->frames != 0U ? (double)stats->output_bytes / frames / 1024.0 :
           0.0);
    for (stage = 0; stage < PIPELINE_STAGE_COUNT; ++stage) {
        uint64_t sum = 0U;
        uint64_t index;
        uint64_t p95 = pipeline_p95(stats, stage);
        uint64_t max = 0U;

        for (index = 0U; index < stats->frames; ++index) {
            sum += stats->samples[stage][index];
            if (stats->samples[stage][index] > max) {
                max = stats->samples[stage][index];
            }
        }
        printf("[%s] stage %-24s avg=%7.3fms p95=%7.3fms max=%7.3fms\n",
               pipeline_mode_name(mode), pipeline_stage_names[stage],
               frames != 0.0 ? (double)sum / frames / 1000.0 : 0.0,
               p95 / 1000.0, max / 1000.0);
    }
}

static void pipeline_print_resource_delta(const char *label,
                                          const ResourceSample *before,
                                          const ResourceSample *after)
{
    int64_t dma_objects = 0;
    int64_t dma_bytes = 0;
    int64_t fd = (int64_t)after->fd_count - (int64_t)before->fd_count;
    int64_t rss = (int64_t)after->rss_kb - (int64_t)before->rss_kb;
    int64_t cma = 0;

    if (before->have_dma_buf && after->have_dma_buf) {
        dma_objects = (int64_t)after->dma_buf_objects -
                      (int64_t)before->dma_buf_objects;
        dma_bytes = (int64_t)after->dma_buf_bytes -
                    (int64_t)before->dma_buf_bytes;
    }
    if (before->have_cma_free && after->have_cma_free) {
        cma = (int64_t)before->cma_free_kb -
              (int64_t)after->cma_free_kb;
    }
    printf("resource %s delta cma_free_loss_kb=%" PRId64
           " dma_buf_count_delta=%" PRId64 " dma_buf_bytes_delta=%" PRId64
           " fd_delta=%" PRId64 " rss_delta_kb=%" PRId64 "\n", label,
           cma, dma_objects, dma_bytes, fd, rss);
}

static bool pipeline_encode_frame(const PipelineEncoderApi *api,
                                  CedarVideoEncoder *encoder,
                                  const PipelineBuffer *final_argb,
                                  const PipelineOptions *options,
                                  unsigned int round,
                                  unsigned int frame,
                                  bool write_sample,
                                  PipelineCedarInput *input_info,
                                  PipelineFrameTiming *timing,
                                  CedarVencInputBuffer *acquired_input)
{
    CedarVencInputBuffer local_input;
    CedarVencInputBuffer *input = acquired_input;
    CedarVencOutputBuffer output;
    uint64_t stage;
    uint64_t output_stage;
    uint64_t write_stage;
    uint64_t release_stage;
    char output_path[512];
    bool output_acquired = false;
    bool input_added = false;
    bool contiguous_handoff = input == NULL;
    uint64_t handoff_start = contiguous_handoff ? pipeline_now_us() : 0U;
    int encode_result;

    if (input == NULL) {
        input = &local_input;
        if (!pipeline_acquire_input(api, encoder, input, options, frame,
                                     input_info, timing)) {
            return false;
        }
    }

    if (!options->zero_copy) {
        if (final_argb == NULL || final_argb->ion.memory == NULL) {
            pipeline_return_input(api, encoder, input, false);
            fprintf(stderr, "[%s] copy handoff has no final ARGB buffer at "
                    "frame %u\n", pipeline_mode_name(options->mode),
                    frame + 1U);
            return false;
        }
        stage = pipeline_now_us();
        memcpy(input->pAddrVirY, final_argb->ion.memory, PIPELINE_ARGB_BYTES);
        timing->values[PIPELINE_STAGE_ARGB_MEMCPY] =
            pipeline_now_us() - stage;
    }

    stage = pipeline_now_us();
    input->nWidth = (int)PIPELINE_WIDTH;
    input->nHeight = (int)PIPELINE_HEIGHT;
    input->nAlign = (int)PIPELINE_STRIDE;
    input->nPts = (int64_t)pipeline_now_us();
    timing->values[PIPELINE_STAGE_CEDAR_INPUT_PREPARE] =
        pipeline_now_us() - stage;
    if (!options->skip_cache_flush) {
        stage = pipeline_now_us();
        if (api->flush_input(encoder, input) != 0) {
            pipeline_return_input(api, encoder, input, false);
            fprintf(stderr, "[%s] Cedar input cache flush failed at frame "
                    "%u\n", pipeline_mode_name(options->mode), frame + 1U);
            return false;
        }
        timing->values[PIPELINE_STAGE_CEDAR_CACHE_FLUSH] =
            pipeline_now_us() - stage;
    }
    {
        uint64_t component_sum =
            timing->values[PIPELINE_STAGE_CEDAR_INPUT_ACQUIRE] +
            timing->values[PIPELINE_STAGE_ARGB_MEMCPY] +
            timing->values[PIPELINE_STAGE_CEDAR_CACHE_FLUSH] +
            timing->values[PIPELINE_STAGE_CEDAR_INPUT_PREPARE];

        if (contiguous_handoff) {
            timing->values[PIPELINE_STAGE_HANDOFF_TOTAL] =
                pipeline_now_us() - handoff_start;
            timing->values[PIPELINE_STAGE_OTHER_HANDOFF] =
                timing->values[PIPELINE_STAGE_HANDOFF_TOTAL] > component_sum ?
                timing->values[PIPELINE_STAGE_HANDOFF_TOTAL] - component_sum :
                0U;
        } else {
            /* Direct mode acquires the input before G2D owns it, so the
             * total is the sum of the non-G2D ownership operations. */
            timing->values[PIPELINE_STAGE_HANDOFF_TOTAL] = component_sum;
        }
    }

    stage = pipeline_now_us();
    if (api->add_input(encoder, input) != 0) {
        pipeline_return_input(api, encoder, input, false);
        fprintf(stderr, "[%s] Cedar input submit failed at frame %u\n",
                pipeline_mode_name(options->mode), frame + 1U);
        return false;
    }
    input_added = true;
    timing->values[PIPELINE_STAGE_CEDAR_SUBMIT] = pipeline_now_us() - stage;

    stage = pipeline_now_us();
    encode_result = api->encode(encoder);
    timing->values[PIPELINE_STAGE_CEDAR_HW_ENCODE] = pipeline_now_us() - stage;
    if (encode_result < 0) {
        pipeline_return_input(api, encoder, input, input_added);
        input_added = false;
        fprintf(stderr, "[%s] Cedar encode failed at frame %u\n",
                pipeline_mode_name(options->mode), frame + 1U);
        return false;
    }

    output_stage = pipeline_now_us();
    pipeline_return_input(api, encoder, input, input_added);
    input_added = false;
    memset(&output, 0, sizeof(output));
    if (api->get_output(encoder, &output) != 0) {
        fprintf(stderr, "[%s] Cedar output unavailable at frame %u\n",
                pipeline_mode_name(options->mode), frame + 1U);
        return false;
    }
    output_acquired = true;
    if (!pipeline_jpeg_is_valid(&output)) {
        fprintf(stderr, "[%s] invalid Cedar JPEG output at frame %u\n",
                pipeline_mode_name(options->mode), frame + 1U);
        goto fail_output;
    }
    timing->values[PIPELINE_STAGE_CEDAR_ACQUIRE_RELEASE] +=
        pipeline_now_us() - output_stage;
    timing->jpeg_bytes = output.nSize0 + output.nSize1;
    if (write_sample) {
        if (!pipeline_prepare_output_path(output_path, sizeof(output_path),
                                          options, round, frame)) {
            goto fail_output;
        }
        write_stage = pipeline_now_us();
        if (!pipeline_write_jpeg(output_path, &output)) {
            goto fail_output;
        }
        timing->values[PIPELINE_STAGE_JPEG_WRITE] =
            pipeline_now_us() - write_stage;
        printf("jpeg sample frame=%u path=%s bytes=%u\n", frame + 1U,
               output_path, timing->jpeg_bytes);
    }
    release_stage = pipeline_now_us();
    if (api->free_output(encoder, &output) != 0) {
        fprintf(stderr, "[%s] Cedar output release failed at frame %u\n",
                pipeline_mode_name(options->mode), frame + 1U);
        output_acquired = false;
        return false;
    }
    timing->values[PIPELINE_STAGE_CEDAR_ACQUIRE_RELEASE] +=
        pipeline_now_us() - release_stage;
    output_acquired = false;
    return true;

fail_output:
    if (output_acquired) {
        release_stage = pipeline_now_us();
        (void)api->free_output(encoder, &output);
        timing->values[PIPELINE_STAGE_CEDAR_ACQUIRE_RELEASE] +=
            pipeline_now_us() - release_stage;
    }
    return false;
}

static bool pipeline_run(const PipelineOptions *options, unsigned int round)
{
    const g2d_fmt_enh argb_format = G2D_FORMAT_ARGB8888;
    const g2d_fmt_enh yuv_format = G2D_FORMAT_YUV420_PLANAR;
    PipelineEncoderApi encoder_api;
    CedarVideoEncoder *encoder = NULL;
    CedarVencBaseConfig encoder_config;
    CedarVencAllocateBufferParam buffer_param;
    HudRenderer hud_renderer;
    const HudLayer *dynamic_layer;
    HudSurface hud_surface;
    IonApi ion;
    PipelineBuffer camera;
    PipelineBuffer hud;
    PipelineBuffer intermediate;
    PipelineBuffer final_argb;
    PipelineCedarInput cedar_input;
    int g2d_fd = -1;
    bool ion_opened = false;
    bool input_allocated = false;
    bool success = false;
    bool monitor_finished = false;
    bool baseline_valid = false;
    ResourceSample baseline;
    ResourceSample shutdown;
    ResourceMonitor monitor;
    PipelineTiming stats;
    struct rusage cpu_start;
    struct rusage cpu_end;
    uint64_t wall_start;
    uint64_t wall_end;
    uint32_t previous_hud_checksum = 0U;
    bool have_previous_hud_checksum = false;
    unsigned int frame;

    memset(&encoder_api, 0, sizeof(encoder_api));
    memset(&hud_renderer, 0, sizeof(hud_renderer));
    memset(&hud_surface, 0, sizeof(hud_surface));
    memset(&ion, 0, sizeof(ion));
    memset(&camera, 0, sizeof(camera));
    memset(&hud, 0, sizeof(hud));
    memset(&intermediate, 0, sizeof(intermediate));
    memset(&final_argb, 0, sizeof(final_argb));
    memset(&cedar_input, 0, sizeof(cedar_input));
    camera.ion.fd = -1;
    hud.ion.fd = -1;
    intermediate.ion.fd = -1;
    final_argb.ion.fd = -1;
    memset(&stats, 0, sizeof(stats));
    resource_monitor_init(&monitor, options->monitor_interval);
    if (!resource_monitor_collect(&baseline, 0U)) {
        fprintf(stderr, "resource baseline unavailable for round %u\n",
                round + 1U);
    } else {
        baseline_valid = true;
        printf("resource round=%u baseline\n", round + 1U);
        resource_monitor_print(&baseline);
    }
    if (!pipeline_timing_init(&stats, options->frames)) {
        fprintf(stderr, "timing allocation failed for %u frames\n",
                options->frames);
        return false;
    }
    (void)getrusage(RUSAGE_SELF, &cpu_start);
    wall_start = pipeline_now_us();

    printf("video ARGB pipeline: mode=%s handoff=%s round=%u/%u "
           "1280x720 frames=%u benchmark=%s cache_flush=%s\n",
           pipeline_mode_name(options->mode),
           options->zero_copy ? "cedar-input" : "copy", round + 1U,
           options->rounds, options->frames, options->benchmark ? "on" : "off",
           options->skip_cache_flush ? "off" : "on");
    printf("chain: %s\n", options->zero_copy ?
           (options->mode == PIPELINE_MODE_F ?
            "YUV420P --BITBLT_H/NONE_H--> ARGB camera + ARGB HUD "
            "--BLD_H dst=Cedar input--> JPEG" :
            "ARGB camera + ARGB HUD --BLD_H dst=Cedar input--> JPEG") :
           (options->mode == PIPELINE_MODE_F ?
            "YUV420P --BITBLT_H/NONE_H--> ARGB camera + ARGB HUD "
            "--BLD_H--> final ARGB --copy--> Cedar ARGB8888 --JPEG--> output" :
            "ARGB camera + ARGB HUD --BLD_H--> final ARGB "
            "--copy--> Cedar ARGB8888 --JPEG--> output"));
    if (!options->zero_copy) {
        printf("chain handoff detail: G2D final ARGB --CPU memcpy--> Cedar "
               "input\n");
    } else {
        printf("chain handoff detail: G2D BLD_H dst=Cedar input dma-buf fd; "
               "physical-address fallback=disabled\n");
    }

    if (!pipeline_load_encoder_api(&encoder_api)) {
        goto cleanup;
    }
    encoder = encoder_api.create(CEDAR_VENC_CODEC_JPEG);
    if (encoder == NULL) {
        fprintf(stderr, "VideoEncCreate JPEG failed\n");
        goto cleanup;
    }
    memset(&encoder_config, 0, sizeof(encoder_config));
    encoder_config.nInputWidth = 1280U;
    encoder_config.nInputHeight = 720U;
    encoder_config.nDstWidth = 1280U;
    encoder_config.nDstHeight = 720U;
    encoder_config.nStride = PIPELINE_STRIDE;
    encoder_config.eInputFormat = CEDAR_VENC_PIXEL_ARGB;
    if (encoder_api.set_parameter(encoder, 0x200,
                                  &(int){PIPELINE_QUALITY}) != 0 ||
        encoder_api.init(encoder, &encoder_config) != 0) {
        fprintf(stderr, "Cedar JPEG ARGB init failed\n");
        goto cleanup;
    }
    memset(&buffer_param, 0, sizeof(buffer_param));
    buffer_param.nBufferNum = 1U;
    buffer_param.nSizeY = PIPELINE_ARGB_BYTES;
    if (encoder_api.alloc_input(encoder, &buffer_param) != 0) {
        fprintf(stderr, "Cedar persistent ARGB input allocation failed\n");
        goto cleanup;
    }
    input_allocated = true;

    if (!ion_load(&ion) || ion.open() != 0) {
        fprintf(stderr, "cannot open board ION allocator\n");
        goto cleanup;
    }
    ion_opened = true;
    if (options->mode == PIPELINE_MODE_F) {
        if (!pipeline_buffer_alloc(&ion, &camera, "yuv_source", "pipeline",
                                   image_size_for_format(yuv_format, 1280U,
                                                         720U)) ||
            !pipeline_buffer_alloc(&ion, &intermediate, "argb_camera",
                                   "G2D YUV->ARGB", PIPELINE_ARGB_BYTES)) {
            fprintf(stderr, "F mode camera/intermediate ION allocation failed\n");
            goto cleanup;
        }
        fill_yuv420_pattern((uint8_t *)camera.ion.memory, 1280U, 720U);
        memset(intermediate.ion.memory, 0, intermediate.ion.size);
        flush_buffer(&ion, &camera.ion);
        flush_buffer(&ion, &intermediate.ion);
    } else {
        if (!pipeline_buffer_alloc(&ion, &camera, "argb_camera", "pipeline",
                                   PIPELINE_ARGB_BYTES)) {
            fprintf(stderr, "E mode camera ION allocation failed\n");
            goto cleanup;
        }
        fill_pattern((uint32_t *)camera.ion.memory, 1280U, 720U);
        flush_buffer(&ion, &camera.ion);
    }
    if (!pipeline_buffer_alloc(&ion, &hud, "argb_hud", "HUD/G2D",
                               PIPELINE_ARGB_BYTES) ||
        (!options->zero_copy &&
         !pipeline_buffer_alloc(&ion, &final_argb, "argb_final", "G2D/Cedar",
                                PIPELINE_ARGB_BYTES))) {
        fprintf(stderr, "HUD/final ARGB ION allocation failed\n");
        goto cleanup;
    }
    memset(hud.ion.memory, 0, hud.ion.size);
    flush_buffer(&ion, &hud.ion);
    if (!options->zero_copy) {
        memset(final_argb.ion.memory, 0, final_argb.ion.size);
        flush_buffer(&ion, &final_argb.ion);
    }

    hud_surface.pixels = (uint8_t *)hud.ion.memory;
    hud_surface.width = PIPELINE_WIDTH;
    hud_surface.height = PIPELINE_HEIGHT;
    hud_surface.stride_bytes = PIPELINE_STRIDE;
    hud_surface.pixel_format = HUD_PIXEL_FORMAT_ARGB8888;
    if (hud_renderer_init_external(&hud_renderer, PIPELINE_WIDTH,
                                   PIPELINE_HEIGHT) != 0) {
        fprintf(stderr, "hud_renderer_init_external failed\n");
        goto cleanup;
    }
    dynamic_layer = hud_renderer_render_dynamic_to_surface(
        &hud_renderer, NULL, true, &hud_surface, NULL);
    if (dynamic_layer == NULL) {
        fprintf(stderr, "initial HUD render failed\n");
        goto cleanup;
    }
    flush_buffer(&ion, &hud.ion);
    printf("persistent buffers ready mode=%s camera_fd=%d hud_fd=%d "
           "intermediate_fd=%d final_fd=%d\n", pipeline_mode_name(options->mode),
           camera.ion.fd, hud.ion.fd, intermediate.ion.fd,
           options->zero_copy ? -1 : final_argb.ion.fd);

    g2d_fd = open(PIPELINE_G2D_DEVICE, O_RDWR | O_CLOEXEC);
    if (g2d_fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", PIPELINE_G2D_DEVICE,
                strerror(errno));
        goto cleanup;
    }
    resource_monitor_sample(&monitor, 0U, true);

    for (frame = 0U; frame < options->frames; ++frame) {
        VehicleState state;
        PipelineFrameTiming timing;
        HudRenderTiming hud_timing;
        CedarVencInputBuffer direct_input;
        uint64_t frame_start = pipeline_now_us();
        uint64_t stage;
        uint32_t hud_checksum = 0U;
        bool sample_frame = pipeline_should_sample(options, frame);
        bool full_diagnostics = !options->benchmark || sample_frame;
        bool direct_input_acquired = false;

        memset(&timing, 0, sizeof(timing));
        memset(&hud_timing, 0, sizeof(hud_timing));
        memset(&direct_input, 0, sizeof(direct_input));
        pipeline_set_mock_state(&state, frame);
        dynamic_layer = hud_renderer_render_dynamic_to_surface(
            &hud_renderer, &state, true, &hud_surface, &hud_timing);
        if (dynamic_layer == NULL) {
            fprintf(stderr, "HUD render failed at frame %u\n", frame + 1U);
            goto cleanup;
        }
        timing.values[PIPELINE_STAGE_HUD_CLEAR] = hud_timing.clear_time_us;
        timing.values[PIPELINE_STAGE_HUD_DRAW] = hud_timing.draw_time_us;
        stage = pipeline_now_us();
        flush_buffer(&ion, &hud.ion);
        timing.values[PIPELINE_STAGE_HUD_CACHE_SYNC] =
            pipeline_now_us() - stage;
        timing.values[PIPELINE_STAGE_HUD_TOTAL] =
            hud_timing.total_time_us +
            timing.values[PIPELINE_STAGE_HUD_CACHE_SYNC];
        if (full_diagnostics) {
            hud_checksum = checksum32((const uint8_t *)hud.ion.memory,
                                       hud.ion.size);
            if (have_previous_hud_checksum &&
                hud_checksum == previous_hud_checksum) {
                fprintf(stderr, "HUD checksum did not change at frame %u\n",
                        frame + 1U);
                goto cleanup;
            }
            previous_hud_checksum = hud_checksum;
            have_previous_hud_checksum = true;
        }
        if (options->zero_copy) {
            /* Input ownership: FREE -> G2D_WRITING.  The input is returned
             * only after Cedar has consumed the frame below. */
            if (!pipeline_acquire_input(&encoder_api, encoder, &direct_input,
                                        options, frame, &cedar_input,
                                        &timing)) {
                goto cleanup;
            }
            direct_input_acquired = true;
        }
        if (options->mode == PIPELINE_MODE_F) {
            stage = pipeline_now_us();
            if (!run_bitblt(g2d_fd, yuv_format, camera.ion.fd, argb_format,
                            intermediate.ion.fd, 1280U, 720U, frame + 1U)) {
                if (direct_input_acquired) {
                    pipeline_return_input(&encoder_api, encoder, &direct_input,
                                          false);
                    direct_input_acquired = false;
                }
                goto cleanup;
            }
            flush_buffer(&ion, &intermediate.ion);
            timing.values[PIPELINE_STAGE_G2D_YUV_TO_ARGB] =
                pipeline_now_us() - stage;
        }
        stage = pipeline_now_us();
        if (!run_blend(g2d_fd, argb_format,
                       options->mode == PIPELINE_MODE_F ? intermediate.ion.fd :
                       camera.ion.fd, hud.ion.fd, argb_format,
                       options->zero_copy ? direct_input.nShareBufFd :
                       final_argb.ion.fd,
                       1280U, 720U, frame + 1U)) {
            if (direct_input_acquired) {
                pipeline_return_input(&encoder_api, encoder, &direct_input,
                                      false);
                direct_input_acquired = false;
            }
            goto cleanup;
        }
        timing.values[PIPELINE_STAGE_G2D_BLEND] = pipeline_now_us() - stage;
        if (!options->zero_copy) {
            flush_buffer(&ion, &final_argb.ion);
            if (full_diagnostics && !buffer_has_nonzero_data(&final_argb.ion)) {
                fprintf(stderr, "final ARGB remained zero at frame %u\n",
                        frame + 1U);
                goto cleanup;
            }
        }
        if (!pipeline_encode_frame(&encoder_api, encoder, &final_argb,
                                   options, round, frame, sample_frame,
                                   &cedar_input, &timing,
                                   direct_input_acquired ? &direct_input : NULL)) {
            direct_input_acquired = false;
            goto cleanup;
        }
        direct_input_acquired = false;
        timing.values[PIPELINE_STAGE_TOTAL] = pipeline_now_us() - frame_start;
        {
            uint64_t named_sum = 0U;
            PipelineStage named_stage;

            for (named_stage = 0;
                 named_stage < PIPELINE_STAGE_OTHER_UNACCOUNTED; ++named_stage) {
                if (named_stage == PIPELINE_STAGE_HUD_TOTAL ||
                    named_stage == PIPELINE_STAGE_HANDOFF_TOTAL) {
                    continue;
                }
                named_sum += timing.values[named_stage];
            }
            timing.values[PIPELINE_STAGE_OTHER_UNACCOUNTED] =
                timing.values[PIPELINE_STAGE_TOTAL] > named_sum ?
                timing.values[PIPELINE_STAGE_TOTAL] - named_sum : 0U;
        }
        pipeline_timing_record(&stats, &timing);
        if (frame == 0U || frame + 1U == options->frames ||
            (options->progress_interval != 0U &&
             (frame + 1U) % options->progress_interval == 0U)) {
            pipeline_print_timing(options->mode, frame + 1U, &timing);
        }
        resource_monitor_sample(&monitor, (uint64_t)frame + 1U, false);
    }
    (void)getrusage(RUSAGE_SELF, &cpu_end);
    wall_end = pipeline_now_us();
    {
        uint64_t cpu_us = (uint64_t)(cpu_end.ru_utime.tv_sec -
                                     cpu_start.ru_utime.tv_sec) * 1000000U +
                          (uint64_t)(cpu_end.ru_utime.tv_usec -
                                     cpu_start.ru_utime.tv_usec) +
                          (uint64_t)(cpu_end.ru_stime.tv_sec -
                                     cpu_start.ru_stime.tv_sec) * 1000000U +
                          (uint64_t)(cpu_end.ru_stime.tv_usec -
                                     cpu_start.ru_stime.tv_usec);

        pipeline_print_summary(options->mode, &stats, wall_end - wall_start,
                               cpu_us);
    }
    resource_monitor_finish(&monitor, stats.frames);
    monitor_finished = true;
    success = true;

cleanup:
    if (!monitor_finished && monitor.have_first) {
        resource_monitor_finish(&monitor, stats.frames);
    }
    if (g2d_fd >= 0) {
        close(g2d_fd);
        g2d_fd = -1;
    }
    hud_renderer_close(&hud_renderer);
    if (ion_opened) {
        pipeline_buffer_release(&ion, &final_argb, &monitor, stats.frames);
        pipeline_buffer_release(&ion, &intermediate, &monitor, stats.frames);
        pipeline_buffer_release(&ion, &hud, &monitor, stats.frames);
        pipeline_buffer_release(&ion, &camera, &monitor, stats.frames);
        ion.close();
    }
    if (encoder != NULL) {
        pipeline_close_encoder(&encoder_api, encoder, input_allocated,
                               &cedar_input);
        encoder = NULL;
    }
    pipeline_unload_encoder_api(&encoder_api);
    ion_unload(&ion);
    if (baseline_valid && resource_monitor_collect(&shutdown, stats.frames)) {
        printf("resource round=%u shutdown\n", round + 1U);
        resource_monitor_print(&shutdown);
        pipeline_print_resource_delta("round_teardown", &baseline, &shutdown);
        if (shutdown.have_dma_buf && baseline.have_dma_buf &&
            (shutdown.dma_buf_objects != baseline.dma_buf_objects ||
             shutdown.dma_buf_bytes != baseline.dma_buf_bytes)) {
            fprintf(stderr, "round %u teardown retained dma-buf objects\n",
                    round + 1U);
            success = false;
        }
        if (shutdown.fd_count != baseline.fd_count) {
            fprintf(stderr, "round %u teardown changed fd count\n", round + 1U);
            success = false;
        }
    }
    pipeline_timing_close(&stats);
    return success;
}

int main(int argc, char **argv)
{
    PipelineOptions options;
    unsigned int round;
    bool success = true;

    if (!pipeline_parse_options(argc, argv, &options)) {
        pipeline_usage(argv[0]);
        return EXIT_FAILURE;
    }
    for (round = 0U; round < options.rounds; ++round) {
        if (!pipeline_run(&options, round)) {
            success = false;
            break;
        }
    }
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
