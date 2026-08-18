#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lap_timer.h"
#include "console_log.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define LAP_TIMER_MAX_PROJECTION_DISTANCE_M 30.0
#define LAP_TIMER_MIN_LAP_TIME_S 3.0
#define LAP_TIMER_PREVIOUS_CROSSING_FRACTION 0.65
#define LAP_TIMER_CURRENT_CROSSING_FRACTION 0.10
#define LAP_TIMER_INITIAL_CROSSING_FRACTION 0.25

static void lap_timer_close_trace(LapTimer *timer)
{
    if (timer->trace_file != NULL) {
        fflush(timer->trace_file);
        fclose(timer->trace_file);
        timer->trace_file = NULL;
    }
}

static void lap_timer_open_trace(LapTimer *timer)
{
    char path[256];
    int written;

    lap_timer_close_trace(timer);
    if (timer->trace_directory[0] == '\0') {
        return;
    }
    written = snprintf(path, sizeof(path), "%s/lap_%03u.csv",
                       timer->trace_directory, timer->lap_number);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        LOG_WARN("[LAP] trace path is too long");
        return;
    }
    timer->trace_file = fopen(path, "w");
    if (timer->trace_file == NULL) {
        LOG_WARN("[LAP] cannot open trace %s: %s", path, strerror(errno));
        return;
    }
    setvbuf(timer->trace_file, NULL, _IOFBF, 4096U);
    if (fprintf(timer->trace_file,
                "timestamp_us,distance_s_m,time_s,x_m,y_m,speed_kmh,"
                "heading_deg\n") < 0) {
        lap_timer_close_trace(timer);
    }
}

static void lap_timer_write_trace(LapTimer *timer,
                                  const GpsTrackPoint *point)
{
    if (timer->trace_file == NULL ||
        fprintf(timer->trace_file,
                "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                (unsigned long long)point->timestamp_us,
                timer->current_distance_s_m, timer->current_time_s,
                point->x_m, point->y_m, point->speed_kmh,
                point->heading_deg) < 0) {
        if (timer->trace_file != NULL) {
            LOG_WARN("[LAP] trace write failed");
            lap_timer_close_trace(timer);
        }
    }
}

static size_t lap_timer_find_projection(const LapTimer *timer,
                                        const GpsTrackPoint *point)
{
    size_t index;
    size_t nearest = 0U;
    double nearest_distance = HUGE_VAL;

    for (index = 0U; index < timer->reference_count; ++index) {
        const LapReferencePoint *reference = &timer->reference[index];
        double distance = hypot(point->x_m - reference->x_m,
                                point->y_m - reference->y_m);
        double heading_error = fabs(point->heading_deg -
                                    reference->heading_deg);

        while (heading_error > 360.0) {
            heading_error -= 360.0;
        }
        if (heading_error > 180.0) {
            heading_error = 360.0 - heading_error;
        }
        if (heading_error > 60.0) {
            continue;
        }
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest = index;
        }
    }
    return nearest_distance <= LAP_TIMER_MAX_PROJECTION_DISTANCE_M
               ? nearest
               : timer->reference_count;
}

static bool lap_timer_crossed_virtual_start(const LapTimer *timer,
                                            size_t previous_index,
                                            size_t current_index,
                                            double current_fraction)
{
    double lap_distance_m;
    double previous_distance_m;
    double current_distance_m;

    if (timer == NULL || previous_index >= timer->reference_count ||
        current_index >= timer->reference_count || timer->reference_count < 2U) {
        return false;
    }
    lap_distance_m = timer->reference_loop_distance_s_m;
    if (lap_distance_m <= 0.1) {
        return false;
    }
    previous_distance_m = timer->reference[previous_index].distance_s_m;
    current_distance_m = timer->reference[current_index].distance_s_m;
    return previous_distance_m >=
               lap_distance_m * LAP_TIMER_PREVIOUS_CROSSING_FRACTION &&
           current_distance_m <=
               lap_distance_m * current_fraction;
}

