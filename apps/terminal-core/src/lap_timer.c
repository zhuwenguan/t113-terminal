#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lap_timer.h"
#include "console_log.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define LAP_TIMER_MAX_PROJECTION_DISTANCE_M 30.0
#define LAP_TIMER_MIN_LAP_TIME_S 3.0
#define LAP_TIMER_START_HALF_WIDTH_M 20.0
#define LAP_TIMER_TRACE_MAX_ATTEMPTS 10000U
#define LAP_TIMER_REFERENCE_SAMPLE_INTERVAL_US 200000ULL

static FILE *lap_timer_open_trace_file(const LapTimer *timer,
                                       uint32_t lap_number, char *path,
                                       size_t path_size)
{
    unsigned int suffix;

    for (suffix = 0U; suffix < LAP_TIMER_TRACE_MAX_ATTEMPTS; ++suffix) {
        int written;
        int descriptor;
        FILE *file;

        written = suffix == 0U
                      ? snprintf(path, path_size, "%s/lap_%03u.csv",
                                 timer->trace_directory, lap_number)
                      : snprintf(path, path_size, "%s/lap_%03u_%03u.csv",
                                 timer->trace_directory, lap_number, suffix);
        if (written < 0 || (size_t)written >= path_size) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (descriptor < 0) {
            if (errno == EEXIST) {
                continue;
            }
            return NULL;
        }
        file = fdopen(descriptor, "w");
        if (file == NULL) {
            int saved_errno = errno;

            close(descriptor);
            errno = saved_errno;
            return NULL;
        }
        return file;
    }

    errno = EEXIST;
    return NULL;
}

static void lap_timer_trace_close_file(LapTimer *timer)
{
    if (timer->trace_file == NULL) {
        return;
    }
    if (fflush(timer->trace_file) < 0) {
        timer->trace_write_errors++;
        LOG_WARN("[LAP] trace flush failed");
    }
    if (fclose(timer->trace_file) != 0) {
        timer->trace_write_errors++;
        LOG_WARN("[LAP] trace close failed");
    }
    timer->trace_file = NULL;
}

static bool lap_timer_trace_open_file(LapTimer *timer, uint32_t lap_number)
{
    char path[256] = {0};

    if (timer->trace_file != NULL &&
        timer->trace_file_lap_number == lap_number) {
        return true;
    }
    lap_timer_trace_close_file(timer);
    if (timer->trace_file_failed &&
        timer->trace_file_lap_number == lap_number) {
        return false;
    }
    timer->trace_file_lap_number = lap_number;
    timer->trace_file_failed = false;
    timer->trace_file = lap_timer_open_trace_file(timer, lap_number, path,
                                                  sizeof(path));
    if (timer->trace_file == NULL) {
        timer->trace_file_failed = true;
        timer->trace_write_errors++;
        LOG_WARN("[LAP] cannot open trace %s: %s",
                 path[0] != '\0' ? path : timer->trace_directory,
                 strerror(errno));
        return false;
    }
    setvbuf(timer->trace_file, NULL, _IOFBF, 4096U);
    if (fprintf(timer->trace_file,
                "timestamp_us,distance_s_m,time_s,x_m,y_m,speed_kmh,"
                "heading_deg\n") < 0) {
        timer->trace_write_errors++;
        timer->trace_file_failed = true;
        LOG_WARN("[LAP] trace header write failed");
        lap_timer_trace_close_file(timer);
        return false;
    }
    return true;
}

static void lap_timer_trace_write_record(LapTimer *timer,
                                         const LapTraceRecord *record)
{
    if (!lap_timer_trace_open_file(timer, record->lap_number)) {
        return;
    }
    if (record->open_only) {
        return;
    }
    if (fprintf(timer->trace_file,
                "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                (unsigned long long)record->timestamp_us,
                record->distance_s_m, record->time_s, record->x_m,
                record->y_m, record->speed_kmh, record->heading_deg) < 0) {
        timer->trace_write_errors++;
        timer->trace_file_failed = true;
        LOG_WARN("[LAP] trace write failed");
        lap_timer_trace_close_file(timer);
    }
}

