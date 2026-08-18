#include "race_event.h"

#include <assert.h>
#include <string.h>

static void update(RaceEvent *event, VehicleState *state,
                   uint64_t timestamp_us, double speed_kmh,
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
    sample.acc_x = 0.2;
    race_event_update(event, timestamp_us, &sample, state);
}

int main(void)
{
    const double start_longitude = 116.20048420;
    const double start_latitude = 39.92446810;
    RaceEvent event;
    VehicleState state;
    unsigned int index;

    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);

    /* GPS speed jitter below 3 km/h must still establish a stationary start. */
    update(&event, &state, 1000000ULL, 2.5, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 1500000ULL, 2.6, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 2000000ULL, 2.4, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 2500000ULL, 2.7, 90.0, start_longitude,
           start_latitude);
    assert(event.accel_test.state == ACCEL_TEST_ARMED);
    assert(event.mode == RACE_EVENT_UNKNOWN);

    update(&event, &state, 3000000ULL, 3.5, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 3040000ULL, 5.0, 90.0,
           start_longitude + 0.000006, start_latitude);
    update(&event, &state, 3080000ULL, 7.0, 90.0,
           start_longitude + 0.000014, start_latitude);

    assert(event.accel_test.timing_started);
    assert(!accel_test_is_candidate(&event.accel_test));
    assert(event.mode == RACE_EVENT_UNKNOWN);

    /* Sub-metre GPS backtracking is noise, not evidence of a turn. */
    update(&event, &state, 3120000ULL, 7.0, 90.0,
           start_longitude + 0.000010, start_latitude);
    assert(event.accel_test.state == ACCEL_TEST_RUNNING);

    for (index = 1U; index <= 7U; ++index) {
        update(&event, &state, 3120000ULL + index * 40000ULL, 40.0, 90.0,
               start_longitude + index * 0.000130, start_latitude);
    }

    assert(accel_test_is_candidate(&event.accel_test));
    assert(event.mode == RACE_EVENT_ACCEL_MODE);
    assert(state.event_mode == RACE_EVENT_ACCEL_MODE);

    race_event_close(&event);

    /* A circuit-style launch that turns before 75 m must not claim ACCEL. */
    memset(&state, 0, sizeof(state));
    race_event_init(&event, NULL);
    update(&event, &state, 10000000ULL, 2.5, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 10500000ULL, 2.6, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 11000000ULL, 2.4, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 11500000ULL, 2.7, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 12000000ULL, 4.0, 90.0, start_longitude,
           start_latitude);
    update(&event, &state, 12500000ULL, 30.0, 90.0,
           start_longitude + 0.000120, start_latitude);
    assert(event.accel_test.timing_started);
    update(&event, &state, 13000000ULL, 30.0, 0.0,
           start_longitude + 0.000130, start_latitude + 0.000010);
    assert(event.accel_test.state == ACCEL_TEST_ABORTED);
    assert(!accel_test_is_candidate(&event.accel_test));
    assert(event.mode == RACE_EVENT_UNKNOWN);
    race_event_close(&event);
    return 0;
}
