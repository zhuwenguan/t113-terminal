#define _POSIX_C_SOURCE 200809L

#include "hud_render.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_WIDTH 800
#define BENCH_HEIGHT 600
#define BENCH_STRIDE (BENCH_WIDTH * 4 + 64)
#define BENCH_CLEAR_ITERATIONS 1000U
#define BENCH_COPY_ITERATIONS 1000U
#define BENCH_RENDER_ITERATIONS 100U

typedef struct {
    const char *name;
    VehicleState state;
    bool recording;
} BenchCase;

static uint64_t bench_now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) return 0U;
    return (uint64_t)timestamp.tv_sec * 1000000ULL +
           (uint64_t)timestamp.tv_nsec / 1000ULL;
}

static VehicleState bench_state(void)
{
    VehicleState state;

    memset(&state, 0, sizeof(state));
    state.valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                       VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_HV_CURRENT_A |
                       VEHICLE_STATE_VALID_HV_POWER_W |
                       VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                       VEHICLE_STATE_VALID_ACC_X_MPS2 |
                       VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT |
                       VEHICLE_STATE_VALID_DRIVE_STATE;
    state.speed_kmh = 86.0;
    state.motor_rpm = 8200.0;
    state.throttle_percent = 63.0;
    state.brake_percent = 18.0;
    state.hv_voltage_v = 398.0;
    state.hv_current_a = 81.0;
    state.hv_power_w = 32200.0;
    state.hv_soc_percent = 72.0;
    state.acc_x = 1.2 * 9.80665;
    state.acc_y = -0.4 * 9.80665;
    state.drive_state = VEHICLE_DRIVE_STATE_READY;
    state.can_valid = true;
    state.gps_valid = true;
    state.gps_fix_type = 4U;
    state.lap_valid = true;
    state.lap_number = 3U;
    state.lap_time_s = 62.314;
    state.best_lap_time_s = 61.900;
    state.delta_time_s = 0.214;
    state.accel_50m_time_s = 3.82;
    state.accel_time_s = 5.41;
    return state;
}

static void bench_make_cases(BenchCase cases[5])
{
    VehicleState state = bench_state();

    cases[0] = (BenchCase){ "normal", state, false };
    state.speed_kmh = 999.0;
    state.motor_rpm = 99999.0;
    state.hv_voltage_v = 9999.0;
    state.hv_current_a = 9999.0;
    state.hv_power_w = 999999.0;
    state.hv_soc_percent = 100.0;
    state.lap_number = 999U;
    state.lap_time_s = 5999.999;
    state.best_lap_time_s = 5999.998;
    state.delta_time_s = -99.999;
    cases[1] = (BenchCase){ "long_values", state, true };
    state.accel_50m_time_s = 1.23;
    state.accel_time_s = 2.34;
    state.delta_time_s = -0.42;
    cases[2] = (BenchCase){ "accel_popup", state, false };
    state.speed_kmh = 300.0;
    state.motor_rpm = 10000.0;
    state.throttle_percent = 100.0;
    state.brake_percent = 100.0;
    state.hv_current_a = 250.0;
    state.hv_power_w = 100000.0;
    cases[3] = (BenchCase){ "high_bars", state, true };
    memset(&state, 0, sizeof(state));
    state.valid_mask = UINT32_MAX;
    state.lap_valid = true;
    state.speed_kmh = NAN;
    state.motor_rpm = INFINITY;
    state.hv_voltage_v = -INFINITY;
    state.hv_current_a = NAN;
    state.hv_power_w = NAN;
    state.hv_soc_percent = INFINITY;
    state.throttle_percent = -100.0;
    state.brake_percent = 10000.0;
    state.lap_time_s = NAN;
    state.delta_time_s = INFINITY;
    state.acc_x = NAN;
    state.acc_y = NAN;
    state.accel_50m_time_s = NAN;
    state.accel_time_s = INFINITY;
    cases[4] = (BenchCase){ "invalid", state, false };
}

