#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "accel_test.h"
#include "console_log.h"

#include <math.h>
#include <string.h>

#define ACCEL_TEST_STATIONARY_SPEED_KMH 1.0
#define ACCEL_TEST_STATIONARY_TIME_US 1500000ULL
#define ACCEL_TEST_PI 3.14159265358979323846
#define ACCEL_TEST_LAUNCH_SPEED_KMH 3.0
#define ACCEL_TEST_LAUNCH_ACCEL_MPS2 1.5
#define ACCEL_TEST_MAX_HEADING_ERROR_DEG 25.0
#define ACCEL_TEST_MAX_LATERAL_DEVIATION_M 10.0
#define ACCEL_TEST_TIMING_OFFSET_M 1.0
#define ACCEL_TEST_DISTANCE_M 75.0
#define ACCEL_TEST_MIN_STRAIGHT_SAMPLES 3U

static double accel_test_heading_error(double first_deg, double second_deg)
{
    double error = fabs(first_deg - second_deg);

    while (error > 360.0) {
        error -= 360.0;
    }
    return error > 180.0 ? 360.0 - error : error;
}

static double accel_test_along_distance(const AccelTest *test,
                                        const GpsTrackPoint *point)
{
    double heading_rad = test->launch_heading_deg * ACCEL_TEST_PI / 180.0;
    double delta_x = point->x_m - test->launch_x_m;
    double delta_y = point->y_m - test->launch_y_m;

    /* Heading is a compass heading: 0 degrees points to local +Y (north). */
    return delta_x * sin(heading_rad) + delta_y * cos(heading_rad);
}

static double accel_test_lateral_distance(const AccelTest *test,
                                          const GpsTrackPoint *point)
{
    double heading_rad = test->launch_heading_deg * ACCEL_TEST_PI / 180.0;
    double delta_x = point->x_m - test->launch_x_m;
    double delta_y = point->y_m - test->launch_y_m;

    return delta_x * cos(heading_rad) - delta_y * sin(heading_rad);
}

static uint64_t accel_test_interpolate_timestamp(
    uint64_t previous_timestamp_us, double previous_distance_m,
    uint64_t timestamp_us, double distance_m, double target_distance_m)
{
    double denominator = distance_m - previous_distance_m;
    double fraction;

    if (timestamp_us <= previous_timestamp_us || denominator <= 0.001) {
        return timestamp_us;
    }
    fraction = (target_distance_m - previous_distance_m) / denominator;
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }
    return previous_timestamp_us +
           (uint64_t)((double)(timestamp_us - previous_timestamp_us) *
                      fraction);
}

static void accel_test_set_state(const AccelTest *test, VehicleState *state)
{
    state->accel_distance_m = test->accel_distance_m;
    state->accel_time_s = test->accel_time_s;
    state->max_acceleration = test->max_acceleration;
}

void accel_test_init(AccelTest *test)
{
    if (test == NULL) {
        return;
    }
    memset(test, 0, sizeof(*test));
    test->state = ACCEL_TEST_IDLE;
}

