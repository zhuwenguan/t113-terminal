#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "console_log.h"
#include "race_event.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_ORIGIN_LATITUDE 30.0
#define TEST_ORIGIN_LONGITUDE 120.0
#define TEST_LOOP_POINTS 32U
#define TEST_LOOP_PERIOD_US 30000000ULL
#define TEST_LOOP_STEP_US (TEST_LOOP_PERIOD_US / TEST_LOOP_POINTS)
#define TEST_HIGH_RATE_STEP_US 20000ULL

static void update_local(RaceEvent *event, VehicleState *state,
                         uint64_t timestamp_us, double x_m, double y_m,
                         double speed_kmh, double heading_deg)
{
    LapProState sample;
    double latitude_scale = cos(TEST_ORIGIN_LATITUDE * M_PI / 180.0);

    memset(&sample, 0, sizeof(sample));
    sample.fix_type = 3U;
    sample.num_sv = 12U;
    sample.speed_kmh = speed_kmh;
    sample.heading_deg = heading_deg;
    sample.hacc_m = 1.0;
    sample.latitude = TEST_ORIGIN_LATITUDE + y_m / 111111.0;
    sample.longitude = TEST_ORIGIN_LONGITUDE +
                       x_m / (111111.0 * latitude_scale);
    race_event_update(event, timestamp_us, &sample, state);
}

static void loop_xy_heading(unsigned int index, double *x_m, double *y_m,
                            double *heading_deg)
{
    unsigned int side = index / 8U;
    unsigned int offset = index % 8U;

    switch (side) {
    case 0U:
        *x_m = (double)offset * 10.0;
        *y_m = 0.0;
        *heading_deg = 90.0;
        break;
    case 1U:
        *x_m = 70.0;
        *y_m = (double)offset * 10.0;
        *heading_deg = 0.0;
        break;
    case 2U:
        *x_m = 70.0 - (double)offset * 10.0;
        *y_m = 70.0;
        *heading_deg = 270.0;
        break;
    default:
        *x_m = 0.0;
        *y_m = 70.0 - (double)offset * 10.0;
        *heading_deg = 180.0;
        break;
    }
}

static void update_loop_point(RaceEvent *event, VehicleState *state,
                              uint64_t timestamp_us, unsigned int index)
{
    double x_m;
    double y_m;
    double heading_deg;

    loop_xy_heading(index % TEST_LOOP_POINTS, &x_m, &y_m, &heading_deg);
    update_local(event, state, timestamp_us, x_m, y_m, 20.0, heading_deg);
}

static void test_unknown_running_time(void)
{
    RaceEvent event;
    VehicleState state;

    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);
    update_local(&event, &state, 1000000ULL, 0.0, 0.0, 0.0, 90.0);
    assert(state.lap_time_s == 0.0);
    update_local(&event, &state, 2000000ULL, 0.0, 0.0, 2.0, 90.0);
    assert(!event.movement_started);
    assert(state.lap_time_s == 0.0);
    update_local(&event, &state, 3000000ULL, 0.0, 0.0, 2.1, 90.0);
    assert(event.movement_started);
    assert(state.lap_time_s == 0.0);
    update_local(&event, &state, 4000000ULL, 5.0, 0.0, 10.0, 90.0);
    assert(fabs(state.lap_time_s - 1.0) < 0.001);
    update_local(&event, &state, 8000000ULL, 25.0, 0.0, 10.0, 90.0);
    assert(fabs(state.lap_time_s - 5.0) < 0.001);
    update_local(&event, &state, 13000000ULL, 50.0, 0.0, 10.0, 90.0);
    assert(fabs(state.lap_time_s - 10.0) < 0.001);
    assert(event.mode == RACE_EVENT_UNKNOWN);
    assert(!state.lap_valid);
    assert(state.delta_time_s == 0.0);
    race_event_close(&event);
}

