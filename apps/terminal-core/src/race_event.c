#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "race_event.h"
#include "console_log.h"

#include <math.h>
#include <string.h>

#define RACE_EVENT_MIN_LOOP_TIME_US 15000000ULL
#define RACE_EVENT_MAX_LOOP_TIME_US 900000000ULL
#define RACE_EVENT_MIN_LOOP_DISTANCE_M 80.0
/* The initial closure must be tighter than a nearby track crossing. */
#define RACE_EVENT_LOOP_MATCH_DISTANCE_M 8.0
#define RACE_EVENT_LOOP_CONTINUITY_DISTANCE_M 20.0
#define RACE_EVENT_LOOP_HEADING_ERROR_DEG 45.0
#define RACE_EVENT_LOOP_CONTINUITY_HEADING_DEG 60.0
#define RACE_EVENT_LOOP_MIN_MATCHES 5U
#define RACE_EVENT_LOOP_SEARCH_RADIUS 2U
#define RACE_EVENT_MOVING_SPEED_KMH 2.0
#define RACE_EVENT_TRACK_SAMPLE_INTERVAL_US 200000ULL

static double race_event_heading_error(double first_deg, double second_deg)
{
    double error = fabs(first_deg - second_deg);

    while (error > 360.0) {
        error -= 360.0;
    }
    return error > 180.0 ? 360.0 - error : error;
}

static bool race_event_point_matches(const GpsTrackPoint *current,
                                     const GpsTrackPoint *history,
                                     double distance_limit_m,
                                     double heading_limit_deg)
{
    return current != NULL && history != NULL &&
           gps_track_distance(current, history) <= distance_limit_m &&
           race_event_heading_error(current->heading_deg,
                                    history->heading_deg) <=
               heading_limit_deg;
}

static bool race_event_find_initial_match(const GpsTrack *track,
                                          size_t current_index,
                                          size_t *history_index)
{
    const GpsTrackPoint *current;
    size_t index;

    if (track == NULL || history_index == NULL || current_index >=
                                                       track->point_count) {
        return false;
    }
    current = gps_track_at(track, current_index);
    if (current == NULL) {
        return false;
    }
    for (index = 0U; index < current_index; ++index) {
        const GpsTrackPoint *history = gps_track_at(track, index);
        uint64_t time_gap;

        if (history == NULL || current->timestamp_us < history->timestamp_us) {
            continue;
        }
        time_gap = current->timestamp_us - history->timestamp_us;
        if (time_gap < RACE_EVENT_MIN_LOOP_TIME_US ||
            time_gap > RACE_EVENT_MAX_LOOP_TIME_US ||
            !race_event_point_matches(current, history,
                                      RACE_EVENT_LOOP_MATCH_DISTANCE_M,
                                      RACE_EVENT_LOOP_HEADING_ERROR_DEG)) {
            continue;
        }
        /* When more than one crossing is geometrically possible, the first
         * historical match is the earlier crossing and is treated as the
         * pit-lane merge reference. */
        *history_index = index;
        return true;
    }
    return false;
}

static bool race_event_find_continuous_match(const GpsTrack *track,
                                             size_t current_index,
                                             size_t expected_index,
                                             size_t minimum_history_index,
                                             size_t *matched_index)
{
    size_t offset;
    size_t index;
    const GpsTrackPoint *current;

    if (track == NULL || matched_index == NULL || current_index >=
                                                       track->point_count) {
        return false;
    }
    current = gps_track_at(track, current_index);
    if (current == NULL) {
        return false;
    }
    /* Prefer the expected phase, then tolerate a small sample offset.  The
     * old forward scan preferred expected-2 and could select a point before
     * the original loop anchor, causing a valid candidate to be discarded. */
    for (offset = 0U; offset <= RACE_EVENT_LOOP_SEARCH_RADIUS; ++offset) {
        if (expected_index >= offset) {
            index = expected_index - offset;
            if (index >= minimum_history_index && index < current_index &&
                race_event_point_matches(
                    current, gps_track_at(track, index),
                    RACE_EVENT_LOOP_CONTINUITY_DISTANCE_M,
                    RACE_EVENT_LOOP_CONTINUITY_HEADING_DEG)) {
                *matched_index = index;
                return true;
            }
        }
        if (offset > 0U && expected_index + offset < current_index &&
            race_event_point_matches(
                current, gps_track_at(track, expected_index + offset),
                RACE_EVENT_LOOP_CONTINUITY_DISTANCE_M,
                RACE_EVENT_LOOP_CONTINUITY_HEADING_DEG)) {
            *matched_index = expected_index + offset;
            return true;
        }
    }
    return false;
}

