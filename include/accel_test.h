#ifndef T113_ACCEL_TEST_H
#define T113_ACCEL_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include "gps_track.h"
#include "lappro.h"
#include "vehicle_state.h"

typedef enum {
    ACCEL_TEST_IDLE = 0,
    ACCEL_TEST_ARMED,
    ACCEL_TEST_RUNNING
} AccelTestState;

typedef struct {
    AccelTestState state;
    bool have_previous_point;
    bool timing_started;
    bool intermediate_finished;
    uint64_t stationary_since_us;
    uint64_t previous_timestamp_us;
    uint64_t launch_timestamp_us;
    uint64_t timing_start_timestamp_us;
    uint64_t intermediate_timestamp_us;
    uint64_t finish_timestamp_us;
    uint64_t last_report_timestamp_us;
    double launch_x_m;
    double launch_y_m;
    double launch_heading_deg;
    double previous_along_m;
    double previous_speed_kmh;
    double accel_distance_m;
    double accel_50m_time_s;
    double accel_time_s;
    double max_acceleration;
    uint32_t straight_samples;
} AccelTest;

void accel_test_init(AccelTest *test);

/* Updates the detector and its output fields in VehicleState. */
void accel_test_update(AccelTest *test, uint64_t timestamp_us,
                       const LapProState *sample,
                       const GpsTrackPoint *point, VehicleState *state);

void accel_test_close(AccelTest *test);

#endif