static uint64_t lap_timer_crossing_timestamp(
    const LapTimer *timer, size_t previous_index, size_t current_index,
    uint64_t previous_timestamp_us, uint64_t current_timestamp_us)
{
    double lap_distance_m;
    double previous_distance_m;
    double current_distance_m;
    double distance_to_line_m;
    double crossed_segment_m;
    double fraction;

    if (timer == NULL || current_timestamp_us <= previous_timestamp_us ||
        previous_index >= timer->reference_count ||
        current_index >= timer->reference_count) {
        return current_timestamp_us;
    }
    lap_distance_m = timer->reference_loop_distance_s_m;
    previous_distance_m = timer->reference[previous_index].distance_s_m;
    current_distance_m = timer->reference[current_index].distance_s_m;
    distance_to_line_m = lap_distance_m - previous_distance_m;
    crossed_segment_m = distance_to_line_m + current_distance_m;
    if (crossed_segment_m <= 0.1) {
        return current_timestamp_us;
    }
    fraction = distance_to_line_m / crossed_segment_m;
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }
    return previous_timestamp_us +
           (uint64_t)((double)(current_timestamp_us - previous_timestamp_us) *
                      fraction);
}

static void lap_timer_set_state(const LapTimer *timer, VehicleState *state)
{
    state->lap_number = timer->lap_number;
    state->lap_valid = timer->configured &&
                       timer->lap_start_timestamp_us != 0U;
    state->lap_time_s = timer->current_time_s;
    state->current_lap_time_s = timer->current_time_s;
    state->last_lap_time_s = timer->last_lap_time_s;
    state->best_lap_time_s = timer->best_lap_time_s;
    state->delta_time_s = timer->delta_time_s;
    state->estimated_lap_time_s = timer->estimated_lap_time_s;
    state->track_distance_m = timer->current_distance_s_m;
}

void lap_timer_init(LapTimer *timer, const char *trace_directory)
{
    if (timer == NULL) {
        return;
    }
    memset(timer, 0, sizeof(*timer));
    if (trace_directory != NULL) {
        snprintf(timer->trace_directory, sizeof(timer->trace_directory), "%s",
                 trace_directory);
    }
}

