#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "console_log.h"
#include "race_event.h"
#include "test_check.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void feed_circle(RaceEvent *event, VehicleState *state,
                        double elapsed, double phase)
{
    LapProState sample = {0};
    const double radius = 100.0;
    const double degrees_per_m = 180.0 / (M_PI * 6371000.0);
    sample.fix_type = 3U;
    sample.hacc_m = 1.0;
    sample.speed_kmh = 25.0;
    sample.latitude = radius * sin(phase) * degrees_per_m;
    sample.longitude = radius * cos(phase) * degrees_per_m;
    sample.heading_deg = fmod(720.0 - phase * 180.0 / M_PI, 360.0);
    race_event_update(event, 1000000ULL + (uint64_t)llround(elapsed * 1e6),
                      &sample, state);
}

/* Neither the lap period nor the second crossing aligns with the 5 Hz
 * reference samples. Every second-lap sample must have continuous Delta. */
static void test_identical_laps(double period, double step)
{
    static RaceEvent event;
    VehicleState state = {0};
    double max_delta = 0.0;
    double first_result_at = 0.0;
    unsigned updates = 0U;
    race_event_init(&event, NULL);
    for (unsigned i = 0U; i * step <= 2.0 * period + 2.0; ++i) {
        double elapsed = i * step;
        double phase = 2.0 * M_PI * fmod(elapsed, period) / period;
        feed_circle(&event, &state, elapsed, phase);
        if (state.lap_number >= 2U) {
            if (first_result_at == 0.0) {
                first_result_at = elapsed;
                TEST_CHECK(state.last_lap_time_s > period);
                TEST_CHECK(state.last_lap_time_s < period + 2.0);
            }
            TEST_CHECK(fabs(state.best_lap_time_s - period) < 0.002);
            max_delta = fmax(max_delta, fabs(state.delta_time_s));
            TEST_CHECK(fabs(state.delta_time_s) < 0.003);
            updates++;
        }
    }
    TEST_CHECK(first_result_at > 0.0 && first_result_at < period + 2.0);
    TEST_CHECK(state.lap_number == 3U);
    TEST_CHECK(fabs(state.last_lap_time_s - period) < 0.002);
    TEST_CHECK(updates > (unsigned)(period / step * 0.9));
    printf("period=%.3f step=%.3f first_result_at=%.3f max_delta=%.6f updates=%u\n",
           period, step, first_result_at, max_delta, updates);
    race_event_close(&event);
}

/* An analytic speed profile changes the second lap's time distribution.
 * Compare against the first lap at the same physical phase, not a nearby
 * stored vertex. The virtual gate is deliberately selected by the detector. */
static void test_different_speed_profile(void)
{
    static RaceEvent event;
    VehicleState state = {0};
    const double first_period = 90.0, second_period = 84.0, amplitude = 1.5;
    const unsigned samples = 4500U;
    double gate_phase = 0.0, gate_second_time = 0.0, max_error = 0.0;
    bool have_gate = false;
    unsigned compared = 0U;
    race_event_init(&event, NULL);
    for (unsigned i = 0U; i <= 2U * samples; ++i) {
        unsigned round = i / samples;
        double u = (double)(i % samples) / samples;
        double elapsed = round == 0U ? first_period * u :
            first_period + (round - 1U) * second_period +
            second_period * u + amplitude * sin(2.0 * M_PI * u);
        feed_circle(&event, &state, elapsed, 2.0 * M_PI * u);
        if (state.lap_number == 2U && !have_gate) {
            gate_phase = atan2(event.lap_timer.start_y_m,
                               event.lap_timer.start_x_m + 100.0) /
                         (2.0 * M_PI);
            gate_second_time = second_period * gate_phase +
                amplitude * sin(2.0 * M_PI * gate_phase);
            have_gate = true;
        }
        if (round == 1U && have_gate && u > gate_phase + 0.01) {
            double expected = second_period * u + amplitude * sin(2.0*M_PI*u) -
                gate_second_time - first_period * (u - gate_phase);
            double error = fabs(state.delta_time_s - expected);
            max_error = fmax(error, max_error);
            TEST_CHECK(error < 0.004);
            compared++;
        }
    }
    TEST_CHECK(compared > 4000U);
    printf("different speed profile max_delta_error=%.6f samples=%u\n",
           max_error, compared);
    race_event_close(&event);
}

/* Known straight gate, asynchronous samples and a laterally shifted finish.
 * The interpolated finish is 20.0s, between two receiving samples. A faster
 * trace has distinct start/end coordinates and must not make Delta wrap. */
static void test_gate_interpolation_and_driving_line(void)
{
    static GpsTrack track;
    static LapTimer timer;
    VehicleState state = {0};
    const double xy[][2] = {{0,0}, {50,0}, {50,50}, {-50,50}, {-50,0}, {0,0}};
    const double headings[] = {90,45,315,225,135,90};
    GpsTrackPoint point;
    gps_track_init(&track);
    track.point_count = 6U;
    for (size_t i = 0; i < 6U; ++i) {
        track.points[i].timestamp_us = 1000000ULL + i * 2000000ULL;
        track.points[i].x_m = xy[i][0];
        track.points[i].y_m = xy[i][1];
        track.points[i].heading_deg = headings[i];
    }
    lap_timer_init(&timer, NULL);
    TEST_CHECK(lap_timer_configure_from_loop(&timer, &track, 0U, 5U, 0U,
                                           1000000ULL, &track.points[5], &state));
    TEST_CHECK(state.lap_number == 2U);
    TEST_CHECK(fabs(state.last_lap_time_s - 10.0) < 1e-6);
    /* Retain a complete polygon with a nine-second second lap. */
    for (size_t i = 1; i < 5U; ++i) {
        point = track.points[i];
        point.timestamp_us = 11000000ULL + i * 1600000ULL;
        lap_timer_update(&timer, &point, &state);
    }
    point.timestamp_us = 19300000ULL;
    point.x_m = -7.0; point.y_m = 4.0; point.heading_deg = 90.0;
    lap_timer_update(&timer, &point, &state);
    point.timestamp_us = 19990000ULL; point.x_m = -0.1;
    lap_timer_update(&timer, &point, &state);
    point.timestamp_us = 20000100ULL; point.x_m = 0.001;
    lap_timer_update(&timer, &point, &state);
    TEST_CHECK(state.lap_number == 3U);
    TEST_CHECK(timer.lap_start_timestamp_us == 20000000ULL);
    TEST_CHECK(fabs(state.last_lap_time_s - 9.0) < 1e-6);
    TEST_CHECK(fabs(state.best_lap_time_s - 9.0) < 1e-6);
    TEST_CHECK(fabs(state.lap_time_s - 0.0001) < 1e-6);
    TEST_CHECK(fabs(state.delta_time_s) < 0.005);
    lap_timer_close(&timer);
}

int main(void)
{
    console_log_init();
    console_log_set_level(CONSOLE_LOG_WARN);
    test_identical_laps(90.0, 0.02);
    test_identical_laps(90.037, 0.02);
    test_identical_laps(30.073, 0.07);
    test_different_speed_profile();
    test_gate_interpolation_and_driving_line();
    return 0;
}
