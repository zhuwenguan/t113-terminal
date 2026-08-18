#include "race_event.h"

#include <assert.h>
#include <string.h>

static void update(RaceEvent *event, VehicleState *state,
                   uint64_t timestamp_us, double speed_kmh, double acc_x,
                   double heading_deg, double longitude, double latitude)
{
    LapProState sample;

    memset(&sample, 0, sizeof(sample));
    sample.fix_type = 3U;
    sample.speed_kmh = speed_kmh;
    sample.heading_deg = heading_deg;
    sample.hacc_m = 1.0;
    sample.latitude = latitude;
    sample.longitude = longitude;
    sample.acc_x = acc_x;
    race_event_update(event, timestamp_us, &sample, state);
}

static void wait_until_armed(RaceEvent *event, VehicleState *state,
                             uint64_t start_us, double longitude,
                             double latitude)
{
    update(event, state, start_us, 2.5, 0.2, 90.0, longitude, latitude);
    update(event, state, start_us + 500000ULL, 2.6, 0.2, 90.0,
           longitude, latitude);
    update(event, state, start_us + 1000000ULL, 2.4, 0.2, 90.0,
           longitude, latitude);
    update(event, state, start_us + 1500000ULL, 2.7, 0.2, 90.0,
           longitude, latitude);
    assert(event->accel_test.state == ACCEL_TEST_ARMED);
}

static void finish_run(RaceEvent *event, VehicleState *state,
                       uint64_t start_us, double longitude, double latitude)
{
    unsigned int index;

    /* Crossing either launch signal starts a run from the current point. */
    update(event, state, start_us, 4.0, 0.2, 90.0, longitude, latitude);
    assert(event->accel_test.state == ACCEL_TEST_RUNNING);
    update(event, state, start_us + 40000ULL, 5.0, 2.0, 90.0,
           longitude, latitude);
    assert(event->accel_test.state == ACCEL_TEST_RUNNING);
    update(event, state, start_us + 80000ULL, 7.0, 2.0, 90.0,
           longitude + 0.000014, latitude);
    assert(event->accel_test.timing_started);

    /* Sub-metre GPS backtracking is noise, not evidence of a turn. */
    update(event, state, start_us + 120000ULL, 7.0, 0.2, 90.0,
           longitude + 0.000010, latitude);
    assert(event->accel_test.state == ACCEL_TEST_RUNNING);

    for (index = 1U; index <= 7U; ++index) {
        update(event, state, start_us + 120000ULL + index * 40000ULL,
               40.0, 0.2, index >= 6U ? 55.0 : 90.0,
               longitude + index * 0.000130, latitude);
    }

    assert(event->accel_test.state == ACCEL_TEST_IDLE);
    assert(event->accel_test.result_valid);
    assert(event->accel_test.finish_timestamp_us < start_us + 400000ULL);
    assert(event->accel_test.result_hold_until_us ==
           start_us + 5400000ULL);
    assert(state->accel_distance_m == 75.0);
    assert(state->accel_50m_time_s > 0.0);
    assert(state->accel_time_s > state->accel_50m_time_s);
}

int main(void)
{
    const double start_longitude = 116.20048420;
    const double start_latitude = 39.92446810;
    RaceEvent event;
    VehicleState state;
    double first_50m_time_s;
    double first_75m_time_s;

    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);

    wait_until_armed(&event, &state, 1000000ULL, start_longitude,
                     start_latitude);
    finish_run(&event, &state, 3000000ULL, start_longitude, start_latitude);
    assert(event.mode == RACE_EVENT_UNKNOWN);
    assert(state.event_mode == RACE_EVENT_UNKNOWN);
    assert(event.gps_track.point_count > 0U);
    first_50m_time_s = state.accel_50m_time_s;
    first_75m_time_s = state.accel_time_s;

    /* Results remain available while the next run waits for a new launch. */
    wait_until_armed(&event, &state, 5000000ULL,
                     start_longitude + 0.001200, start_latitude);
    assert(state.accel_50m_time_s == first_50m_time_s);
    assert(state.accel_time_s == first_75m_time_s);

    /* A new launch cannot overwrite the result during its five-second hold. */
    update(&event, &state, 8399999ULL, 4.0, 2.0, 90.0,
           start_longitude + 0.001200, start_latitude);
    assert(event.accel_test.state == ACCEL_TEST_ARMED);
    assert(event.accel_test.result_valid);
    assert(state.accel_distance_m == 75.0);
    assert(state.accel_50m_time_s == first_50m_time_s);
    assert(state.accel_time_s == first_75m_time_s);

    finish_run(&event, &state, 9000000ULL,
               start_longitude + 0.001200, start_latitude);
    assert(event.mode == RACE_EVENT_UNKNOWN);

    race_event_close(&event);

    /* Turning before 75 m aborts only the acceleration run. */
    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);
    wait_until_armed(&event, &state, 10000000ULL, start_longitude,
                     start_latitude);
    update(&event, &state, 12000000ULL, 4.0, 0.2, 90.0,
           start_longitude, start_latitude);
    update(&event, &state, 12500000ULL, 30.0, 0.2, 90.0,
           start_longitude + 0.000120, start_latitude);
    assert(event.accel_test.timing_started);
    update(&event, &state, 13000000ULL, 30.0, 0.2, 0.0,
           start_longitude + 0.000130, start_latitude + 0.000100);
    assert(event.accel_test.state == ACCEL_TEST_IDLE);
    assert(!event.accel_test.result_valid);
    assert(event.mode == RACE_EVENT_UNKNOWN);
    race_event_close(&event);

    /* Backtrack tolerance is relative to the furthest forward position. */
    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);
    wait_until_armed(&event, &state, 30000000ULL, start_longitude,
                     start_latitude);
    update(&event, &state, 32000000ULL, 4.0, 0.2, 90.0,
           start_longitude, start_latitude);
    update(&event, &state, 32500000ULL, 20.0, 0.2, 90.0,
           start_longitude + 0.000120, start_latitude);
    update(&event, &state, 33000000ULL, 20.0, 0.2, 90.0,
           start_longitude + 0.000110, start_latitude);
    assert(event.accel_test.state == ACCEL_TEST_RUNNING);
    update(&event, &state, 33500000ULL, 20.0, 0.2, 90.0,
           start_longitude + 0.000095, start_latitude);
    assert(event.accel_test.state == ACCEL_TEST_IDLE);
    assert(!event.accel_test.result_valid);
    race_event_close(&event);

    /* A 75 m run is still measured while circuit timing owns the mode. */
    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);
    event.mode = RACE_EVENT_CIRCUIT_MODE;
    wait_until_armed(&event, &state, 20000000ULL, start_longitude,
                     start_latitude);
    finish_run(&event, &state, 22000000ULL, start_longitude, start_latitude);
    assert(event.mode == RACE_EVENT_CIRCUIT_MODE);
    assert(state.event_mode == RACE_EVENT_CIRCUIT_MODE);
    race_event_close(&event);
    return 0;
}