bool lap_timer_configure_from_loop(LapTimer *timer, const GpsTrack *track,
                                   size_t loop_begin, size_t loop_end,
                                   size_t virtual_start,
                                   uint64_t initial_lap_start_timestamp_us,
                                   const GpsTrackPoint *current_point,
                                   VehicleState *state)
{
    const GpsTrackPoint *loop_start_point;
    const GpsTrackPoint *loop_end_point;
    const GpsTrackPoint *virtual_point;
    uint64_t loop_period_us;
    size_t loop_count;
    size_t virtual_offset;
    size_t index;

    if (timer == NULL || track == NULL || current_point == NULL ||
        state == NULL || loop_begin >= loop_end || loop_end > track->point_count ||
        virtual_start < loop_begin || virtual_start >= loop_end ||
        initial_lap_start_timestamp_us == 0U) {
        return false;
    }
    loop_count = loop_end - loop_begin;
    if (loop_count > GPS_TRACK_MAX_POINTS) {
        return false;
    }
    loop_start_point = gps_track_at(track, loop_begin);
    loop_end_point = gps_track_at(track, loop_end);
    virtual_point = gps_track_at(track, virtual_start);
    if (loop_start_point == NULL || loop_end_point == NULL ||
        virtual_point == NULL || loop_end_point->timestamp_us <=
                                     loop_start_point->timestamp_us) {
        return false;
    }
    loop_period_us = loop_end_point->timestamp_us -
                     loop_start_point->timestamp_us;
    virtual_offset = virtual_start - loop_begin;
    timer->reference_count = loop_count;
    timer->reference_lap_time_s = (double)loop_period_us / 1000000.0;
    for (index = 0U; index < loop_count; ++index) {
        size_t source_index = loop_begin +
                              (virtual_offset + index) % loop_count;
        const GpsTrackPoint *source = gps_track_at(track, source_index);
        LapReferencePoint *destination = &timer->reference[index];
        uint64_t relative_time_us;

        if (source == NULL) {
            return false;
        }
        destination->timestamp_us = source->timestamp_us;
        destination->x_m = source->x_m;
        destination->y_m = source->y_m;
        destination->speed_kmh = source->speed_kmh;
        destination->heading_deg = source->heading_deg;
        relative_time_us = source->timestamp_us >= virtual_point->timestamp_us
                               ? source->timestamp_us -
                                     virtual_point->timestamp_us
                               : source->timestamp_us + loop_period_us -
                                     virtual_point->timestamp_us;
        destination->time_s = (double)relative_time_us / 1000000.0;
        if (index == 0U) {
            destination->distance_s_m = 0.0;
        } else {
            const LapReferencePoint *previous =
                &timer->reference[index - 1U];

            destination->distance_s_m =
                previous->distance_s_m +
                hypot(destination->x_m - previous->x_m,
                     destination->y_m - previous->y_m);
        }
    }
    timer->reference_loop_distance_s_m =
        timer->reference[timer->reference_count - 1U].distance_s_m +
        hypot(timer->reference[timer->reference_count - 1U].x_m -
                 timer->reference[0].x_m,
             timer->reference[timer->reference_count - 1U].y_m -
                 timer->reference[0].y_m);
    timer->configured = true;
    timer->have_projection = false;
    timer->waiting_for_initial_crossing = true;
    timer->previous_projection_valid = false;
    timer->lap_number = 1U;
    timer->best_lap_time_s = timer->reference_lap_time_s;
    timer->last_lap_time_s = 0.0;
    timer->current_distance_s_m = 0.0;
    timer->current_time_s = 0.0;
    timer->delta_time_s = 0.0;
    timer->estimated_lap_time_s = timer->reference_lap_time_s;
    timer->lap_start_timestamp_us = initial_lap_start_timestamp_us;
    timer->previous_projection_timestamp_us = 0U;

    /* Start the first real crossing test at the original closed-loop point,
     * not at the delayed detector-confirmation point. */
    {
        size_t initial_projection =
            lap_timer_find_projection(timer, loop_end_point);

        if (initial_projection < timer->reference_count) {
            timer->previous_projection_index = initial_projection;
            timer->previous_projection_timestamp_us =
                loop_end_point->timestamp_us;
            timer->previous_projection_valid = true;
        }
    }
    lap_timer_open_trace(timer);
    if (current_point->timestamp_us >= timer->lap_start_timestamp_us) {
        timer->current_time_s =
            (double)(current_point->timestamp_us -
                     timer->lap_start_timestamp_us) /
            1000000.0;
    }
    lap_timer_set_state(timer, state);
    if (current_point->timestamp_us > loop_end_point->timestamp_us) {
        lap_timer_update(timer, current_point, state);
    }
    return timer->configured;
}