void accel_test_update(AccelTest *test, uint64_t timestamp_us,
                       const LapProState *sample,
                       const GpsTrackPoint *point, VehicleState *state)
{
    double along_distance_m;
    double lateral_distance_m;
    double heading_error_deg;
    bool stationary;
    bool straight;

    if (test == NULL || sample == NULL || point == NULL || state == NULL ||
        sample->fix_type < 3U || !isfinite(sample->speed_kmh) ||
        !isfinite(sample->heading_deg)) {
        return;
    }

    stationary = sample->speed_kmh <= ACCEL_TEST_STATIONARY_SPEED_KMH;
    if (stationary) {
        if (test->stationary_since_us == 0U) {
            test->stationary_since_us = timestamp_us;
        }
        if (test->state == ACCEL_TEST_ABORTED) {
            accel_test_init(test);
        }
        if (test->state == ACCEL_TEST_IDLE &&
            timestamp_us >= test->stationary_since_us &&
            timestamp_us - test->stationary_since_us >=
                ACCEL_TEST_STATIONARY_TIME_US) {
            test->state = ACCEL_TEST_ARMED;
            LOG_DEBUG("[ACCEL] armed after stationary period");
        }
    } else if (test->state == ACCEL_TEST_IDLE) {
        test->stationary_since_us = 0U;
    }

    if (test->state == ACCEL_TEST_ARMED &&
        (sample->speed_kmh >= ACCEL_TEST_LAUNCH_SPEED_KMH ||
         sample->acc_x >= ACCEL_TEST_LAUNCH_ACCEL_MPS2)) {
        test->state = ACCEL_TEST_RUNNING;
        test->launch_timestamp_us = timestamp_us;
        test->launch_x_m = point->x_m;
        test->launch_y_m = point->y_m;
        test->launch_heading_deg = sample->heading_deg;
        test->previous_along_m = 0.0;
        test->previous_timestamp_us = timestamp_us;
        test->previous_speed_kmh = sample->speed_kmh;
        test->straight_samples = 0U;
        test->accel_distance_m = 0.0;
        test->accel_time_s = 0.0;
        test->max_acceleration = 0.0;
        LOG_INFO("[ACCEL] launch detected");
    }

    if (test->state != ACCEL_TEST_RUNNING) {
        test->have_previous_point = true;
        test->previous_timestamp_us = timestamp_us;
        test->previous_speed_kmh = sample->speed_kmh;
        accel_test_set_state(test, state);
        return;
    }

    along_distance_m = accel_test_along_distance(test, point);
    lateral_distance_m = fabs(accel_test_lateral_distance(test, point));
    heading_error_deg = accel_test_heading_error(sample->heading_deg,
                                                 test->launch_heading_deg);
    straight = along_distance_m >= test->previous_along_m &&
               heading_error_deg <= ACCEL_TEST_MAX_HEADING_ERROR_DEG &&
               lateral_distance_m <= ACCEL_TEST_MAX_LATERAL_DEVIATION_M;
    if (!straight) {
        if (!test->timing_started && along_distance_m >
                                        ACCEL_TEST_TIMING_OFFSET_M) {
            LOG_WARN("[ACCEL] straight-line check failed; test aborted");
            test->state = ACCEL_TEST_ABORTED;
            test->accel_distance_m = 0.0;
            test->accel_time_s = 0.0;
            test->max_acceleration = 0.0;
            accel_test_set_state(test, state);
            return;
        }
    } else {
        test->straight_samples++;
        if (!test->timing_started &&
            along_distance_m >= ACCEL_TEST_TIMING_OFFSET_M) {
            test->timing_start_timestamp_us =
                accel_test_interpolate_timestamp(
                    test->previous_timestamp_us, test->previous_along_m,
                    timestamp_us, along_distance_m,
                    ACCEL_TEST_TIMING_OFFSET_M);
            test->timing_started = true;
            LOG_INFO("[ACCEL] timing start at %.1fm", ACCEL_TEST_TIMING_OFFSET_M);
        }
        if (test->timing_started) {
            test->accel_distance_m = along_distance_m -
                                     ACCEL_TEST_TIMING_OFFSET_M;
            if (test->accel_distance_m < 0.0) {
                test->accel_distance_m = 0.0;
            }
            if (sample->acc_x > test->max_acceleration) {
                test->max_acceleration = sample->acc_x;
            }
            if (timestamp_us >= test->timing_start_timestamp_us) {
                test->accel_time_s =
                    (double)(timestamp_us - test->timing_start_timestamp_us) /
                    1000000.0;
            }
            if (test->last_report_timestamp_us == 0U ||
                timestamp_us - test->last_report_timestamp_us >= 1000000ULL) {
                LOG_INFO("[ACCEL] distance=%.1fm time=%.2fs max_acc=%.2fm/s2",
                         test->accel_distance_m, test->accel_time_s,
                         test->max_acceleration);
                test->last_report_timestamp_us = timestamp_us;
            }
            if (test->accel_distance_m >= ACCEL_TEST_DISTANCE_M) {
                test->finish_timestamp_us =
                    accel_test_interpolate_timestamp(
                        test->previous_timestamp_us, test->previous_along_m,
                        timestamp_us, along_distance_m,
                        ACCEL_TEST_TIMING_OFFSET_M + ACCEL_TEST_DISTANCE_M);
                test->accel_time_s =
                    (double)(test->finish_timestamp_us -
                             test->timing_start_timestamp_us) /
                    1000000.0;
                test->accel_distance_m = ACCEL_TEST_DISTANCE_M;
                test->state = ACCEL_TEST_FINISHED;
                LOG_INFO("[ACCEL] 75m finished time=%.2fs max_acc=%.2fm/s2",
                         test->accel_time_s, test->max_acceleration);
            }
        }
    }

    test->have_previous_point = true;
    test->previous_timestamp_us = timestamp_us;
    test->previous_along_m = along_distance_m;
    test->previous_speed_kmh = sample->speed_kmh;
    accel_test_set_state(test, state);
}

bool accel_test_is_candidate(const AccelTest *test)
{
    return test != NULL &&
           (test->state == ACCEL_TEST_RUNNING ||
            test->state == ACCEL_TEST_FINISHED) &&
           test->timing_started &&
           test->straight_samples >= ACCEL_TEST_MIN_STRAIGHT_SAMPLES;
}

void accel_test_close(AccelTest *test)
{
    (void)test;
}
