#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "accel_test.h"
#include "console_log.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static void feed(AccelTest *test, VehicleState *state, uint64_t timestamp_us,
                 double x_m, double y_m, double speed_kmh, double acc_x,
                 double heading_deg)
{
    LapProState sample;
    GpsTrackPoint point;

    memset(&sample, 0, sizeof(sample));
    sample.fix_type = 3U;
    sample.speed_kmh = speed_kmh;
    sample.heading_deg = heading_deg;
    sample.acc_x = acc_x;
    memset(&point, 0, sizeof(point));
    point.timestamp_us = timestamp_us;
    point.x_m = x_m;
    point.y_m = y_m;
    accel_test_update(test, timestamp_us, &sample, &point, state);
}

static void arm(AccelTest *test, VehicleState *state, uint64_t start_us)
{
    feed(test, state, start_us, 0.0, 0.0, 2.0, 0.0, 10.0);
    feed(test, state, start_us + 500000ULL, 0.0, 0.0, 2.0, 0.0, 200.0);
    feed(test, state, start_us + 1500000ULL, 0.0, 0.0, 2.0, 0.0,
         315.0);
    assert(test->state == ACCEL_TEST_ARMED);
}

static void feed_acquisition(AccelTest *test, VehicleState *state,
                             uint64_t launch_us, const double *x_values,
                             const double *y_values,
                             const double *headings, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        feed(test, state, launch_us + (uint64_t)index * 100000ULL,
             x_values[index], y_values[index], index == 0U ? 4.0 : 20.0,
             index == 0U ? 2.0 : 0.2, headings[index]);
    }
}

static void finish_line_run(AccelTest *test, VehicleState *state,
                            uint64_t launch_us, double heading_deg,
                            double *axis_x, double *axis_y)
{
    const double x_values[] = {0.0, 0.5, 1.2, 2.2, 3.4, 4.8, 6.2};
    const double y_values[] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double headings[] = {heading_deg, heading_deg, heading_deg,
                         heading_deg, heading_deg, heading_deg, heading_deg};

    feed_acquisition(test, state, launch_us, x_values, y_values, headings,
                     sizeof(x_values) / sizeof(x_values[0]));
    assert(test->axis_locked);
    if (axis_x != NULL) {
        *axis_x = test->axis_x;
    }
    if (axis_y != NULL) {
        *axis_y = test->axis_y;
    }
    feed(test, state, launch_us + 700000ULL, 51.2, 0.0, 50.0, 0.2,
         heading_deg);
    feed(test, state, launch_us + 1000000ULL, 76.2, 0.0, 50.0, 0.2,
         heading_deg);
    assert(test->state == ACCEL_TEST_IDLE);
    assert(test->result_valid);
    assert(test->accel_distance_m == 75.0);
    assert(test->accel_50m_time_s > 0.0);
    assert(test->accel_time_s > test->accel_50m_time_s);
    assert(!test->axis_locked);
    assert(test->axis_sample_count == 0U);
}

static void test_correct_heading(void)
{
    AccelTest test;
    VehicleState state;
    double axis_x;
    double axis_y;

    memset(&state, 0, sizeof(state));
    accel_test_init(&test);
    arm(&test, &state, 1000000ULL);
    finish_line_run(&test, &state, 3000000ULL, 90.0, &axis_x, &axis_y);
    assert(axis_x > 0.99);
    assert(fabs(axis_y) < 0.02);
    accel_test_close(&test);
}

static void test_launch_heading_is_ignored(void)
{
    AccelTest test;
    VehicleState state;
    double axis_x;
    double axis_y;

    memset(&state, 0, sizeof(state));
    accel_test_init(&test);
    arm(&test, &state, 1000000ULL);
    {
        const double x_values[] = {0.0, 0.5, 1.2, 2.2, 3.4, 4.8, 6.2};
        const double y_values[] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        const double headings[] = {100.0, 108.0, 98.0, 112.0,
                                   101.0, 95.0, 105.0};

        feed_acquisition(&test, &state, 3000000ULL, x_values, y_values,
                         headings, sizeof(x_values) / sizeof(x_values[0]));
    }
    assert(test.axis_locked);
    axis_x = test.axis_x;
    axis_y = test.axis_y;
    assert(axis_x > 0.99);
    assert(fabs(axis_y) < 0.02);
    feed(&test, &state, 3700000ULL, 51.2, 0.0, 50.0, 0.2, 80.0);
    feed(&test, &state, 4000000ULL, 76.2, 0.0, 50.0, 0.2, 270.0);
    assert(test.result_valid);
    accel_test_close(&test);
}