static uint64_t bench_checksum(const uint8_t *pixels, size_t bytes)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        hash ^= pixels[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void bench_profile_add(HudRenderProfile *total,
                              const HudRenderProfile *sample)
{
    total->clear_time_us += sample->clear_time_us;
    total->static_copy_time_us += sample->static_copy_time_us;
    total->lap_time_us += sample->lap_time_us;
    total->accel_event_time_us += sample->accel_event_time_us;
    total->speed_time_us += sample->speed_time_us;
    total->throttle_time_us += sample->throttle_time_us;
    total->brake_time_us += sample->brake_time_us;
    total->rpm_time_us += sample->rpm_time_us;
    total->g_meter_time_us += sample->g_meter_time_us;
    total->vehicle_info_time_us += sample->vehicle_info_time_us;
    total->total_time_us += sample->total_time_us;
}

static int bench_write_frame(const char *directory, const char *name,
                             const uint8_t *pixels)
{
    char path[512];
    FILE *file;
    int written;
    size_t row;

    written = snprintf(path, sizeof(path), "%s/%s.argb", directory, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return -1;
    file = fopen(path, "wb");
    if (file == NULL) return -1;
    for (row = 0U; row < BENCH_HEIGHT; ++row) {
        const uint8_t *source = pixels + row * BENCH_STRIDE;

        if (fwrite(source, 1U, BENCH_STRIDE, file) != BENCH_STRIDE) {
            fclose(file);
            return -1;
        }
    }
    if (fclose(file) != 0) return -1;
    return 0;
}

static void bench_clear(UiCanvas *canvas, unsigned int iterations)
{
    unsigned int iteration;

    for (iteration = 0U; iteration < iterations; ++iteration) {
        ui_clear(canvas, UI_COLOR_TRANSPARENT);
    }
}

static void bench_copy(uint8_t *destination, const uint8_t *source,
                       unsigned int iterations)
{
    size_t row;
    unsigned int iteration;

    for (iteration = 0U; iteration < iterations; ++iteration) {
        for (row = 0U; row < BENCH_HEIGHT; ++row) {
            memcpy(destination + row * BENCH_STRIDE,
                   source + row * BENCH_STRIDE,
                   (size_t)BENCH_WIDTH * 4U);
        }
    }
}

int main(int argc, char **argv)
{
    HudContext *context = NULL;
    uint8_t *pixels;
    UiCanvas canvas;
    BenchCase cases[5];
    uint64_t start;
    uint64_t elapsed;
    uint64_t checksum;
    HudRenderProfile profile;
    HudRenderProfile profile_total;
    uint8_t *static_pixels;
    unsigned int index;
    unsigned int iteration;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s FONT_PATH OUTPUT_DIRECTORY\n", argv[0]);
        return EXIT_FAILURE;
    }
    pixels = calloc(1U, (size_t)BENCH_STRIDE * BENCH_HEIGHT);
    if (pixels == NULL) return EXIT_FAILURE;
    static_pixels = malloc((size_t)BENCH_STRIDE * BENCH_HEIGHT);
    if (static_pixels == NULL) {
        free(pixels);
        return EXIT_FAILURE;
    }
    memset(static_pixels, 0x5a, (size_t)BENCH_STRIDE * BENCH_HEIGHT);
    canvas = (UiCanvas){ pixels, BENCH_WIDTH, BENCH_HEIGHT, BENCH_STRIDE };
    if (hud_init(&context, argv[1]) < 0) {
        free(pixels);
        free(static_pixels);
        return EXIT_FAILURE;
    }
    bench_make_cases(cases);

    start = bench_now_us();
    bench_clear(&canvas, BENCH_CLEAR_ITERATIONS);
    elapsed = bench_now_us() - start;
    checksum = bench_checksum(pixels, (size_t)BENCH_STRIDE * BENCH_HEIGHT);
    printf("ui_clear iterations=%u total_us=%" PRIu64
           " avg_us=%.3f checksum=%016" PRIx64 "\n",
           BENCH_CLEAR_ITERATIONS, elapsed,
           (double)elapsed / BENCH_CLEAR_ITERATIONS, checksum);

    start = bench_now_us();
    bench_copy(pixels, static_pixels, BENCH_COPY_ITERATIONS);
    elapsed = bench_now_us() - start;
    checksum = bench_checksum(pixels, (size_t)BENCH_STRIDE * BENCH_HEIGHT);
    printf("static_copy iterations=%u total_us=%" PRIu64
           " avg_us=%.3f checksum=%016" PRIx64 "\n",
           BENCH_COPY_ITERATIONS, elapsed,
           (double)elapsed / BENCH_COPY_ITERATIONS, checksum);

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        memset(&profile_total, 0, sizeof(profile_total));
        start = bench_now_us();
        for (iteration = 0U; iteration < BENCH_RENDER_ITERATIONS;
             ++iteration) {
            if (hud_render_profiled(context, &canvas, &cases[index].state,
                                    cases[index].recording, &profile) < 0) {
                hud_shutdown(context);
                free(pixels);
                free(static_pixels);
                return EXIT_FAILURE;
            }
            bench_profile_add(&profile_total, &profile);
        }
        elapsed = bench_now_us() - start;
        checksum = bench_checksum(pixels,
                                  (size_t)BENCH_STRIDE * BENCH_HEIGHT);
        if (bench_write_frame(argv[2], cases[index].name, pixels) < 0) {
            fprintf(stderr, "cannot write %s/%s.argb: %s\n", argv[2],
                    cases[index].name, strerror(errno));
            hud_shutdown(context);
            free(pixels);
            free(static_pixels);
            return EXIT_FAILURE;
        }
        printf("case=%s iterations=%u total_us=%" PRIu64
               " avg_us=%.3f checksum=%016" PRIx64
               " profile_us[clear=%.3f copy=%.3f lap=%.3f accel=%.3f speed=%.3f "
               "throttle=%.3f brake=%.3f rpm=%.3f g=%.3f vehicle=%.3f "
               "total=%.3f]\n",
               cases[index].name, BENCH_RENDER_ITERATIONS, elapsed,
               (double)elapsed / BENCH_RENDER_ITERATIONS, checksum,
               (double)profile_total.clear_time_us / BENCH_RENDER_ITERATIONS,
               (double)profile_total.static_copy_time_us /
                   BENCH_RENDER_ITERATIONS,
               (double)profile_total.lap_time_us / BENCH_RENDER_ITERATIONS,
               (double)profile_total.accel_event_time_us /
                   BENCH_RENDER_ITERATIONS,
               (double)profile_total.speed_time_us / BENCH_RENDER_ITERATIONS,
               (double)profile_total.throttle_time_us /
                   BENCH_RENDER_ITERATIONS,
               (double)profile_total.brake_time_us / BENCH_RENDER_ITERATIONS,
               (double)profile_total.rpm_time_us / BENCH_RENDER_ITERATIONS,
               (double)profile_total.g_meter_time_us /
                   BENCH_RENDER_ITERATIONS,
               (double)profile_total.vehicle_info_time_us /
                   BENCH_RENDER_ITERATIONS,
               (double)profile_total.total_time_us / BENCH_RENDER_ITERATIONS);
    }
    hud_shutdown(context);
    free(pixels);
    free(static_pixels);
    return EXIT_SUCCESS;
}