static void *lap_timer_trace_writer_main(void *context)
{
    LapTimer *timer = context;

    for (;;) {
        LapTraceRecord record;
        int wait_result;

        if (pthread_mutex_lock(&timer->trace_mutex) != 0) {
            LOG_ERROR("[LAP] cannot lock trace queue");
            break;
        }
        while (timer->trace_queue_count == 0U &&
               !timer->trace_stop_requested) {
            wait_result = pthread_cond_wait(&timer->trace_condition,
                                            &timer->trace_mutex);
            if (wait_result != 0) {
                LOG_ERROR("[LAP] trace queue wait failed: %s",
                          strerror(wait_result));
                timer->trace_stop_requested = true;
                break;
            }
        }
        if (timer->trace_queue_count == 0U &&
            timer->trace_stop_requested) {
            pthread_mutex_unlock(&timer->trace_mutex);
            break;
        }
        record = timer->trace_queue[timer->trace_queue_head];
        timer->trace_queue_head =
            (timer->trace_queue_head + 1U) % LAP_TIMER_TRACE_QUEUE_CAPACITY;
        timer->trace_queue_count--;
        pthread_mutex_unlock(&timer->trace_mutex);

        lap_timer_trace_write_record(timer, &record);
    }

    lap_timer_trace_close_file(timer);
    return NULL;
}

static void lap_timer_trace_queue_record(LapTimer *timer,
                                         const LapTraceRecord *record)
{
    if (!timer->trace_queue_initialized || !timer->trace_thread_started) {
        return;
    }
    if (pthread_mutex_lock(&timer->trace_mutex) != 0) {
        return;
    }
    if (timer->trace_stop_requested ||
        timer->trace_queue_count >= LAP_TIMER_TRACE_QUEUE_CAPACITY) {
        timer->trace_dropped_records++;
        pthread_mutex_unlock(&timer->trace_mutex);
        return;
    }
    timer->trace_queue[timer->trace_queue_tail] = *record;
    timer->trace_queue_tail =
        (timer->trace_queue_tail + 1U) % LAP_TIMER_TRACE_QUEUE_CAPACITY;
    timer->trace_queue_count++;
    pthread_cond_signal(&timer->trace_condition);
    pthread_mutex_unlock(&timer->trace_mutex);
}

static void lap_timer_trace_queue_open(LapTimer *timer)
{
    LapTraceRecord record;

    memset(&record, 0, sizeof(record));
    record.open_only = true;
    record.lap_number = timer->lap_number;
    lap_timer_trace_queue_record(timer, &record);
}

static void lap_timer_trace_queue_sample(LapTimer *timer,
                                         const GpsTrackPoint *point)
{
    LapTraceRecord record;

    memset(&record, 0, sizeof(record));
    record.lap_number = timer->lap_number;
    record.timestamp_us = point->timestamp_us;
    record.x_m = point->x_m;
    record.y_m = point->y_m;
    record.distance_s_m = timer->current_distance_s_m;
    record.time_s = timer->current_time_s;
    record.speed_kmh = point->speed_kmh;
    record.heading_deg = point->heading_deg;
    lap_timer_trace_queue_record(timer, &record);
}

static int lap_timer_trace_start(LapTimer *timer)
{
    int result;

    result = pthread_mutex_init(&timer->trace_mutex, NULL);
    if (result != 0) {
        LOG_WARN("[LAP] cannot initialize trace queue mutex: %s",
                 strerror(result));
        return -1;
    }
    result = pthread_cond_init(&timer->trace_condition, NULL);
    if (result != 0) {
        LOG_WARN("[LAP] cannot initialize trace queue condition: %s",
                 strerror(result));
        pthread_mutex_destroy(&timer->trace_mutex);
        return -1;
    }
    timer->trace_queue_initialized = true;
    result = pthread_create(&timer->trace_thread, NULL,
                            lap_timer_trace_writer_main, timer);
    if (result != 0) {
        LOG_WARN("[LAP] trace writer did not start: %s", strerror(result));
        pthread_cond_destroy(&timer->trace_condition);
        pthread_mutex_destroy(&timer->trace_mutex);
        timer->trace_queue_initialized = false;
        return -1;
    }
    timer->trace_thread_started = true;
    return 0;
}

