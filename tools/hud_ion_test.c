#define _POSIX_C_SOURCE 200809L

/* Standalone Phase A/B HUD-to-ION test.  It never starts Cedar encoding. */
#define G2D_SMOKE_TEST_NO_MAIN
#define G2D_SMOKE_TEST_NO_UNUSED_HELPERS
#include "g2d_smoke_test.c"

#include "hud_renderer.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <sys/resource.h>
#include <time.h>

#define HUD_ION_WIDTH 1280U
#define HUD_ION_HEIGHT 720U
#define HUD_ION_BPP 4U
#define HUD_ION_STRIDE (HUD_ION_WIDTH * HUD_ION_BPP)
#define HUD_ION_BYTES (HUD_ION_STRIDE * HUD_ION_HEIGHT)
#define HUD_ION_DEFAULT_FRAMES 100U
#define HUD_ION_DEFAULT_MONITOR_INTERVAL 100U
#define HUD_ION_DEFAULT_SAMPLE_INTERVAL 1000U
#define HUD_ION_DEFAULT_PROGRESS_INTERVAL 100U
#define HUD_ION_G2D_DEVICE "/dev/g2d"

typedef enum {
    HUD_ION_PHASE_DIRECT = 0,
    HUD_ION_PHASE_BLEND
} HudIonPhase;

typedef struct {
    HudIonPhase phase;
    unsigned int frames;
    unsigned int monitor_interval;
    unsigned int sample_interval;
    unsigned int progress_interval;
    const char *output_prefix;
} HudIonOptions;

typedef struct {
    const char *name;
    IonBuffer ion;
    uint32_t physical;
} HudIonBuffer;

typedef enum {
    HUD_ION_STAGE_CLEAR = 0,
    HUD_ION_STAGE_DRAW,
    HUD_ION_STAGE_CACHE_SYNC,
    HUD_ION_STAGE_YUV_TO_ARGB,
    HUD_ION_STAGE_BLEND,
    HUD_ION_STAGE_TOTAL,
    HUD_ION_STAGE_COUNT
} HudIonStage;

typedef struct {
    uint64_t values[HUD_ION_STAGE_COUNT];
} HudIonFrameTiming;

typedef struct {
    uint64_t frames;
    uint64_t *samples[HUD_ION_STAGE_COUNT];
} HudIonTimingStats;

typedef uint32_t (*HudIonVir2PhyFn)(void *memory);

static const char *const hud_ion_stage_names[HUD_ION_STAGE_COUNT] = {
    "hud_clear", "hud_draw", "hud_cache_sync", "g2d_yuv_to_argb",
    "g2d_blend", "total_frame"
};

static uint64_t hud_ion_now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0U;
    }
    return (uint64_t)timestamp.tv_sec * 1000000U +
           (uint64_t)timestamp.tv_nsec / 1000U;
}

static const char *hud_ion_phase_name(HudIonPhase phase)
{
    return phase == HUD_ION_PHASE_BLEND ? "B" : "A";
}

static bool hud_ion_parse_phase(const char *value, HudIonPhase *phase)
{
    if (strcmp(value, "A") == 0 || strcmp(value, "a") == 0 ||
        strcmp(value, "hud") == 0) {
        *phase = HUD_ION_PHASE_DIRECT;
        return true;
    }
    if (strcmp(value, "B") == 0 || strcmp(value, "b") == 0 ||
        strcmp(value, "blend") == 0) {
        *phase = HUD_ION_PHASE_BLEND;
        return true;
    }
    return false;
}

static bool hud_ion_parse_uint(const char *text, unsigned int *value,
                               bool allow_zero)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX ||
        (!allow_zero && parsed == 0UL)) {
        return false;
    }
    *value = (unsigned int)parsed;
    return true;
}

static void hud_ion_usage(const char *program)
{
    printf("Usage: %s [--phase A|B] [--frames N] "
           "[--monitor-interval N] [--sample-interval N] "
           "[--progress-interval N] [--output-prefix PATH]\n", program);
    printf("  A/direct: VehicleState -> HUD -> persistent ION ARGB8888\n");
    printf("  B/blend: YUV420P -> ARGB + direct ION HUD -> G2D BLD_H\n");
    printf("  defaults: phase A, %u frames, monitor every %u, "
           "samples every %u\n", HUD_ION_DEFAULT_FRAMES,
           HUD_ION_DEFAULT_MONITOR_INTERVAL, HUD_ION_DEFAULT_SAMPLE_INTERVAL);
}