static bool race_event_update_loop_detector(RaceEvent *event,
                                            size_t current_index,
                                            size_t *loop_begin,
                                            size_t *loop_end)
{
    const GpsTrackPoint *current;
    const GpsTrackPoint *history;
    size_t expected_index;
    size_t matched_index;

    if (event == NULL || loop_begin == NULL || loop_end == NULL ||
        current_index >= event->gps_track.point_count) {
        return false;
    }
    current = gps_track_at(&event->gps_track, current_index);
    if (current == NULL) {
        return false;
    }
    if (event->loop_match_count == 0U) {
        if (!race_event_find_initial_match(&event->gps_track, current_index,
                                           &event->loop_history_index)) {
            return false;
        }
        event->loop_current_index = current_index;
        event->loop_match_count = 1U;
    } else {
        expected_index = event->loop_history_index +
                         (current_index - event->loop_current_index);
        if (!race_event_find_continuous_match(&event->gps_track, current_index,
                                              expected_index,
                                              event->loop_history_index,
                                              &matched_index)) {
            event->loop_match_count = 0U;
            return false;
        }
        if (matched_index != expected_index) {
            /* Keep the original pair as the phase reference, but tolerate
             * small GPS sampling gaps while matching the route. */
            if (matched_index < event->loop_history_index ||
                matched_index >= current_index) {
                event->loop_match_count = 0U;
                return false;
            }
        }
        event->loop_match_count++;
    }

    history = gps_track_at(&event->gps_track, event->loop_history_index);
    if (history == NULL || current->timestamp_us < history->timestamp_us ||
        current->timestamp_us - history->timestamp_us <
            RACE_EVENT_MIN_LOOP_TIME_US ||
        current->timestamp_us - history->timestamp_us >
            RACE_EVENT_MAX_LOOP_TIME_US ||
        current->total_distance_m - history->total_distance_m <
            RACE_EVENT_MIN_LOOP_DISTANCE_M) {
        return false;
    }
    if (event->loop_match_count < RACE_EVENT_LOOP_MIN_MATCHES) {
        return false;
    }

    *loop_begin = event->loop_history_index;
    *loop_end = event->loop_current_index;
    return *loop_begin < *loop_end && *loop_end < event->gps_track.point_count;
}

static void race_event_set_unknown_lap_time(const RaceEvent *event,
                                            uint64_t timestamp_us,
                                            VehicleState *state)
{
    state->lap_valid = false;
    state->delta_time_s = 0.0;
    if (event->movement_started &&
        timestamp_us >= event->movement_start_timestamp_us) {
        state->lap_time_s =
            (double)(timestamp_us - event->movement_start_timestamp_us) /
            1000000.0;
    } else {
        state->lap_time_s = 0.0;
    }
}

void race_event_init(RaceEvent *event, const char *trace_directory)
{
    if (event == NULL) {
        return;
    }
    memset(event, 0, sizeof(*event));
    event->mode = RACE_EVENT_UNKNOWN;
    gps_track_init(&event->gps_track);
    lap_timer_init(&event->lap_timer, trace_directory);
    accel_test_init(&event->accel_test);
}

