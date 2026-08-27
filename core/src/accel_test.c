#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "accel_test.h"
#include "console_log.h"

#include <math.h>
#include <string.h>

#define ACCEL_TEST_STATIONARY_SPEED_KMH 3.0
#define ACCEL_TEST_STATIONARY_TIME_US 1500000ULL
#define ACCEL_TEST_PI 3.14159265358979323846
#define ACCEL_TEST_LAUNCH_SPEED_KMH 3.0
#define ACCEL_TEST_LAUNCH_ACCEL_MPS2 1.5
#define ACCEL_TEST_MAX_LATERAL_DEVIATION_M 10.0
#define ACCEL_TEST_MAX_BACKTRACK_M 1.0
#define ACCEL_TEST_TIMING_OFFSET_M 1.0
#define ACCEL_TEST_INTERMEDIATE_DISTANCE_M 50.0
#define ACCEL_TEST_DISTANCE_M 75.0
#define ACCEL_TEST_RESULT_HOLD_US 5000000ULL
#define ACCEL_TEST_AXIS_ACQUIRE_DISTANCE_M 6.0
#define ACCEL_TEST_AXIS_MIN_POINTS 5U
#define ACCEL_TEST_AXIS_EPSILON 0.000001

static double accel_test_along_distance(const AccelTest *test, double x_m,
                                        double y_m)
{
    return (x_m - test->launch_x_m) * test->axis_x +
           (y_m - test->launch_y_m) * test->axis_y;
}