static bool hud_ion_parse_options(int argc, char **argv,
                                  HudIonOptions *options)
{
    int index;

    memset(options, 0, sizeof(*options));
    options->phase = HUD_ION_PHASE_DIRECT;
    options->frames = HUD_ION_DEFAULT_FRAMES;
    options->monitor_interval = HUD_ION_DEFAULT_MONITOR_INTERVAL;
    options->sample_interval = HUD_ION_DEFAULT_SAMPLE_INTERVAL;
    options->progress_interval = HUD_ION_DEFAULT_PROGRESS_INTERVAL;
    options->output_prefix = "/tmp/hud_ion";
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 ||
            strcmp(argv[index], "-h") == 0) {
            hud_ion_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(argv[index], "--phase") == 0 && index + 1 < argc) {
            if (!hud_ion_parse_phase(argv[++index], &options->phase)) {
                fprintf(stderr, "invalid --phase value: %s\n", argv[index]);
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
            if (!hud_ion_parse_uint(argv[++index], &options->frames, false)) {
                fprintf(stderr, "invalid --frames value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--monitor-interval") == 0 &&
            index + 1 < argc) {
            if (!hud_ion_parse_uint(argv[++index], &options->monitor_interval,
                                    true)) {
                fprintf(stderr, "invalid --monitor-interval value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--sample-interval") == 0 &&
            index + 1 < argc) {
            if (!hud_ion_parse_uint(argv[++index], &options->sample_interval,
                                    true)) {
                fprintf(stderr, "invalid --sample-interval value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--progress-interval") == 0 &&
            index + 1 < argc) {
            if (!hud_ion_parse_uint(argv[++index], &options->progress_interval,
                                    true)) {
                fprintf(stderr, "invalid --progress-interval value\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--output-prefix") == 0 && index + 1 < argc) {
            options->output_prefix = argv[++index];
            continue;
        }
        fprintf(stderr, "unknown or incomplete option: %s\n", argv[index]);
        return false;
    }
    return true;
}

static void hud_ion_mock_state(VehicleState *state, unsigned int frame)
{
    memset(state, 0, sizeof(*state));
    state->valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                        VEHICLE_STATE_VALID_MOTOR_RPM |
                        VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                        VEHICLE_STATE_VALID_ACC_X_MPS2 |
                        VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                        VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                        VEHICLE_STATE_VALID_BRAKE_PERCENT |
                        VEHICLE_STATE_VALID_DRIVE_STATE;
    state->lap_valid = true;
    state->lap_number = frame + 1U;
    state->lap_time_s = 87.25 + (double)(frame % 900U) * 0.01;
    state->best_lap_time_s = 86.91;
    state->delta_time_s = ((int)(frame % 41U) - 20) * 0.006;
    state->speed_kmh = 72.0 + (double)(frame % 120U) * 0.25;
    state->motor_rpm = 2800.0 + (double)(frame % 180U) * 37.0;
    state->hv_soc_percent = 78.0 - (double)(frame % 500U) * 0.01;
    state->acc_x = ((int)(frame % 30U) - 15) * 0.08;
    state->acc_y = ((int)(frame % 50U) - 25) * 0.05;
    state->throttle_percent = 34.0 + (double)(frame % 40U) * 0.5;
    state->brake_percent = frame % 75U < 8U ? 18.0 : 0.0;
    state->drive_state = VEHICLE_DRIVE_STATE_DRIVE;
}

static bool hud_ion_buffer_alloc(const IonApi *ion, HudIonBuffer *buffer,
                                 const char *name, size_t size)
{
    void *symbol;
    HudIonVir2PhyFn vir2phy = NULL;

    memset(buffer, 0, sizeof(*buffer));
    buffer->name = name;
    buffer->ion.fd = -1;
    if (!ion_buffer_alloc(ion, &buffer->ion, size)) {
        fprintf(stderr, "ION allocation failed name=%s size=%zu\n", name,
                size);
        return false;
    }
    symbol = dlsym(ion->library, "sunxi_ion_alloc_vir2phy_cpu");
    if (symbol != NULL) {
        memcpy(&vir2phy, &symbol, sizeof(vir2phy));
        buffer->physical = vir2phy(buffer->ion.memory);
    }
    printf("buffer name=%s size=%zu stride=%u fd=%d virtual=%p "
           "physical=0x%08" PRIx32 " owner=ION persistent\n", name, size,
           name != NULL && strcmp(name, "hud_argb") == 0 ? HUD_ION_STRIDE : 0U,
           buffer->ion.fd, buffer->ion.memory, buffer->physical);
    return true;
}

static void hud_ion_buffer_free(const IonApi *ion, HudIonBuffer *buffer)
{
    if (buffer->ion.memory != NULL) {
        printf("buffer release name=%s fd=%d owner=ION\n", buffer->name,
               buffer->ion.fd);
        ion_buffer_free(ion, &buffer->ion);
    }
}

static uint32_t hud_ion_checksum32(const uint8_t *data, size_t size)
{
    size_t index;
    uint32_t checksum = 2166136261U;

    for (index = 0U; index < size; ++index) {
        checksum ^= data[index];
        checksum *= 16777619U;
    }
    return checksum;
}

static bool hud_ion_write_bmp(const char *path, const IonBuffer *buffer,
                              unsigned int width, unsigned int height,
                              unsigned int stride)
{
    uint8_t header[54];
    FILE *file;
    unsigned int row;
    uint32_t image_size = stride * height;
    uint32_t file_size = 54U + image_size;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (uint8_t)file_size;
    header[3] = (uint8_t)(file_size >> 8U);
    header[4] = (uint8_t)(file_size >> 16U);
    header[5] = (uint8_t)(file_size >> 24U);
    header[10] = 54U;
    header[14] = 40U;
    header[18] = (uint8_t)width;
    header[19] = (uint8_t)(width >> 8U);
    header[20] = (uint8_t)(width >> 16U);
    header[21] = (uint8_t)(width >> 24U);
    header[22] = (uint8_t)height;
    header[23] = (uint8_t)(height >> 8U);
    header[24] = (uint8_t)(height >> 16U);
    header[25] = (uint8_t)(height >> 24U);
    header[26] = 1U;
    header[28] = 32U;
    header[34] = (uint8_t)image_size;
    header[35] = (uint8_t)(image_size >> 8U);
    header[36] = (uint8_t)(image_size >> 16U);
    header[37] = (uint8_t)(image_size >> 24U);
    file = fopen(path, "wb");
    if (file == NULL || fwrite(header, 1U, sizeof(header), file) !=
                            sizeof(header)) {
        if (file != NULL) {
            fclose(file);
        }
        fprintf(stderr, "cannot write HUD sample %s: %s\n", path,
                strerror(errno));
        return false;
    }
    for (row = 0U; row < height; ++row) {
        const uint8_t *source = (const uint8_t *)buffer->memory +
                                (size_t)(height - row - 1U) * stride;
        if (fwrite(source, 1U, stride, file) != stride) {
            fclose(file);
            fprintf(stderr, "cannot write HUD sample %s: %s\n", path,
                    strerror(errno));
            return false;
        }
    }
    return fclose(file) == 0;
}

static bool hud_ion_should_sample(const HudIonOptions *options,
                                  unsigned int frame)
{
    unsigned int one_based = frame + 1U;

    return frame == 0U || one_based == options->frames ||
           (options->sample_interval != 0U &&
            one_based % options->sample_interval == 0U);
}

static bool hud_ion_timing_init(HudIonTimingStats *stats,
                                unsigned int frames)
{
    HudIonStage stage;

    memset(stats, 0, sizeof(*stats));
    for (stage = 0; stage < HUD_ION_STAGE_COUNT; ++stage) {
        stats->samples[stage] = calloc(frames, sizeof(uint64_t));
        if (stats->samples[stage] == NULL) {
            HudIonStage release_stage;

            for (release_stage = 0; release_stage <= stage; ++release_stage) {
                free(stats->samples[release_stage]);
                stats->samples[release_stage] = NULL;
            }
            return false;
        }
    }
    return true;
}

static void hud_ion_timing_close(HudIonTimingStats *stats)
{
    HudIonStage stage;

    for (stage = 0; stage < HUD_ION_STAGE_COUNT; ++stage) {
        free(stats->samples[stage]);
        stats->samples[stage] = NULL;
    }
}

static int hud_ion_compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t hud_ion_p95(const HudIonTimingStats *stats, HudIonStage stage)
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
          hud_ion_compare_u64);
    position = (stats->frames * 95U + 99U) / 100U;
    if (position == 0U) {
        position = 1U;
    }
    result = sorted[position - 1U];
    free(sorted);
    return result;
}

static void hud_ion_record(HudIonTimingStats *stats,
                           const HudIonFrameTiming *timing)
{
    HudIonStage stage;
    uint64_t frame = stats->frames;

    for (stage = 0; stage < HUD_ION_STAGE_COUNT; ++stage) {
        stats->samples[stage][frame] = timing->values[stage];
    }
    stats->frames++;
}

static void hud_ion_print_timing(HudIonPhase phase, unsigned int frame,
                                 const HudIonFrameTiming *timing)
{
    printf("[HUD-%s] frame=%u clear=%.3fms draw=%.3fms sync=%.3fms "
           "yuv_to_argb=%.3fms BLD_H=%.3fms total=%.3fms\n",
           hud_ion_phase_name(phase), frame,
           timing->values[HUD_ION_STAGE_CLEAR] / 1000.0,
           timing->values[HUD_ION_STAGE_DRAW] / 1000.0,
           timing->values[HUD_ION_STAGE_CACHE_SYNC] / 1000.0,
           timing->values[HUD_ION_STAGE_YUV_TO_ARGB] / 1000.0,
           timing->values[HUD_ION_STAGE_BLEND] / 1000.0,
           timing->values[HUD_ION_STAGE_TOTAL] / 1000.0);
}

static void hud_ion_print_summary(HudIonPhase phase,
                                  const HudIonTimingStats *stats,
                                  uint64_t wall_us, uint64_t cpu_us)
{
    HudIonStage stage;
    double frames = (double)stats->frames;

    printf("[HUD-%s] PASS: frames=%" PRIu64 " wall=%.2fs FPS=%.2f "
           "cpu=%.2fms cpu_util=%.1f%%\n", hud_ion_phase_name(phase),
           stats->frames, wall_us / 1000000.0,
           wall_us != 0U ? frames * 1000000.0 / (double)wall_us : 0.0,
           cpu_us / 1000.0,
           wall_us != 0U ? (double)cpu_us * 100.0 / (double)wall_us : 0.0);
    for (stage = 0; stage < HUD_ION_STAGE_COUNT; ++stage) {
        uint64_t sum = 0U;
        uint64_t max = 0U;
        uint64_t frame;

        for (frame = 0U; frame < stats->frames; ++frame) {
            sum += stats->samples[stage][frame];
            if (stats->samples[stage][frame] > max) {
                max = stats->samples[stage][frame];
            }
        }
        printf("[HUD-%s] stage %-16s avg=%7.3fms p95=%7.3fms "
               "max=%7.3fms\n", hud_ion_phase_name(phase),
               hud_ion_stage_names[stage],
               frames != 0.0 ? (double)sum / frames / 1000.0 : 0.0,
               hud_ion_p95(stats, stage) / 1000.0, max / 1000.0);
    }
}

static void hud_ion_print_resource_delta(const ResourceSample *before,
                                         const ResourceSample *after)
{
    int64_t dma_objects = 0;
    int64_t dma_bytes = 0;

    if (before->have_dma_buf && after->have_dma_buf) {
        dma_objects = (int64_t)after->dma_buf_objects -
                      (int64_t)before->dma_buf_objects;
        dma_bytes = (int64_t)after->dma_buf_bytes -
                    (int64_t)before->dma_buf_bytes;
    }
    printf("resource teardown delta dma_buf_count=%" PRId64
           " dma_buf_bytes=%" PRId64 " fd=%d rss_kb=%ld cma_free_kb=%" PRId64
           "\n", dma_objects, dma_bytes,
           after->fd_count - before->fd_count,
           after->rss_kb - before->rss_kb,
           (int64_t)before->cma_free_kb - (int64_t)after->cma_free_kb);
}

static bool hud_ion_run(const HudIonOptions *options)
{
    const g2d_fmt_enh argb_format = G2D_FORMAT_ARGB8888;
    const g2d_fmt_enh yuv_format = G2D_FORMAT_YUV420_PLANAR;
    HudIonBuffer hud;
    HudIonBuffer yuv_source;
    HudIonBuffer argb_camera;
    HudIonBuffer final_argb;
    HudRenderer renderer;
    HudSurface surface;
    IonApi ion;
    ResourceMonitor monitor;
    ResourceSample baseline;
    ResourceSample shutdown;
    HudIonTimingStats stats;
    struct rusage cpu_start;
    struct rusage cpu_end;
    int g2d_fd = -1;
    bool ion_opened = false;
    bool renderer_initialized = false;
    bool success = false;
    bool baseline_valid = false;
    bool monitor_finished = false;
    uint32_t previous_checksum = 0U;
    bool have_previous_checksum = false;
    uint64_t wall_start;
    uint64_t wall_end;
    unsigned int frame;

    memset(&hud, 0, sizeof(hud));
    memset(&yuv_source, 0, sizeof(yuv_source));
    memset(&argb_camera, 0, sizeof(argb_camera));
    memset(&final_argb, 0, sizeof(final_argb));
    memset(&renderer, 0, sizeof(renderer));
    memset(&surface, 0, sizeof(surface));
    memset(&ion, 0, sizeof(ion));
    hud.ion.fd = -1;
    yuv_source.ion.fd = -1;
    argb_camera.ion.fd = -1;
    final_argb.ion.fd = -1;
    memset(&stats, 0, sizeof(stats));
    resource_monitor_init(&monitor, options->monitor_interval);
    if (resource_monitor_collect(&baseline, 0U)) {
        baseline_valid = true;
        printf("resource baseline phase=%s\n", hud_ion_phase_name(options->phase));
        resource_monitor_print(&baseline);
    }
    if (!hud_ion_timing_init(&stats, options->frames)) {
        fprintf(stderr, "timing allocation failed\n");
        return false;
    }
    (void)getrusage(RUSAGE_SELF, &cpu_start);
    wall_start = hud_ion_now_us();

    if (!ion_load(&ion) || ion.open() != 0) {
        fprintf(stderr, "cannot open board ION allocator\n");
        goto cleanup;
    }
    ion_opened = true;
    if (!hud_ion_buffer_alloc(&ion, &hud, "hud_argb", HUD_ION_BYTES)) {
        goto cleanup;
    }
    memset(hud.ion.memory, 0, hud.ion.size);
    flush_buffer(&ion, &hud.ion);
    if (options->phase == HUD_ION_PHASE_BLEND) {
        if (!hud_ion_buffer_alloc(&ion, &yuv_source, "yuv_source",
                                  image_size_for_format(yuv_format,
                                                         HUD_ION_WIDTH,
                                                         HUD_ION_HEIGHT)) ||
            !hud_ion_buffer_alloc(&ion, &argb_camera, "argb_camera",
                                  HUD_ION_BYTES) ||
            !hud_ion_buffer_alloc(&ion, &final_argb, "argb_final",
                                  HUD_ION_BYTES)) {
            goto cleanup;
        }
        fill_yuv420_pattern((uint8_t *)yuv_source.ion.memory, HUD_ION_WIDTH,
                            HUD_ION_HEIGHT);
        memset(argb_camera.ion.memory, 0, argb_camera.ion.size);
        memset(final_argb.ion.memory, 0, final_argb.ion.size);
        flush_buffer(&ion, &yuv_source.ion);
        flush_buffer(&ion, &argb_camera.ion);
        flush_buffer(&ion, &final_argb.ion);
        g2d_fd = open(HUD_ION_G2D_DEVICE, O_RDWR | O_CLOEXEC);
        if (g2d_fd < 0) {
            fprintf(stderr, "cannot open %s: %s\n", HUD_ION_G2D_DEVICE,
                    strerror(errno));
            goto cleanup;
        }
    }
    surface.pixels = (uint8_t *)hud.ion.memory;
    surface.width = HUD_ION_WIDTH;
    surface.height = HUD_ION_HEIGHT;
    surface.stride_bytes = HUD_ION_STRIDE;
    surface.pixel_format = HUD_PIXEL_FORMAT_ARGB8888;
    if (hud_renderer_init_external(&renderer, HUD_ION_WIDTH, HUD_ION_HEIGHT) !=
        0) {
        fprintf(stderr, "hud_renderer_init_external failed\n");
        goto cleanup;
    }
    renderer_initialized = true;
    resource_monitor_sample(&monitor, 0U, true);
    printf("HUD ION phase=%s address_mode=dma-buf-fd format=ARGB8888 "
           "size=%u stride=%u frames=%u\n", hud_ion_phase_name(options->phase),
           HUD_ION_BYTES, HUD_ION_STRIDE, options->frames);

    for (frame = 0U; frame < options->frames; ++frame) {
        VehicleState state;
        HudRenderTiming render_timing;
        HudIonFrameTiming timing;
        uint64_t frame_start = hud_ion_now_us();
        uint64_t stage;
        uint32_t checksum;
        bool sample = hud_ion_should_sample(options, frame);

        memset(&render_timing, 0, sizeof(render_timing));
        memset(&timing, 0, sizeof(timing));
        hud_ion_mock_state(&state, frame);
        if (hud_renderer_render_dynamic_to_surface(
                &renderer, &state, true, &surface, &render_timing) == NULL) {
            fprintf(stderr, "direct HUD render failed at frame %u\n",
                    frame + 1U);
            goto cleanup;
        }
        timing.values[HUD_ION_STAGE_CLEAR] = render_timing.clear_time_us;
        timing.values[HUD_ION_STAGE_DRAW] = render_timing.draw_time_us;
        stage = hud_ion_now_us();
        flush_buffer(&ion, &hud.ion);
        timing.values[HUD_ION_STAGE_CACHE_SYNC] = hud_ion_now_us() - stage;
        if (options->phase == HUD_ION_PHASE_BLEND) {
            stage = hud_ion_now_us();
            if (!run_bitblt(g2d_fd, yuv_format, yuv_source.ion.fd,
                            argb_format, argb_camera.ion.fd, HUD_ION_WIDTH,
                            HUD_ION_HEIGHT, frame + 1U)) {
                goto cleanup;
            }
            flush_buffer(&ion, &argb_camera.ion);
            timing.values[HUD_ION_STAGE_YUV_TO_ARGB] =
                hud_ion_now_us() - stage;
            stage = hud_ion_now_us();
            if (!run_blend(g2d_fd, argb_format, argb_camera.ion.fd, hud.ion.fd,
                           argb_format, final_argb.ion.fd, HUD_ION_WIDTH,
                           HUD_ION_HEIGHT, frame + 1U)) {
                goto cleanup;
            }
            flush_buffer(&ion, &final_argb.ion);
            timing.values[HUD_ION_STAGE_BLEND] = hud_ion_now_us() - stage;
            if (sample && !buffer_has_nonzero_data(&final_argb.ion)) {
                fprintf(stderr, "BLD_H output remained zero at frame %u\n",
                        frame + 1U);
                goto cleanup;
            }
        }
        timing.values[HUD_ION_STAGE_TOTAL] = hud_ion_now_us() - frame_start;
        if (sample) {
            checksum = hud_ion_checksum32((const uint8_t *)hud.ion.memory,
                                          HUD_ION_BYTES);
            if (have_previous_checksum && checksum == previous_checksum) {
                fprintf(stderr, "HUD checksum did not change at frame %u\n",
                        frame + 1U);
                goto cleanup;
            }
            previous_checksum = checksum;
            have_previous_checksum = true;
            {
                char path[512];
                int written = snprintf(path, sizeof(path), "%s_%s_f%u.bmp",
                                       options->output_prefix,
                                       hud_ion_phase_name(options->phase),
                                       frame + 1U);
                if (written < 0 || (size_t)written >= sizeof(path) ||
                    !hud_ion_write_bmp(path, &hud.ion, HUD_ION_WIDTH,
                                       HUD_ION_HEIGHT, HUD_ION_STRIDE)) {
                    goto cleanup;
                }
                printf("HUD sample frame=%u path=%s checksum=0x%08" PRIx32
                       "\n", frame + 1U, path, checksum);
                if (options->phase == HUD_ION_PHASE_BLEND) {
                    char final_path[512];
                    int final_written = snprintf(
                        final_path, sizeof(final_path), "%s_%s_f%u_final.bmp",
                        options->output_prefix,
                        hud_ion_phase_name(options->phase), frame + 1U);
                    if (final_written < 0 ||
                        (size_t)final_written >= sizeof(final_path) ||
                        !hud_ion_write_bmp(final_path, &final_argb.ion,
                                           HUD_ION_WIDTH, HUD_ION_HEIGHT,
                                           HUD_ION_STRIDE)) {
                        goto cleanup;
                    }
                    printf("BLD sample frame=%u path=%s\n", frame + 1U,
                           final_path);
                }
            }
        }
        hud_ion_record(&stats, &timing);
        if (frame == 0U || frame + 1U == options->frames ||
            (options->progress_interval != 0U &&
             (frame + 1U) % options->progress_interval == 0U)) {
            hud_ion_print_timing(options->phase, frame + 1U, &timing);
        }
        resource_monitor_sample(&monitor, frame + 1U, false);
    }
    (void)getrusage(RUSAGE_SELF, &cpu_end);
    wall_end = hud_ion_now_us();
    {
        uint64_t cpu_us = (uint64_t)(cpu_end.ru_utime.tv_sec -
                                     cpu_start.ru_utime.tv_sec) * 1000000U +
                          (uint64_t)(cpu_end.ru_utime.tv_usec -
                                     cpu_start.ru_utime.tv_usec) +
                          (uint64_t)(cpu_end.ru_stime.tv_sec -
                                     cpu_start.ru_stime.tv_sec) * 1000000U +
                          (uint64_t)(cpu_end.ru_stime.tv_usec -
                                     cpu_start.ru_stime.tv_usec);
        hud_ion_print_summary(options->phase, &stats, wall_end - wall_start,
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
    }
    if (renderer_initialized) {
        hud_renderer_close(&renderer);
    }
    if (ion_opened) {
        hud_ion_buffer_free(&ion, &final_argb);
        hud_ion_buffer_free(&ion, &argb_camera);
        hud_ion_buffer_free(&ion, &yuv_source);
        hud_ion_buffer_free(&ion, &hud);
        ion.close();
    }
    ion_unload(&ion);
    if (baseline_valid && resource_monitor_collect(&shutdown, stats.frames)) {
        printf("resource shutdown phase=%s\n", hud_ion_phase_name(options->phase));
        resource_monitor_print(&shutdown);
        hud_ion_print_resource_delta(&baseline, &shutdown);
        if (shutdown.have_dma_buf && baseline.have_dma_buf &&
            (shutdown.dma_buf_objects != baseline.dma_buf_objects ||
             shutdown.dma_buf_bytes != baseline.dma_buf_bytes)) {
            fprintf(stderr, "HUD ION phase retained dma-buf objects\n");
            success = false;
        }
        if (shutdown.fd_count != baseline.fd_count) {
            fprintf(stderr, "HUD ION phase changed fd count\n");
            success = false;
        }
    }
    hud_ion_timing_close(&stats);
    return success;
}

int main(int argc, char **argv)
{
    HudIonOptions options;

    if (!hud_ion_parse_options(argc, argv, &options)) {
        hud_ion_usage(argv[0]);
        return EXIT_FAILURE;
    }
    return hud_ion_run(&options) ? EXIT_SUCCESS : EXIT_FAILURE;
}