void race_event_update(RaceEvent *event, uint64_t timestamp_us,
                       const LapProState *sample, VehicleState *state)
{
    GpsTrackPoint point;
    size_t loop_begin;
    size_t loop_end;
    size_t virtual_start;
    bool loop_detected;
    bool track_point_stored;
    size_t previous_point_count;

    if (event == NULL || sample == NULL || state == NULL ||
        sample->fix_type < 3U || !isfinite(sample->latitude) ||
        !isfinite(sample->longitude) || !isfinite(sample->speed_kmh) ||
        !isfinite(sample->heading_deg)) {
        if (event != NULL && state != NULL) {
            state->event_mode = event->mode;
        }
        return;
    }

    previous_point_count = event->gps_track.point_count;
    if (!gps_track_sample(&event->gps_track, timestamp_us, sample->latitude,
                          sample->longitude, sample->speed_kmh,
                          sample->heading_deg, sample->hacc_m,
                          RACE_EVENT_TRACK_SAMPLE_INTERVAL_US, &point,
                          &track_point_stored)) {
        return;
    }
    if (track_point_stored && previous_point_count >= GPS_TRACK_MAX_POINTS) {
        /* The track buffer compacted in place; old detector indexes are no
         * longer meaningful after that point. */
        event->loop_match_count = 0U;
    }

    if (!event->movement_started &&
        sample->speed_kmh > RACE_EVENT_MOVING_SPEED_KMH) {
        event->movement_started = true;
        event->movement_start_timestamp_us = timestamp_us;
        LOG_DEBUG("[RACE] movement started at timestamp_us=%llu",
                  (unsigned long long)timestamp_us);
    }

    accel_test_update(&event->accel_test, timestamp_us, sample, &point,
                      state);

    if (event->mode == RACE_EVENT_UNKNOWN) {
        loop_detected = track_point_stored &&
            race_event_update_loop_detector(
                event, event->gps_track.point_count - 1U, &loop_begin,
                &loop_end);
        if (loop_detected &&
            gps_track_find_virtual_start(&event->gps_track, loop_begin,
                                         loop_end, &virtual_start) &&
            lap_timer_configure_from_loop(
                &event->lap_timer, &event->gps_track, loop_begin, loop_end,
                virtual_start,
                event->movement_started
                    ? event->movement_start_timestamp_us
                    : gps_track_at(&event->gps_track, loop_begin)
                          ->timestamp_us,
                &point, state)) {
            const GpsTrackPoint *loop_start =
                gps_track_at(&event->gps_track, loop_begin);
            const GpsTrackPoint *loop_finish =
                gps_track_at(&event->gps_track, loop_end);
            double loop_time_s =
                loop_start != NULL && loop_finish != NULL
                    ? (double)(loop_finish->timestamp_us -
                               loop_start->timestamp_us) /
                          1000000.0
                    : 0.0;
            double loop_distance_m =
                loop_start != NULL && loop_finish != NULL
                    ? loop_finish->total_distance_m -
                          loop_start->total_distance_m
                    : 0.0;

            event->mode = RACE_EVENT_CIRCUIT_MODE;
            LOG_DEBUG("[RACE] loop candidate history=%zu current=%zu "
                      "distance=%.1fm",
                      loop_begin, loop_end, loop_distance_m);
            LOG_INFO("[RACE] event detected: CIRCUIT_MODE loop=%.2fs",
                     loop_time_s);
        }
        if (event->mode == RACE_EVENT_UNKNOWN) {
            race_event_set_unknown_lap_time(event, timestamp_us, state);
        }
    } else if (event->mode == RACE_EVENT_CIRCUIT_MODE) {
        lap_timer_update(&event->lap_timer, &point, state);
    }

    state->event_mode = event->mode;
    if (event->last_report_timestamp_us == 0U ||
        timestamp_us - event->last_report_timestamp_us >= 1000000ULL) {
        LOG_INFO("[RACE] mode=%s gps_points=%zu",
                 vehicle_state_event_mode_name(event->mode),
                 event->gps_track.point_count);
        event->last_report_timestamp_us = timestamp_us;
    }
}

void race_event_close(RaceEvent *event)
{
    if (event == NULL) {
        return;
    }
    lap_timer_close(&event->lap_timer);
    accel_test_close(&event->accel_test);
}
