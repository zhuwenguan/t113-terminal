#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "console_log.h"
#include "gps_track.h"
#include "lap_timer.h"

#include <assert.h>
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
    assert(gps_track_append(&track, 1000000ULL, 0.000, 0.000, 10.0,
                            0.0, 1.0, &first));
    assert(gps_track_append(&track, 2000000ULL, 0.000, 0.001, 10.0,
                            90.0, 1.0, &second));
    assert(gps_track_append(&track, 3000000ULL, 0.001, 0.001, 10.0,
                            0.0, 1.0, &third));
    assert(gps_track_append(&track, 4000000ULL, 0.001, 0.000, 10.0,
                            270.0, 1.0, &fourth));
    assert(gps_track_append(&track, 5000000ULL, 0.000, 0.000, 10.0,
                            0.0, 1.0, &finish));

    lap_timer_init(&timer, NULL);
    assert(lap_timer_configure_from_loop(&timer, &track, 0U, 4U, 0U,
                                         1000000ULL, &finish, &state));
    assert(timer.best_lap_time_s == 4.0);

    /* The learned reference is deliberately slow (4 seconds). This initial
     * crossing is slower still and therefore remains only the arming lap. */
    feed_point(&timer, &second, 5250000ULL, &state);
    feed_point(&timer, &third, 5750000ULL, &state);
    feed_point(&timer, &fourth, 7250000ULL, &state);
    feed_point(&timer, &first, 8250000ULL, &state);
    assert(timer.lap_number == 2U);
    assert(fabs(timer.best_lap_time_s - 4.0) < 0.001);

    /* Lap 2 takes 3.5 seconds and has a different time distribution: the
     * third point is reached at 1.0s instead of the old reference's 2.0s. */
    feed_point(&timer, &second, 8750000ULL, &state);
    feed_point(&timer, &third, 9250000ULL, &state);
    feed_point(&timer, &fourth, 10750000ULL, &state);
    feed_point(&timer, &first, 11750000ULL, &state);
    assert(timer.lap_number == 3U);
    assert(fabs(timer.last_lap_time_s - 3.5) < 0.001);
    assert(fabs(timer.best_lap_time_s - 3.5) < 0.001);
    assert(fabs(timer.reference_lap_time_s - 3.5) < 0.001);
    assert(timer.reference_count >= 5U);
    {
        size_t index;
        bool found_third = false;

        for (index = 0U; index < timer.reference_count; ++index) {
            if (hypot(timer.reference[index].x_m - third.x_m,
                      timer.reference[index].y_m - third.y_m) < 0.1) {
                assert(fabs(timer.reference[index].time_s - 1.0) < 0.001);
                found_third = true;
                break;
            }
        }
        assert(found_third);
    }

    /* Lap 3 is slower. At its third point, Delta must use Lap 2's 1.0s
     * reference time (+1.00s), not the initial reference's 2.00s. */
    feed_point(&timer, &second, 12750000ULL, &state);
    feed_point(&timer, &third, 13750000ULL, &state);
    assert(fabs(timer.current_time_s - 2.0) < 0.001);
    assert(fabs(timer.delta_time_s - 1.0) < 0.001);
    assert(fabs(state.delta_time_s - timer.delta_time_s) < 0.001);

    lap_timer_close(&timer);
    return 0;
}