static bool lap_timer_trace_stop(LapTimer *timer)
{
    int result;

    if (!timer->trace_queue_initialized) {
        return true;
    }
    if (timer->trace_thread_started) {
        result = pthread_mutex_lock(&timer->trace_mutex);
        if (result == 0) {
            timer->trace_stop_requested = true;
            pthread_cond_signal(&timer->trace_condition);
            pthread_mutex_unlock(&timer->trace_mutex);
        } else {
            LOG_WARN("[LAP] cannot stop trace writer: %s", strerror(result));
            return false;
        }
        result = pthread_join(timer->trace_thread, NULL);
        if (result != 0) {
            LOG_WARN("[LAP] cannot join trace writer: %s", strerror(result));
            return false;
        }
        timer->trace_thread_started = false;
    }
    pthread_cond_destroy(&timer->trace_condition);
    pthread_mutex_destroy(&timer->trace_mutex);
    timer->trace_queue_initialized = false;
    if (timer->trace_dropped_records > 0U) {
        LOG_WARN("[LAP] trace queue dropped=%llu records",
                 (unsigned long long)timer->trace_dropped_records);
    }
    if (timer->trace_write_errors > 0U) {
        LOG_WARN("[LAP] trace write errors=%llu",
                 (unsigned long long)timer->trace_write_errors);
    }
    return true;
}

typedef struct {
    double distance_s_m;
    double time_s;
} LapProjection;

static double lap_timer_heading_difference(double a, double b)
{
    return remainder(a - b, 360.0);
}

/* Project onto segments, then interpolate the reference clock. Squared
 * distances avoid a square root for every candidate in the 50 Hz path. */
static bool lap_timer_find_projection(const LapTimer *timer,
                                      const GpsTrackPoint *point,
                                      LapProjection *projection)
{
    double nearest_squared = HUGE_VAL;
    double start_side = (point->x_m - timer->start_x_m) *
                            timer->start_direction_x +
                        (point->y_m - timer->start_y_m) *
                            timer->start_direction_y;
    double gate_dx = point->x_m - timer->start_x_m;
    double gate_dy = point->y_m - timer->start_y_m;
    bool near_gate = gate_dx * gate_dx + gate_dy * gate_dy <=
        LAP_TIMER_MAX_PROJECTION_DISTANCE_M * LAP_TIMER_MAX_PROJECTION_DISTANCE_M;
    size_t index;

    for (index = 0U; index + 1U < timer->reference_count; ++index) {
        const LapReferencePoint *a = &timer->reference[index];
        const LapReferencePoint *b = &timer->reference[index + 1U];
        double dx = b->x_m - a->x_m;
        double dy = b->y_m - a->y_m;
        double length_squared = dx * dx + dy * dy;
        double u, x, y, distance_squared, heading;

        /* Different driving lines need not share identical endpoints.
         * Select the correct side of the clock seam even when the other
         * endpoint is spatially closer. */
        if (near_gate &&
            ((start_side < 0.0 && b->time_s < timer->reference_lap_time_s * 0.5) ||
             (start_side >= 0.0 && a->time_s > timer->reference_lap_time_s * 0.5))) {
            continue;
        }
        if (length_squared < 1e-8) {
            continue;
        }
        u = ((point->x_m - a->x_m) * dx +
             (point->y_m - a->y_m) * dy) / length_squared;
        u = fmax(0.0, fmin(1.0, u));
        x = point->x_m - (a->x_m + u * dx);
        y = point->y_m - (a->y_m + u * dy);
        distance_squared = x * x + y * y;
        if (distance_squared >= nearest_squared) {
            continue;
        }
        heading = a->heading_deg + u *
            lap_timer_heading_difference(b->heading_deg, a->heading_deg);
        if (fabs(lap_timer_heading_difference(point->heading_deg, heading)) >
            60.0) {
            continue;
        }
        nearest_squared = distance_squared;
        projection->distance_s_m = a->distance_s_m +
            u * (b->distance_s_m - a->distance_s_m);
        projection->time_s = a->time_s + u * (b->time_s - a->time_s);
    }
    return nearest_squared <= LAP_TIMER_MAX_PROJECTION_DISTANCE_M *
                              LAP_TIMER_MAX_PROJECTION_DISTANCE_M;
}

static double lap_timer_start_side(const LapTimer *timer,
                                   const GpsTrackPoint *point)
{
    return (point->x_m - timer->start_x_m) * timer->start_direction_x +
           (point->y_m - timer->start_y_m) * timer->start_direction_y;
}

