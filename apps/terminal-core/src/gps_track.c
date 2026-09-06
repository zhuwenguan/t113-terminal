#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "gps_track.h"

#include <math.h>
#include <string.h>

#define GPS_TRACK_EARTH_RADIUS_M 6371000.0
#define GPS_TRACK_PI 3.14159265358979323846
#define GPS_TRACK_HEADING_STABILITY_DEG 18.0
#define GPS_TRACK_MAX_HACC_M 5.0
#define GPS_TRACK_STRAIGHT_WINDOW 5U
#define GPS_TRACK_VIRTUAL_START_FRACTION 0.15
/* Prefer an earlier gate when straightness differs by less than 0.2%. */
#define GPS_TRACK_STRAIGHT_SCORE_TOLERANCE 0.002

static double gps_track_radians(double degrees)
{
    return degrees * GPS_TRACK_PI / 180.0;
}

static double gps_track_heading_error(double first_deg, double second_deg)
{
    double error = fabs(first_deg - second_deg);

    while (error > 360.0) {
        error -= 360.0;
    }
    return error > 180.0 ? 360.0 - error : error;
}

double gps_track_distance(const GpsTrackPoint *a, const GpsTrackPoint *b)
{
    if (a == NULL || b == NULL) {
        return 0.0;
    }
    return hypot(b->x_m - a->x_m, b->y_m - a->y_m);
}

void gps_track_init(GpsTrack *track)
{
    if (track == NULL) {
        return;
    }
    memset(track, 0, sizeof(*track));
}

bool gps_track_sample(GpsTrack *track, uint64_t timestamp_us,
                      double latitude, double longitude, double speed_kmh,
                      double heading_deg, double hacc_m,
                      uint64_t minimum_store_interval_us,
                      GpsTrackPoint *current_point, bool *stored)
{
    GpsTrackPoint point;
    GpsTrackPoint previous_point;
    double latitude_scale;
    double delta_latitude;
    double delta_longitude;
    bool should_store;

    if (track == NULL || current_point == NULL || stored == NULL) {
        return false;
    }

    memset(&point, 0, sizeof(point));
    point.timestamp_us = timestamp_us;
    point.speed_kmh = speed_kmh;
    point.heading_deg = heading_deg;
    point.hacc_m = hacc_m;
    if (!track->initialized) {
        track->initialized = true;
        track->origin_latitude = latitude;
        track->origin_longitude = longitude;
        track->previous_latitude = latitude;
        track->previous_longitude = longitude;
    }

    latitude_scale = cos(gps_track_radians(track->origin_latitude));
    delta_latitude = gps_track_radians(latitude - track->origin_latitude);
    delta_longitude = gps_track_radians(longitude - track->origin_longitude);
    point.x_m = delta_longitude * GPS_TRACK_EARTH_RADIUS_M * latitude_scale;
    point.y_m = delta_latitude * GPS_TRACK_EARTH_RADIUS_M;
    memset(&previous_point, 0, sizeof(previous_point));
    if (track->point_count > 0U) {
        double previous_delta_latitude = gps_track_radians(
            track->previous_latitude - track->origin_latitude);
        double previous_delta_longitude = gps_track_radians(
            track->previous_longitude - track->origin_longitude);

        previous_point.x_m = previous_delta_longitude *
                             GPS_TRACK_EARTH_RADIUS_M * latitude_scale;
        previous_point.y_m = previous_delta_latitude *
                             GPS_TRACK_EARTH_RADIUS_M;
        track->total_distance_m += gps_track_distance(&point, &previous_point);
    }
    point.total_distance_m = track->total_distance_m;
    track->previous_latitude = latitude;
    track->previous_longitude = longitude;
    should_store = track->point_count == 0U ||
        timestamp_us < track->points[track->point_count - 1U].timestamp_us ||
        timestamp_us - track->points[track->point_count - 1U].timestamp_us >=
            minimum_store_interval_us;
    if (should_store) {
        if (track->point_count >= GPS_TRACK_MAX_POINTS) {
            /* Keep the most recent half without allocating memory. */
            memmove(track->points,
                    track->points + GPS_TRACK_MAX_POINTS / 2U,
                    (GPS_TRACK_MAX_POINTS / 2U) * sizeof(track->points[0]));
            track->point_count = GPS_TRACK_MAX_POINTS / 2U;
        }
        if (track->point_count > 0U) {
            point.segment_distance_m = point.total_distance_m -
                track->points[track->point_count - 1U].total_distance_m;
        }
        track->points[track->point_count++] = point;
    }
    *current_point = point;
    *stored = should_store;
    return true;
}

bool gps_track_append(GpsTrack *track, uint64_t timestamp_us,
                      double latitude, double longitude, double speed_kmh,
                      double heading_deg, double hacc_m,
                      GpsTrackPoint *current_point)
{
    bool stored = false;

    return gps_track_sample(track, timestamp_us, latitude, longitude,
                            speed_kmh, heading_deg, hacc_m, 0U,
                            current_point, &stored) && stored;
}