static void test_position_noise(void)
{
    AccelTest test;
    VehicleState state;
    const double x_values[] = {0.0, 0.5, 1.2, 2.2, 3.4, 4.8, 6.2};
    const double y_values[] = {0.0, 0.12, -0.18, 0.20, -0.22, 0.16, -0.10};
    const double headings[] = {15.0, 210.0, 45.0, 300.0, 80.0, 5.0, 270.0};

    memset(&state, 0, sizeof(state));
    accel_test_init(&test);
    arm(&test, &state, 1000000ULL);
    feed_acquisition(&test, &state, 3000000ULL, x_values, y_values,
                     headings, sizeof(x_values) / sizeof(x_values[0]));
    assert(test.axis_locked);
    assert(fabs(test.axis_y) < 0.05);
    feed(&test, &state, 3700000ULL, 51.2, 0.2, 50.0, 0.2, 90.0);
    feed(&test, &state, 4000000ULL, 76.2, -0.2, 50.0, 0.2, 90.0);
    assert(test.result_valid);
    accel_test_close(&test);
}

static void test_single_acquisition_outlier(void)
{
    AccelTest test;
    VehicleState state;
    const double x_values[] = {0.0, 0.5, 1.2, 2.2, 3.4, 4.0, 5.0, 6.2};
    const double y_values[] = {0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    const double headings[] = {90.0, 90.0, 90.0, 90.0,
                               90.0, 90.0, 90.0, 90.0};

    memset(&state, 0, sizeof(state));
    accel_test_init(&test);
    arm(&test, &state, 1000000ULL);
    feed_acquisition(&test, &state, 3000000ULL, x_values, y_values,
                     headings, sizeof(x_values) / sizeof(x_values[0]));
    assert(test.axis_locked);
    assert(fabs(test.axis_y) < 0.12);
    feed(&test, &state, 3900000ULL, 51.2, 0.0, 50.0, 0.2, 90.0);
    feed(&test, &state, 4200000ULL, 76.2, 0.0, 50.0, 0.2, 90.0);
    assert(test.result_valid);
    accel_test_close(&test);
}

static void test_axis_sign_and_reset(void)
{
    AccelTest test;
    VehicleState state;
    const double x_values[] = {0.0, 0.5, 1.2, 2.2, 3.4, 4.8, 6.2};
    const double y_values[] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    const double headings[] = {270.0, 270.0, 270.0, 270.0,
                               270.0, 270.0, 270.0};

    memset(&state, 0, sizeof(state));
    accel_test_init(&test);
    arm(&test, &state, 1000000ULL);
    feed_acquisition(&test, &state, 3000000ULL, x_values, y_values,
                     headings, sizeof(x_values) / sizeof(x_values[0]));
    assert(test.axis_locked);
    assert(test.axis_x > 0.99);
    assert(test.previous_along_m > 6.0);

    /* Abort after locking and ensure neither the axis nor its cache leaks. */
    feed(&test, &state, 3700000ULL, 6.2, 12.0, 50.0, 0.2, 90.0);
    assert(test.state == ACCEL_TEST_IDLE);
    assert(!test.axis_locked);
    assert(test.axis_sample_count == 0U);
    assert(!test.result_valid);

    arm(&test, &state, 10000000ULL);
    finish_line_run(&test, &state, 12000000ULL, 45.0, NULL, NULL);
    accel_test_close(&test);
}

static void test_timing_offset_replay(void)
{
    AccelTest test;
    VehicleState state;
    const double x_values[] = {0.0, 0.6, 1.4, 2.0, 4.0, 6.2};
    const double y_values[] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    const double headings[] = {15.0, 15.0, 15.0, 15.0, 15.0, 15.0};

    memset(&state, 0, sizeof(state));
    accel_test_init(&test);
    arm(&test, &state, 1000000ULL);
    feed(&test, &state, 3000000ULL, x_values[0], y_values[0], 4.0, 2.0,
         headings[0]);
    feed(&test, &state, 3060000ULL, x_values[1], y_values[1], 20.0, 0.2,
         headings[1]);
    feed(&test, &state, 3140000ULL, x_values[2], y_values[2], 20.0, 0.2,
         headings[2]);
    feed(&test, &state, 3200000ULL, x_values[3], y_values[3], 20.0, 0.2,
         headings[3]);
    feed(&test, &state, 3400000ULL, x_values[4], y_values[4], 20.0, 0.2,
         headings[4]);
    feed(&test, &state, 3600000ULL, x_values[5], y_values[5], 20.0, 0.2,
         headings[5]);
    assert(test.axis_locked);
    assert(test.timing_started);
    assert(test.timing_start_timestamp_us == 3100000ULL);
    feed(&test, &state, 8100000ULL, 51.2, 0.0, 50.0, 0.2, 15.0);
    feed(&test, &state, 10600000ULL, 76.2, 0.0, 50.0, 0.2, 15.0);
    assert(test.result_valid);
    assert(fabs(test.accel_50m_time_s - 5.0) < 0.05);
    assert(fabs(test.accel_time_s - 7.5) < 0.05);
    accel_test_close(&test);
}

int main(void)
{
    console_log_init();
    test_correct_heading();
    test_launch_heading_is_ignored();
    test_position_noise();
    test_single_acquisition_outlier();
    test_axis_sign_and_reset();
    test_timing_offset_replay();
    return 0;
}