/* A directed, finite gate. The timestamp comes from the actual GPS segment,
 * independently of the reference's sample spacing and accumulated length. */
static bool lap_timer_crossing(const LapTimer *timer,
                               const GpsTrackPoint *previous,
                               const GpsTrackPoint *point,
                               GpsTrackPoint *crossing)
{
    double before = lap_timer_start_side(timer, previous);
    double after = lap_timer_start_side(timer, point);
    double u, lateral;

    if (point->timestamp_us <= previous->timestamp_us ||
        before >= 0.0 || after < 0.0) {
        return false;
    }
    u = -before / (after - before);
    *crossing = *point;
    crossing->x_m = previous->x_m + u * (point->x_m - previous->x_m);
    crossing->y_m = previous->y_m + u * (point->y_m - previous->y_m);
    lateral = (crossing->x_m - timer->start_x_m) * timer->start_direction_y -
              (crossing->y_m - timer->start_y_m) * timer->start_direction_x;
    if (fabs(lateral) > LAP_TIMER_START_HALF_WIDTH_M) {
        return false;
    }
    crossing->timestamp_us = previous->timestamp_us + (uint64_t)llround(
        u * (double)(point->timestamp_us - previous->timestamp_us));
    crossing->speed_kmh = previous->speed_kmh +
        u * (point->speed_kmh - previous->speed_kmh);
    crossing->heading_deg = previous->heading_deg + u *
        lap_timer_heading_difference(point->heading_deg, previous->heading_deg);
    return true;
}

static bool lap_timer_append_current_lap_point(
    LapTimer *timer, uint64_t timestamp_us, double x_m, double y_m,
    double speed_kmh, double heading_deg, bool force)
{
    LapReferencePoint *destination;

    if (!force && timer->current_lap_count > 0U &&
        timestamp_us >=
            timer->current_lap[timer->current_lap_count - 1U].timestamp_us &&
        timestamp_us -
                timer->current_lap[timer->current_lap_count - 1U].timestamp_us <
            LAP_TIMER_REFERENCE_SAMPLE_INTERVAL_US) {
        return true;
    }
    if (timer->current_lap_count >= LAP_TIMER_REFERENCE_CAPACITY) {
        timer->current_lap_overflow = true;
        return false;
    }
    destination = &timer->current_lap[timer->current_lap_count];
    memset(destination, 0, sizeof(*destination));
    destination->timestamp_us = timestamp_us;
    destination->x_m = x_m;
    destination->y_m = y_m;
    destination->speed_kmh = speed_kmh;
    destination->heading_deg = heading_deg;
    if (timer->current_lap_count > 0U) {
        const LapReferencePoint *previous =
            &timer->current_lap[timer->current_lap_count - 1U];

        destination->distance_s_m =
            previous->distance_s_m +
            hypot(destination->x_m - previous->x_m,
                 destination->y_m - previous->y_m);
    }
    destination->time_s = timestamp_us >= timer->current_lap_start_timestamp_us
                              ? (double)(timestamp_us -
                                         timer->current_lap_start_timestamp_us) /
                                    1000000.0
                              : 0.0;
    timer->current_lap_count++;
    return true;
}

static void lap_timer_begin_current_lap(LapTimer *timer,
                                        const GpsTrackPoint *crossing)
{
    timer->current_lap_count = 0U;
    timer->current_lap_start_timestamp_us = crossing->timestamp_us;
    timer->current_lap_overflow = false;
    lap_timer_append_current_lap_point(
        timer, crossing->timestamp_us, crossing->x_m, crossing->y_m,
        crossing->speed_kmh, crossing->heading_deg, true);
}