const GpsTrackPoint *gps_track_at(const GpsTrack *track, size_t index)
{
    if (track == NULL || index >= track->point_count) {
        return NULL;
    }
    return &track->points[index];
}

static bool gps_track_is_straight_window(const GpsTrack *track, size_t index,
                                         size_t end_index)
{
    size_t end = index + GPS_TRACK_STRAIGHT_WINDOW;
    size_t current;

    if (end > end_index) {
        return false;
    }
    if (!isfinite(track->points[index].hacc_m) ||
        track->points[index].hacc_m > GPS_TRACK_MAX_HACC_M) {
        return false;
    }
    for (current = index + 1U; current < end; ++current) {
        const GpsTrackPoint *previous = gps_track_at(track, current - 1U);
        const GpsTrackPoint *point = gps_track_at(track, current);

        if (previous == NULL || point == NULL ||
            gps_track_heading_error(previous->heading_deg,
                                    point->heading_deg) >
                GPS_TRACK_HEADING_STABILITY_DEG ||
            point->segment_distance_m < 0.5 ||
            !isfinite(point->hacc_m) || point->hacc_m > GPS_TRACK_MAX_HACC_M) {
            return false;
        }
    }
    return true;
}

bool gps_track_find_virtual_start(const GpsTrack *track, size_t begin_index,
                                  size_t end_index, size_t *start_index)
{
    const GpsTrackPoint *loop_begin_point;
    const GpsTrackPoint *loop_end_point;
    double loop_distance_m;
    double selection_distance_m;
    size_t index;
    size_t fallback_index = 0U;
    size_t best_index = 0U;
    double fallback_error_m = HUGE_VAL;
    double best_score = -1.0;

    if (track == NULL || start_index == NULL || begin_index >= end_index ||
        end_index >= track->point_count ||
        end_index - begin_index < GPS_TRACK_STRAIGHT_WINDOW + 2U) {
        return false;
    }
    loop_begin_point = gps_track_at(track, begin_index);
    loop_end_point = gps_track_at(track, end_index);
    if (loop_begin_point == NULL || loop_end_point == NULL ||
        loop_end_point->total_distance_m <= loop_begin_point->total_distance_m) {
        return false;
    }
    loop_distance_m = loop_end_point->total_distance_m -
                      loop_begin_point->total_distance_m;
    selection_distance_m =
        loop_distance_m * GPS_TRACK_VIRTUAL_START_FRACTION;

    /* The closed-loop point is the pit-lane merge reference. Search only
     * ahead of it, in the first 15% of the lap. */
    for (index = begin_index + 1U; index < end_index; ++index) {
        const GpsTrackPoint *point = gps_track_at(track, index);
        double distance_from_loop_begin_m;
        double fallback_error;

        if (point == NULL) {
            continue;
        }
        distance_from_loop_begin_m = point->total_distance_m -
                                     loop_begin_point->total_distance_m;
        if (distance_from_loop_begin_m > selection_distance_m) {
            break;
        }
        fallback_error = fabs(distance_from_loop_begin_m -
                              selection_distance_m);
        if (fallback_error < fallback_error_m) {
            fallback_error_m = fallback_error;
            fallback_index = index;
        }
        if (index + GPS_TRACK_STRAIGHT_WINDOW > end_index) {
            continue;
        }
        {
            const GpsTrackPoint *window_last = gps_track_at(
                track, index + GPS_TRACK_STRAIGHT_WINDOW - 1U);

            if (window_last == NULL ||
                window_last->total_distance_m -
                        loop_begin_point->total_distance_m >
                    selection_distance_m ||
                !gps_track_is_straight_window(track, index, end_index)) {
                continue;
            }
        }

        {
            size_t last = index + GPS_TRACK_STRAIGHT_WINDOW - 1U;
            const GpsTrackPoint *first_point = gps_track_at(track, index);
            const GpsTrackPoint *last_point = gps_track_at(track, last);
            double path_distance_m = last_point != NULL && first_point != NULL
                                         ? last_point->total_distance_m -
                                           first_point->total_distance_m
                                         : 0.0;
            double straight_distance_m = first_point != NULL &&
                                                 last_point != NULL
                                             ? gps_track_distance(first_point,
                                                                  last_point)
                                             : 0.0;
            double score = path_distance_m > 0.1
                               ? straight_distance_m / path_distance_m
                               : 0.0;

            if (score > best_score + GPS_TRACK_STRAIGHT_SCORE_TOLERANCE) {
                best_score = score;
                best_index = index + GPS_TRACK_STRAIGHT_WINDOW / 2U;
            }
        }
    }
    if (best_score < 0.0) {
        best_index = fallback_index != 0U ? fallback_index : begin_index + 1U;
    }
    *start_index = best_index;
    return true;
}