static void test_loop_transition_and_next_lap(void)
{
    RaceEvent event;
    VehicleState state;
    double previous_lap_time = 0.0;
    bool saw_circuit = false;
    unsigned int detection_index = 0U;
    unsigned int index;
    uint64_t timestamp_us;

    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);
    update_local(&event, &state, 1000000ULL, 0.0, 0.0, 0.0, 90.0);
    for (index = 0U; index < TEST_LOOP_POINTS; ++index) {
        timestamp_us = 2000000ULL + (uint64_t)index * TEST_LOOP_STEP_US;
        update_loop_point(&event, &state, timestamp_us, index);
        previous_lap_time = state.lap_time_s;
    }
    for (index = 0U; index <= 8U; ++index) {
        timestamp_us = 2000000ULL +
                       (uint64_t)(TEST_LOOP_POINTS + index) *
                           TEST_LOOP_STEP_US;
        update_loop_point(&event, &state, timestamp_us, index);
        if (event.mode == RACE_EVENT_CIRCUIT_MODE) {
            double expected =
                (double)(timestamp_us - event.movement_start_timestamp_us) /
                1000000.0;

            assert(fabs(state.lap_time_s - expected) < 0.01);
            assert(state.lap_time_s >= previous_lap_time);
            assert(state.lap_valid);
            saw_circuit = true;
            detection_index = index;
            break;
        }
        previous_lap_time = state.lap_time_s;
    }
    assert(saw_circuit);

    /* Continue until the next real crossing. The current LapTimer owns the
     * formal lap fields after circuit mode is established. */
    for (index = detection_index + 1U;
         index <= detection_index + TEST_LOOP_POINTS + 8U; ++index) {
        timestamp_us = 2000000ULL +
                       (uint64_t)(TEST_LOOP_POINTS + index) *
                           TEST_LOOP_STEP_US;
        update_loop_point(&event, &state, timestamp_us, index);
    }
    assert(event.mode == RACE_EVENT_CIRCUIT_MODE);
    assert(event.lap_timer.lap_number >= 2U);
    assert(state.last_lap_time_s > 0.0);
    assert(state.lap_time_s >= 0.0);
    assert(state.best_lap_time_s > 0.0);
    race_event_close(&event);
}

static void test_high_rate_ninety_second_loop(void)
{
    const uint64_t loop_period_us = 90000000ULL;
    const unsigned int loop_steps =
        (unsigned int)(loop_period_us / TEST_HIGH_RATE_STEP_US);
    RaceEvent event;
    VehicleState state;
    unsigned int step;

    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);
    update_local(&event, &state, 1000000ULL, 100.0, 0.0, 0.0, 0.0);
    for (step = 0U; step <= loop_steps + 60U; ++step) {
        double phase = 2.0 * M_PI *
                       (double)(step % loop_steps) / (double)loop_steps;
        double x_m = 100.0 * cos(phase);
        double y_m = 100.0 * sin(phase);
        double heading_deg = fmod(360.0 - phase * 180.0 / M_PI, 360.0);

        update_local(&event, &state,
                     2000000ULL + (uint64_t)step * TEST_HIGH_RATE_STEP_US,
                     x_m, y_m, 25.0, heading_deg);
    }
    assert(event.mode == RACE_EVENT_CIRCUIT_MODE);
    assert(event.gps_track.point_count > 400U);
    assert(event.gps_track.point_count < 600U);
    race_event_close(&event);
}

static void test_nine_hundred_second_storage_budget(void)
{
    GpsTrack track;
    GpsTrackPoint point;
    bool stored;
    unsigned int sample;

    gps_track_init(&track);
    for (sample = 0U; sample <= 45000U; ++sample) {
        assert(gps_track_sample(
            &track, 1000000ULL + (uint64_t)sample * TEST_HIGH_RATE_STEP_US,
            TEST_ORIGIN_LATITUDE,
            TEST_ORIGIN_LONGITUDE + (double)sample / 1000000000.0,
            25.0, 90.0, 1.0, 200000ULL, &point, &stored));
    }
    assert(track.point_count == 4501U);
    assert(track.point_count < GPS_TRACK_MAX_POINTS);
}

int main(void)
{
    console_log_init();
    console_log_set_level(CONSOLE_LOG_WARN);
    test_unknown_running_time();
    test_loop_transition_and_next_lap();
    test_high_rate_ninety_second_loop();
    test_nine_hundred_second_storage_budget();
    return 0;
}