static bool lap_timer_rebuild_reference_from_current_lap(
    LapTimer *timer, double lap_time_s)
{
    size_t index;
    double loop_distance_m;

    if (timer->current_lap_overflow || timer->current_lap_count < 2U ||
        lap_time_s <= 0.0 || !isfinite(lap_time_s)) {
        return false;
    }
    for (index = 0U; index < timer->current_lap_count; ++index) {
        LapReferencePoint *point = &timer->current_lap[index];

        if (index == 0U) {
            point->distance_s_m = 0.0;
        } else {
            const LapReferencePoint *previous =
                &timer->current_lap[index - 1U];

            point->distance_s_m =
                previous->distance_s_m +
                hypot(point->x_m - previous->x_m,
                      point->y_m - previous->y_m);
        }
        point->time_s =
            point->timestamp_us >= timer->current_lap_start_timestamp_us
                ? (double)(point->timestamp_us -
                           timer->current_lap_start_timestamp_us) /
                      1000000.0
                : 0.0;
    }
    /* Both endpoints lie on the same fixed gate; there is no timed
     * lateral closing segment between different driving lines. */
    loop_distance_m =
        timer->current_lap[timer->current_lap_count - 1U].distance_s_m;
    if (loop_distance_m <= 0.1 || !isfinite(loop_distance_m)) {
        return false;
    }
    timer->reference_count = timer->current_lap_count;
    memcpy(timer->reference, timer->current_lap,
           timer->reference_count * sizeof(timer->reference[0]));
    timer->reference_lap_time_s = lap_time_s;
    timer->reference_loop_distance_s_m = loop_distance_m;
    return true;
}

static void lap_timer_finish_current_lap(LapTimer *timer,
                                         const GpsTrackPoint *crossing)
{
    double reference_time_s =
        (double)(crossing->timestamp_us - timer->current_lap_start_timestamp_us) /
        1000000.0;

    lap_timer_append_current_lap_point(
        timer, crossing->timestamp_us, crossing->x_m, crossing->y_m,
        crossing->speed_kmh, crossing->heading_deg, true);
    /* The first historical line-to-line trace is already usable. The
     * displayed first result may include the run from movement to the gate. */
    if (timer->waiting_for_initial_crossing ||
        reference_time_s < timer->best_lap_time_s) {
        if (lap_timer_rebuild_reference_from_current_lap(timer,
                                                        reference_time_s)) {
            timer->best_lap_time_s = reference_time_s;
        } else {
            LOG_WARN("[LAP] lap %.2fs has no complete reference trace",
                     reference_time_s);
        }
    }
}

static void lap_timer_set_state(const LapTimer *timer, VehicleState *state)
{
    state->lap_number = timer->lap_number;
    state->lap_valid = timer->configured &&
                       timer->lap_start_timestamp_us != 0U;
    state->lap_time_s = timer->current_time_s;
    state->last_lap_time_s = timer->last_lap_time_s;
    state->best_lap_time_s = timer->best_lap_time_s;
    state->delta_time_s = timer->delta_time_s;
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
        if (timer->trace_directory[0] != '\0' &&
            lap_timer_trace_start(timer) < 0) {
            LOG_WARN("[LAP] trace writer unavailable; traces disabled");
        }
    }
}

bool lap_timer_configure_from_loop(LapTimer *timer, const GpsTrack *track,
                                   size_t loop_begin, size_t loop_end,
                                   size_t virtual_start,
                                   uint64_t initial_lap_start_timestamp_us,
                                   const GpsTrackPoint *current_point,
                                   VehicleState *state)
{
    const GpsTrackPoint *start;
    const GpsTrackPoint *before;
    const GpsTrackPoint *after;
    double dx, dy, length;
    size_t index;

    if (timer == NULL || track == NULL || current_point == NULL ||
        state == NULL || loop_begin >= loop_end || loop_end >= track->point_count ||
        virtual_start < loop_begin || virtual_start >= loop_end ||
        initial_lap_start_timestamp_us == 0U) {
        return false;
    }
    start = gps_track_at(track, virtual_start);
    before = gps_track_at(track, virtual_start > loop_begin
        ? virtual_start - 1U : loop_end - 1U);
    after = gps_track_at(track, virtual_start + 1U);
    dx = after->x_m - before->x_m;
    dy = after->y_m - before->y_m;
    length = hypot(dx, dy);
    if (length < 0.1 || current_point->timestamp_us < start->timestamp_us) {
        return false;
    }
    timer->start_x_m = start->x_m;
    timer->start_y_m = start->y_m;
    timer->start_direction_x = dx / length;
    timer->start_direction_y = dy / length;
    timer->configured = true;
    timer->waiting_for_initial_crossing = true;
    timer->reference_count = 0U;
    timer->reference_lap_time_s = 0.0;
    timer->reference_loop_distance_s_m = 0.0;
    timer->best_lap_time_s = 0.0;
    timer->last_lap_time_s = 0.0;
    timer->lap_number = 1U;
    timer->lap_start_timestamp_us = initial_lap_start_timestamp_us;
    timer->current_distance_s_m = 0.0;
    timer->delta_time_s = 0.0;
    timer->previous_point = *start;
    timer->previous_point_valid = true;
    lap_timer_begin_current_lap(timer, start);
    lap_timer_trace_queue_open(timer);

    /* Import the historical first lap, including the detector's confirmation
     * samples. An already observed finish is settled now, never one lap later.
     * Do not burst the whole historical trace into the bounded writer queue. */
    for (index = virtual_start + 1U; index < loop_end; ++index) {
        const GpsTrackPoint *point = gps_track_at(track, index);
        lap_timer_append_current_lap_point(
            timer, point->timestamp_us, point->x_m, point->y_m,
            point->speed_kmh, point->heading_deg, false);
        timer->previous_point = *point;
    }
    for (index = loop_end; index < track->point_count; ++index) {
        const GpsTrackPoint *point = gps_track_at(track, index);
        if (point->timestamp_us > current_point->timestamp_us) {
            break;
        }
        lap_timer_update(timer, point, state);
    }
    if (current_point->timestamp_us > timer->previous_point.timestamp_us) {
        lap_timer_update(timer, current_point, state);
    }
    lap_timer_set_state(timer, state);
    return true;
}

