#define _POSIX_C_SOURCE 200809L

#include "hud_renderer.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HUD_BENCH_WARMUP 100U
#define HUD_BENCH_SAMPLES 1000U

static uint64_t now_ns(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
    return (uint64_t)value.tv_sec * 1000000000ULL +
           (uint64_t)value.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static VehicleState make_state(void)
{
    VehicleState state;

    memset(&state, 0, sizeof(state));
    state.valid_mask = VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_LV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT |
                       VEHICLE_STATE_VALID_ACC_X_MPS2 |
                       VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                       VEHICLE_STATE_VALID_DRIVE_STATE;
    state.motor_rpm = 8600.0;
    state.hv_voltage_v = 356.0;
    state.lv_voltage_v = 13.6;
    state.hv_soc_percent = 78.0;
    state.throttle_percent = 65.0;
    state.brake_percent = 8.0;
    state.acc_x = -0.6 * 9.80665;
    state.acc_y = 0.8 * 9.80665;
    state.drive_state = VEHICLE_DRIVE_STATE_DRIVE;
    state.lap_valid = true;
    state.lap_number = 2U;
    state.lap_time_s = 83.486;
    state.best_lap_time_s = 82.910;
    state.delta_time_s = -0.42;
    state.gps_valid = true;
    state.gps_fix_type = 4U;
    state.can_valid = true;
    return state;
}

static int benchmark_style(const char *asset_dir, HudStyle style,
                           const VehicleState *state)
{
    HudRenderer renderer;
    HudSurface surface;
    uint8_t *pixels;
    uint64_t *samples;
    uint64_t start;
    uint64_t elapsed;
    uint64_t total = 0U;
    uint64_t timing_total = 0U;
    uint64_t timing_draw = 0U;
    uint64_t timing_rpm = 0U;
    uint64_t timing_pedal = 0U;
    uint64_t timing_g = 0U;
    uint64_t timing_text = 0U;
    uint64_t cold_full_copy_clear = 0U;
    uint32_t index;

    memset(&renderer, 0, sizeof(renderer));
    pixels = calloc(HUD_BASE_WIDTH * HUD_BASE_HEIGHT, 4U);
    samples = calloc(HUD_BENCH_SAMPLES, sizeof(*samples));
    if (pixels == NULL || samples == NULL) {
        free(samples);
        free(pixels);
        fprintf(stderr, "benchmark: allocation failed: %s\n", strerror(errno));
        return -1;
    }
    if (hud_renderer_init_style(&renderer, HUD_BASE_WIDTH, HUD_BASE_HEIGHT,
                                style, asset_dir, HUD_PIXEL_FORMAT_ARGB8888) < 0) {
        fprintf(stderr, "benchmark: cannot initialize %s HUD\n",
                hud_style_name(style));
        free(samples);
        free(pixels);
        return -1;
    }
    surface = (HudSurface){ pixels, HUD_BASE_WIDTH, HUD_BASE_HEIGHT,
                            HUD_BASE_WIDTH * 4U, HUD_PIXEL_FORMAT_ARGB8888 };
    {
        HudRenderTiming cold_timing;

        if (hud_renderer_render_frame_to_surface(&renderer, state, false,
                                                  &surface, &cold_timing) == NULL) {
            hud_renderer_close(&renderer);
            free(samples);
            free(pixels);
            return -1;
        }
        cold_full_copy_clear = cold_timing.clear_time_us;
    }
    for (index = 0U; index < HUD_BENCH_WARMUP; ++index) {
        (void)hud_renderer_render_frame_to_surface(&renderer, state, false,
                                                    &surface, NULL);
    }
    start = now_ns();
    for (index = 0U; index < HUD_BENCH_SAMPLES; ++index) {
        HudRenderTiming timing;
        uint64_t frame_start = now_ns();

        if (hud_renderer_render_frame_to_surface(&renderer, state, false,
                                                  &surface, &timing) == NULL) {
            fprintf(stderr, "benchmark: %s render failed\n",
                    hud_style_name(style));
            hud_renderer_close(&renderer);
            free(samples);
            free(pixels);
            return -1;
        }
        samples[index] = now_ns() - frame_start;
        timing_total += timing.total_time_us;
        timing_draw += timing.draw_time_us;
        timing_rpm += timing.rpm_time_us;
        timing_pedal += timing.pedal_time_us;
        timing_g += timing.g_time_us;
        timing_text += timing.text_time_us;
    }
    elapsed = now_ns() - start;
    qsort(samples, HUD_BENCH_SAMPLES, sizeof(*samples), compare_u64);
    for (index = 0U; index < HUD_BENCH_SAMPLES; ++index) total += samples[index];
    printf("hud_style=%s samples=%u wall_avg_us=%.2f internal_avg_us=%.2f "
           "cold_full_copy_clear_us=%llu clear_avg_us=%.2f draw_avg_us=%.2f rpm_avg_us=%.2f "
           "pedal_avg_us=%.2f g_avg_us=%.2f text_avg_us=%.2f "
           "p95_us=%.2f p99_us=%.2f resident_bytes=%zu\n",
           hud_style_name(style), HUD_BENCH_SAMPLES,
           (double)elapsed / (double)HUD_BENCH_SAMPLES / 1000.0,
           (double)timing_total / (double)HUD_BENCH_SAMPLES,
           (unsigned long long)cold_full_copy_clear,
           (double)(timing_total - timing_draw) /
               (double)HUD_BENCH_SAMPLES,
           (double)timing_draw / (double)HUD_BENCH_SAMPLES,
           (double)timing_rpm / (double)HUD_BENCH_SAMPLES,
           (double)timing_pedal / (double)HUD_BENCH_SAMPLES,
           (double)timing_g / (double)HUD_BENCH_SAMPLES,
           (double)timing_text / (double)HUD_BENCH_SAMPLES,
           (double)samples[(HUD_BENCH_SAMPLES * 95U) / 100U] / 1000.0,
           (double)samples[(HUD_BENCH_SAMPLES * 99U) / 100U] / 1000.0,
           hud_renderer_resident_memory(&renderer));
    (void)total;
    hud_renderer_close(&renderer);
    free(samples);
    free(pixels);
    return 0;
}

int main(int argc, char **argv)
{
    const char *asset_dir = argc > 1 ? argv[1] : "assets/hud";
    VehicleState state = make_state();

    if (benchmark_style(asset_dir, HUD_STYLE_LEGACY, &state) < 0) return 1;
    if (benchmark_style(asset_dir, HUD_STYLE_PNG, &state) < 0) return 1;
    return 0;
}