static double accel_test_lateral_distance(const AccelTest *test, double x_m,
                                           double y_m)
{
    double delta_x = x_m - test->launch_x_m;
    double delta_y = y_m - test->launch_y_m;

    return delta_x * test->axis_y - delta_y * test->axis_x;
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

static void accel_test_clear_axis(AccelTest *test)
{
    test->axis_locked = false;
    test->axis_x = 0.0;
    test->axis_y = 0.0;
    test->axis_heading_deg = 0.0;
    test->axis_sample_count = 0U;
}

static void accel_test_reset_progress(AccelTest *test)
{
    test->timing_started = false;
    test->intermediate_finished = false;
    test->timing_start_timestamp_us = 0U;
    test->intermediate_timestamp_us = 0U;
    test->finish_timestamp_us = 0U;
    test->last_report_timestamp_us = 0U;
    test->previous_timestamp_us = test->launch_timestamp_us;
    test->previous_along_m = 0.0;
    test->furthest_along_m = 0.0;
    test->accel_distance_m = 0.0;
    test->accel_50m_time_s = 0.0;
    test->accel_time_s = 0.0;
    test->max_acceleration = 0.0;
}

static void accel_test_abort_run(AccelTest *test)
{
    test->state = ACCEL_TEST_IDLE;
    test->stationary_since_us = 0U;
    test->result_valid = false;
    test->result_hold_until_us = 0U;
    accel_test_reset_progress(test);
    test->launch_x_m = 0.0;
    test->launch_y_m = 0.0;
    test->launch_heading_deg = 0.0;
    accel_test_clear_axis(test);
}

static void accel_test_append_axis_sample(AccelTest *test,
                                          const GpsTrackPoint *point,
                                          double acc_x_mps2)
{
    AccelAxisSample *destination;

    if (point->timestamp_us < test->launch_timestamp_us) {
        return;
    }
    if (test->axis_sample_count > 0U) {
        AccelAxisSample *last =
            &test->axis_samples[test->axis_sample_count - 1U];

        if (point->timestamp_us < last->timestamp_us) {
            return;
        }
        if (point->timestamp_us == last->timestamp_us) {
            last->x_m = point->x_m;
            last->y_m = point->y_m;
            last->acc_x_mps2 = acc_x_mps2;
            return;
        }
    }
    if (test->axis_sample_count < ACCEL_AXIS_MAX_POINTS) {
        destination = &test->axis_samples[test->axis_sample_count++];
    } else {
        /* Preserve the Launch point and keep the newest point available for
         * the acquisition-distance check without allocating memory. */
        memmove(&test->axis_samples[1], &test->axis_samples[2],
                (ACCEL_AXIS_MAX_POINTS - 2U) *
                    sizeof(test->axis_samples[0]));
        destination = &test->axis_samples[ACCEL_AXIS_MAX_POINTS - 1U];
    }
    destination->timestamp_us = point->timestamp_us;
    destination->x_m = point->x_m;
    destination->y_m = point->y_m;
    destination->acc_x_mps2 = acc_x_mps2;
}

static bool accel_test_fit_axis(const AccelTest *test,
                                const GpsTrackPoint *latest_point,
                                double *axis_x, double *axis_y)
{
    double mean_x = 0.0;
    double mean_y = 0.0;
    double sxx = 0.0;
    double syy = 0.0;
    double sxy = 0.0;
    double endpoint_x;
    double endpoint_y;
    double endpoint_distance;
    double discriminant;
    double principal_value;
    double angle;
    size_t index;

    if (test == NULL || latest_point == NULL || axis_x == NULL ||
        axis_y == NULL || test->axis_sample_count < ACCEL_TEST_AXIS_MIN_POINTS) {
        return false;
    }
    endpoint_x = latest_point->x_m - test->launch_x_m;
    endpoint_y = latest_point->y_m - test->launch_y_m;
    endpoint_distance = hypot(endpoint_x, endpoint_y);
    if (!isfinite(endpoint_distance) ||
        endpoint_distance < ACCEL_TEST_AXIS_ACQUIRE_DISTANCE_M) {
        return false;
    }

    for (index = 0U; index < test->axis_sample_count; ++index) {
        mean_x += test->axis_samples[index].x_m;
        mean_y += test->axis_samples[index].y_m;
    }
    mean_x /= (double)test->axis_sample_count;
    mean_y /= (double)test->axis_sample_count;
    for (index = 0U; index < test->axis_sample_count; ++index) {
        double centered_x = test->axis_samples[index].x_m - mean_x;
        double centered_y = test->axis_samples[index].y_m - mean_y;

        sxx += centered_x * centered_x;
        syy += centered_y * centered_y;
        sxy += centered_x * centered_y;
    }
    discriminant = hypot(sxx - syy, 2.0 * sxy);
    principal_value = (sxx + syy + discriminant) * 0.5;
    if (isfinite(principal_value) &&
        principal_value > ACCEL_TEST_AXIS_EPSILON) {
        angle = 0.5 * atan2(2.0 * sxy, sxx - syy);
        *axis_x = cos(angle);
        *axis_y = sin(angle);
    } else {
        /* PCA is undefined for a degenerate cloud; use the measured
         * Launch-to-latest displacement instead of GPS heading. */
        *axis_x = endpoint_x / endpoint_distance;
        *axis_y = endpoint_y / endpoint_distance;
    }
    if (!isfinite(*axis_x) || !isfinite(*axis_y) ||
        hypot(*axis_x, *axis_y) < ACCEL_TEST_AXIS_EPSILON) {
        *axis_x = endpoint_x / endpoint_distance;
        *axis_y = endpoint_y / endpoint_distance;
    }
    if (!isfinite(*axis_x) || !isfinite(*axis_y)) {
        return false;
    }
    {
        double norm = hypot(*axis_x, *axis_y);

        if (!isfinite(norm) || norm < ACCEL_TEST_AXIS_EPSILON) {
            return false;
        }
        *axis_x /= norm;
        *axis_y /= norm;
    }
    if (*axis_x * endpoint_x + *axis_y * endpoint_y < 0.0) {
        *axis_x = -*axis_x;
        *axis_y = -*axis_y;
    }
    return isfinite(*axis_x) && isfinite(*axis_y);
}

static double accel_test_axis_heading(double axis_x, double axis_y)
{
    double heading_deg = atan2(axis_x, axis_y) * 180.0 / ACCEL_TEST_PI;

    if (heading_deg < 0.0) {
        heading_deg += 360.0;
    }
    return heading_deg;
}

static void accel_test_set_state(const AccelTest *test, VehicleState *state)
{
    state->accel_distance_m = test->accel_distance_m;
    state->accel_50m_time_s = test->accel_50m_time_s;
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

static void accel_test_process_axis_sample(AccelTest *test,
                                           const AccelAxisSample *axis_sample)
{
    double along_distance_m;
    double lateral_distance_m;
    bool straight;

    along_distance_m = accel_test_along_distance(
        test, axis_sample->x_m, axis_sample->y_m);
    lateral_distance_m = fabs(accel_test_lateral_distance(
        test, axis_sample->x_m, axis_sample->y_m));
    straight = along_distance_m + ACCEL_TEST_MAX_BACKTRACK_M >=
                   test->furthest_along_m &&
               lateral_distance_m <= ACCEL_TEST_MAX_LATERAL_DEVIATION_M;
    if (!straight) {
        if (test->timing_started ||
            along_distance_m > ACCEL_TEST_TIMING_OFFSET_M) {
            LOG_WARN("[ACCEL] straight-line check failed; test aborted "
                     "along=%.1fm previous=%.1fm lateral=%.1fm",
                     along_distance_m, test->previous_along_m,
                     lateral_distance_m);
            accel_test_abort_run(test);
            return;
        }
    } else {
        if (!test->timing_started &&
            along_distance_m >= ACCEL_TEST_TIMING_OFFSET_M) {
            test->timing_start_timestamp_us =
                accel_test_interpolate_timestamp(
                    test->previous_timestamp_us, test->previous_along_m,
                    axis_sample->timestamp_us, along_distance_m,
                    ACCEL_TEST_TIMING_OFFSET_M);
            test->timing_started = true;
            LOG_INFO("[ACCEL] timing start at %.1fm",
                     ACCEL_TEST_TIMING_OFFSET_M);
        }
        if (test->timing_started) {
            test->accel_distance_m =
                along_distance_m - ACCEL_TEST_TIMING_OFFSET_M;
            if (test->accel_distance_m < 0.0) {
                test->accel_distance_m = 0.0;
            }
            if (axis_sample->acc_x_mps2 > test->max_acceleration) {
                test->max_acceleration = axis_sample->acc_x_mps2;
            }
            if (axis_sample->timestamp_us >=
                test->timing_start_timestamp_us) {
                test->accel_time_s =
                    (double)(axis_sample->timestamp_us -
                             test->timing_start_timestamp_us) /
                    1000000.0;
            }
            if (test->last_report_timestamp_us == 0U ||
                axis_sample->timestamp_us - test->last_report_timestamp_us >=
                    1000000ULL) {
                LOG_INFO("[ACCEL] distance=%.1fm time=%.2fs max_acc=%.2fm/s2",
                         test->accel_distance_m, test->accel_time_s,
                         test->max_acceleration);
                test->last_report_timestamp_us = axis_sample->timestamp_us;
            }
            if (!test->intermediate_finished &&
                test->accel_distance_m >=
                    ACCEL_TEST_INTERMEDIATE_DISTANCE_M) {
                test->intermediate_timestamp_us =
                    accel_test_interpolate_timestamp(
                        test->previous_timestamp_us, test->previous_along_m,
                        axis_sample->timestamp_us, along_distance_m,
                        ACCEL_TEST_TIMING_OFFSET_M +
                            ACCEL_TEST_INTERMEDIATE_DISTANCE_M);
                test->accel_50m_time_s =
                    (double)(test->intermediate_timestamp_us -
                             test->timing_start_timestamp_us) /
                    1000000.0;
                test->intermediate_finished = true;
                LOG_INFO("[ACCEL] 50m split time=%.2fs",
                         test->accel_50m_time_s);
            }
            if (test->accel_distance_m >= ACCEL_TEST_DISTANCE_M) {
                test->finish_timestamp_us =
                    accel_test_interpolate_timestamp(
                        test->previous_timestamp_us, test->previous_along_m,
                        axis_sample->timestamp_us, along_distance_m,
                        ACCEL_TEST_TIMING_OFFSET_M + ACCEL_TEST_DISTANCE_M);
                test->accel_time_s =
                    (double)(test->finish_timestamp_us -
                             test->timing_start_timestamp_us) /
                    1000000.0;
                test->accel_distance_m = ACCEL_TEST_DISTANCE_M;
                test->state = ACCEL_TEST_IDLE;
                test->timing_started = false;
                test->stationary_since_us = 0U;
                test->result_hold_until_us =
                    axis_sample->timestamp_us + ACCEL_TEST_RESULT_HOLD_US;
                test->result_valid = true;
                accel_test_clear_axis(test);
                LOG_INFO("[ACCEL] run finished 50m=%.2fs 75m=%.2fs "
                         "max_acc=%.2fm/s2",
                         test->accel_50m_time_s, test->accel_time_s,
                         test->max_acceleration);
                return;
            }
        }
    }

    test->previous_timestamp_us = axis_sample->timestamp_us;
    test->previous_along_m = along_distance_m;
    if (along_distance_m > test->furthest_along_m) {
        test->furthest_along_m = along_distance_m;
    }
}

static bool accel_test_lock_axis(AccelTest *test,
                                 const GpsTrackPoint *latest_point)
{
    size_t index;
    size_t sample_count;

    if (!accel_test_fit_axis(test, latest_point, &test->axis_x,
                             &test->axis_y)) {
        return false;
    }
    test->axis_locked = true;
    test->axis_heading_deg =
        accel_test_axis_heading(test->axis_x, test->axis_y);
    sample_count = test->axis_sample_count;
    LOG_INFO("[ACCEL] axis locked: points=%zu distance=%.1fm heading=%.1f",
             sample_count,
             hypot(latest_point->x_m - test->launch_x_m,
                   latest_point->y_m - test->launch_y_m),
             test->axis_heading_deg);

    /* The formal timing state starts at Launch for replay, so the cached
     * points can recover the 1m crossing even though the axis was learned
     * later. */
    accel_test_reset_progress(test);
    for (index = 0U; index < sample_count &&
                      test->state == ACCEL_TEST_RUNNING;
         ++index) {
        accel_test_process_axis_sample(test, &test->axis_samples[index]);
    }
    return true;
}

void accel_test_update(AccelTest *test, uint64_t timestamp_us,
                       const LapProState *sample,
                       const GpsTrackPoint *point, VehicleState *state)
{
    bool stationary;
    bool result_hold_active;

    if (test == NULL || sample == NULL || point == NULL || state == NULL ||
        sample->fix_type < 3U || !isfinite(sample->speed_kmh) ||
        !isfinite(sample->heading_deg) || !isfinite(sample->acc_x) ||
        !isfinite(point->x_m) || !isfinite(point->y_m)) {
        return;
    }

    result_hold_active = test->result_hold_until_us != 0U &&
                         timestamp_us < test->result_hold_until_us;
    if (!result_hold_active && test->result_hold_until_us != 0U) {
        test->result_hold_until_us = 0U;
    }

    stationary = sample->speed_kmh < ACCEL_TEST_STATIONARY_SPEED_KMH;
    if (stationary) {
        if (test->stationary_since_us == 0U) {
            test->stationary_since_us = timestamp_us;
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

    if (test->state == ACCEL_TEST_ARMED && !result_hold_active &&
        (sample->speed_kmh >= ACCEL_TEST_LAUNCH_SPEED_KMH ||
         sample->acc_x >= ACCEL_TEST_LAUNCH_ACCEL_MPS2)) {
        test->state = ACCEL_TEST_RUNNING;
        test->stationary_since_us = 0U;
        test->launch_timestamp_us = timestamp_us;
        test->launch_x_m = point->x_m;
        test->launch_y_m = point->y_m;
        test->launch_heading_deg = sample->heading_deg;
        accel_test_clear_axis(test);
        accel_test_reset_progress(test);
        accel_test_append_axis_sample(test, point, sample->acc_x);
        test->result_valid = false;
        test->result_hold_until_us = 0U;
        LOG_INFO("[ACCEL] launch detected");
    }

    if (test->state != ACCEL_TEST_RUNNING) {
        test->previous_timestamp_us = timestamp_us;
        accel_test_set_state(test, state);
        return;
    }

    if (!test->axis_locked) {
        accel_test_append_axis_sample(test, point, sample->acc_x);
        if (test->axis_sample_count >= ACCEL_TEST_AXIS_MIN_POINTS &&
            hypot(point->x_m - test->launch_x_m,
                  point->y_m - test->launch_y_m) >=
                ACCEL_TEST_AXIS_ACQUIRE_DISTANCE_M) {
            (void)accel_test_lock_axis(test, point);
        }
        if (test->state == ACCEL_TEST_RUNNING) {
            accel_test_set_state(test, state);
        }
        return;
    }

    {
        AccelAxisSample axis_sample;

        axis_sample.timestamp_us = timestamp_us;
        axis_sample.x_m = point->x_m;
        axis_sample.y_m = point->y_m;
        axis_sample.acc_x_mps2 = sample->acc_x;
        accel_test_process_axis_sample(test, &axis_sample);
    }
    accel_test_set_state(test, state);
}

void accel_test_close(AccelTest *test)
{
    (void)test;
}