void lap_timer_update(LapTimer *timer, const GpsTrackPoint *point,
                      VehicleState *state)
{
    GpsTrackPoint crossing;
    LapProjection projection = {0};
    bool crossed;

    if (timer == NULL || point == NULL || state == NULL || !timer->configured ||
        (timer->previous_point_valid &&
         point->timestamp_us <= timer->previous_point.timestamp_us)) {
        return;
    }
    crossed = timer->previous_point_valid &&
        lap_timer_crossing(timer, &timer->previous_point, point, &crossing) &&
        crossing.timestamp_us >= timer->current_lap_start_timestamp_us &&
        (double)(crossing.timestamp_us - timer->current_lap_start_timestamp_us) /
            1000000.0 >= LAP_TIMER_MIN_LAP_TIME_S;
    if (crossed) {
        timer->last_lap_time_s = crossing.timestamp_us >= timer->lap_start_timestamp_us
            ? (double)(crossing.timestamp_us - timer->lap_start_timestamp_us) /
                  1000000.0 : 0.0;
        lap_timer_finish_current_lap(timer, &crossing);
        LOG_INFO("[LAP] lap=%u time=%.3fs best=%.3fs",
                 timer->lap_number, timer->last_lap_time_s,
                 timer->best_lap_time_s);
        timer->waiting_for_initial_crossing = false;
        timer->lap_number++;
        timer->lap_start_timestamp_us = crossing.timestamp_us;
        lap_timer_begin_current_lap(timer, &crossing);
        lap_timer_trace_queue_open(timer);
    }
    timer->current_time_s = point->timestamp_us >= timer->lap_start_timestamp_us
        ? (double)(point->timestamp_us - timer->lap_start_timestamp_us) / 1000000.0
        : 0.0;
    if (timer->waiting_for_initial_crossing) {
        timer->current_distance_s_m = 0.0;
        timer->delta_time_s = 0.0;
    } else if (lap_timer_find_projection(timer, point, &projection)) {
        timer->current_distance_s_m = projection.distance_s_m;
        timer->delta_time_s = timer->current_time_s - projection.time_s;
    }
    lap_timer_append_current_lap_point(
        timer, point->timestamp_us, point->x_m, point->y_m,
        point->speed_kmh, point->heading_deg, false);
    timer->previous_point = *point;
    timer->previous_point_valid = true;
    lap_timer_trace_queue_sample(timer, point);
    if (timer->last_report_timestamp_us == 0U ||
        point->timestamp_us - timer->last_report_timestamp_us >= 1000000ULL) {
        LOG_INFO("[LAP] lap=%u distance=%.1fm delta=%+.3fs",
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
    if (lap_timer_trace_stop(timer)) {
        timer->configured = false;
    } else {
        LOG_WARN("[LAP] trace writer remains active; keeping synchronization "
                 "objects alive");
    }
}