void lap_timer_update(LapTimer *timer, const GpsTrackPoint *point,
                      VehicleState *state)
{
    size_t projection_index;
    const LapReferencePoint *reference;
    uint64_t crossing_timestamp_us;
    bool wrapped = false;

    if (timer == NULL || point == NULL || state == NULL ||
        !timer->configured || timer->reference_count == 0U) {
        return;
    }
    projection_index = lap_timer_find_projection(timer, point);
    if (projection_index >= timer->reference_count) {
        return;
    }
    reference = &timer->reference[projection_index];
    if (!timer->previous_projection_valid) {
        timer->previous_projection_index = projection_index;
        timer->previous_projection_timestamp_us = point->timestamp_us;
        timer->previous_projection_valid = true;
        if (timer->waiting_for_initial_crossing) {
            if (point->timestamp_us >= timer->lap_start_timestamp_us) {
                timer->current_time_s =
                    (double)(point->timestamp_us -
                             timer->lap_start_timestamp_us) /
                    1000000.0;
            }
            timer->current_distance_s_m = 0.0;
            timer->delta_time_s = 0.0;
            lap_timer_set_state(timer, state);
            return;
        }
    }
    wrapped = lap_timer_crossed_virtual_start(
        timer, timer->previous_projection_index, projection_index,
        timer->waiting_for_initial_crossing
            ? LAP_TIMER_INITIAL_CROSSING_FRACTION
            : LAP_TIMER_CURRENT_CROSSING_FRACTION);
    crossing_timestamp_us = lap_timer_crossing_timestamp(
        timer, timer->previous_projection_index, projection_index,
        timer->previous_projection_timestamp_us, point->timestamp_us);

    if (timer->waiting_for_initial_crossing) {
        if (!wrapped) {
            timer->previous_projection_index = projection_index;
            timer->previous_projection_timestamp_us = point->timestamp_us;
            if (point->timestamp_us >= timer->lap_start_timestamp_us) {
                timer->current_time_s =
                    (double)(point->timestamp_us -
                             timer->lap_start_timestamp_us) /
                    1000000.0;
            }
            timer->current_distance_s_m = 0.0;
            timer->delta_time_s = 0.0;
            lap_timer_set_state(timer, state);
            return;
        }

        timer->last_lap_time_s =
            crossing_timestamp_us >= timer->lap_start_timestamp_us
                ? (double)(crossing_timestamp_us -
                           timer->lap_start_timestamp_us) /
                      1000000.0
                : 0.0;
        if (timer->last_lap_time_s < timer->best_lap_time_s) {
            timer->best_lap_time_s = timer->last_lap_time_s;
        }
        LOG_INFO("[LAP] lap=1 time=%.2fs best=%.2fs",
                 timer->last_lap_time_s, timer->best_lap_time_s);
        timer->waiting_for_initial_crossing = false;
        timer->have_projection = true;
        timer->lap_number = 2U;
        timer->lap_start_timestamp_us = crossing_timestamp_us;
        timer->current_time_s =
            point->timestamp_us >= crossing_timestamp_us
                ? (double)(point->timestamp_us - crossing_timestamp_us) /
                      1000000.0
                : 0.0;
        timer->current_distance_s_m = reference->distance_s_m;
        timer->delta_time_s = timer->current_time_s - reference->time_s;
        timer->estimated_lap_time_s = timer->reference_lap_time_s;
        timer->previous_projection_index = projection_index;
        timer->previous_projection_timestamp_us = point->timestamp_us;
        lap_timer_open_trace(timer);
        lap_timer_write_trace(timer, point);
        lap_timer_set_state(timer, state);
        return;
    }

    if (wrapped && timer->lap_start_timestamp_us != 0U &&
        crossing_timestamp_us >= timer->lap_start_timestamp_us &&
        (double)(crossing_timestamp_us - timer->lap_start_timestamp_us) /
                1000000.0 >=
            LAP_TIMER_MIN_LAP_TIME_S) {
        wrapped = true;
    } else {
        wrapped = false;
    }
    if (wrapped) {
        timer->last_lap_time_s =
            (double)(crossing_timestamp_us - timer->lap_start_timestamp_us) /
            1000000.0;
        if (timer->last_lap_time_s < timer->best_lap_time_s) {
            timer->best_lap_time_s = timer->last_lap_time_s;
        }
        LOG_INFO("[LAP] lap=%u time=%.2fs best=%.2fs delta=%+.2fs",
                 timer->lap_number, timer->last_lap_time_s,
                 timer->best_lap_time_s, timer->delta_time_s);
        timer->lap_number++;
        timer->lap_start_timestamp_us = crossing_timestamp_us;
        lap_timer_open_trace(timer);
    }
    timer->have_projection = true;
    timer->previous_projection_index = projection_index;
    timer->previous_projection_timestamp_us = point->timestamp_us;
    timer->current_distance_s_m = reference->distance_s_m;
    timer->current_time_s =
        (double)(point->timestamp_us - timer->lap_start_timestamp_us) /
        1000000.0;
    timer->delta_time_s = timer->current_time_s - reference->time_s;
    if (timer->current_distance_s_m > 0.1 &&
        timer->reference_loop_distance_s_m > 0.1) {
        timer->estimated_lap_time_s =
            timer->current_time_s *
            timer->reference_loop_distance_s_m /
            timer->current_distance_s_m;
    } else {
        timer->estimated_lap_time_s = timer->reference_lap_time_s;
    }
    lap_timer_write_trace(timer, point);
    if (timer->last_report_timestamp_us == 0U ||
        point->timestamp_us - timer->last_report_timestamp_us >= 1000000ULL) {
        LOG_INFO("[LAP] lap=%u distance=%.1fm delta=%+.2fs",
                 timer->lap_number, timer->current_distance_s_m,
                 timer->delta_time_s);
        timer->last_report_timestamp_us = point->timestamp_us;
    }
    lap_timer_set_state(timer, state);
}

void lap_timer_close(LapTimer *timer)
{
    if (timer == NULL) {
        return;
    }
    lap_timer_close_trace(timer);
    timer->configured = false;
}
