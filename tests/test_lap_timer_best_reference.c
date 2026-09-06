#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "console_log.h"
#include "gps_track.h"
#include "lap_timer.h"

#include "test_check.h"
#include <math.h>
#include <string.h>

static GpsTrackPoint point_at(const GpsTrackPoint *source,
                              uint64_t timestamp_us)
{
    GpsTrackPoint point = *source;

    point.timestamp_us = timestamp_us;
    return point;
}

static void feed_point(LapTimer *timer, const GpsTrackPoint *source,
                       uint64_t timestamp_us, VehicleState *state)
{
    GpsTrackPoint point = point_at(source, timestamp_us);

    lap_timer_update(timer, &point, state);
}

int main(void)
{
    GpsTrack track;
    GpsTrackPoint first;
    GpsTrackPoint second;
    GpsTrackPoint third;
    GpsTrackPoint fourth;
    GpsTrackPoint finish;
    VehicleState state;
    LapTimer timer;

    console_log_init();
    memset(&state, 0, sizeof(state));
    gps_track_init(&track);
    TEST_CHECK(gps_track_append(&track, 1000000ULL, 0.000, 0.000, 10.0,
                            0.0, 1.0, &first));
    TEST_CHECK(gps_track_append(&track, 2000000ULL, 0.000, 0.001, 10.0,
                            90.0, 1.0, &second));
    TEST_CHECK(gps_track_append(&track, 3000000ULL, 0.001, 0.001, 10.0,
                            0.0, 1.0, &third));
    TEST_CHECK(gps_track_append(&track, 4000000ULL, 0.001, 0.000, 10.0,
                            270.0, 1.0, &fourth));
    TEST_CHECK(gps_track_append(&track, 5000000ULL, 0.000, 0.000, 10.0,
                            0.0, 1.0, &finish));

    lap_timer_init(&timer, NULL);
    TEST_CHECK(lap_timer_configure_from_loop(&timer, &track, 0U, 4U, 0U,
                                         1000000ULL, &finish, &state));
    TEST_CHECK(timer.best_lap_time_s == 4.0);

    /* The historical first crossing is settled immediately. No extra
     * arming lap is needed before measuring a faster second lap. */
    TEST_CHECK(timer.lap_number == 2U);
    TEST_CHECK(fabs(timer.last_lap_time_s - 4.0) < 0.001);
    TEST_CHECK(fabs(timer.delta_time_s) < 0.001);

    /* Lap 2 takes 3.5 seconds and has a different time distribution: the
     * third point is reached at 1.0s instead of the old reference's 2.0s. */
    feed_point(&timer, &second, 5500000ULL, &state);
    feed_point(&timer, &third, 6000000ULL, &state);
    feed_point(&timer, &fourth, 7500000ULL, &state);
    feed_point(&timer, &first, 8500000ULL, &state);
    TEST_CHECK(timer.lap_number == 3U);
    TEST_CHECK(fabs(timer.last_lap_time_s - 3.5) < 0.001);
    TEST_CHECK(fabs(timer.best_lap_time_s - 3.5) < 0.001);
    TEST_CHECK(fabs(timer.reference_lap_time_s - 3.5) < 0.001);
    TEST_CHECK(timer.reference_count >= 5U);
    {
        size_t index;
        bool found_third = false;

        for (index = 0U; index < timer.reference_count; ++index) {
            if (hypot(timer.reference[index].x_m - third.x_m,
                      timer.reference[index].y_m - third.y_m) < 0.1) {
                TEST_CHECK(fabs(timer.reference[index].time_s - 1.0) < 0.001);
                found_third = true;
                break;
            }
        }
        TEST_CHECK(found_third);
    }

    /* Lap 3 is slower. At its third point, Delta must use Lap 2's 1.0s
     * reference time (+1.00s), not the initial reference's 2.00s. */
    feed_point(&timer, &second, 9500000ULL, &state);
    feed_point(&timer, &third, 10500000ULL, &state);
    TEST_CHECK(fabs(timer.current_time_s - 2.0) < 0.001);
    TEST_CHECK(fabs(timer.delta_time_s - 1.0) < 0.001);
    TEST_CHECK(fabs(state.delta_time_s - timer.delta_time_s) < 0.001);

    /* A 50 Hz long-running lap must not overflow the fixed reference buffer;
     * only the reference trace is decimated, while timing still updates for
     * every sample. */
    {
        unsigned int sample;

        for (sample = 1U; sample <= 5000U; ++sample) {
            feed_point(&timer, &third,
                       10500000ULL + (uint64_t)sample * 20000ULL, &state);
        }
        TEST_CHECK(!timer.current_lap_overflow);
        TEST_CHECK(timer.current_lap_count < 600U);
        TEST_CHECK(timer.current_time_s > 100.0);
    }

    lap_timer_close(&timer);
    return 0;
}
