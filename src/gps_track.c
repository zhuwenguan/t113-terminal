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

bool gps_track_append(GpsTrack *track, uint64_t timestamp_us,
                      double latitude, double longitude, double speed_kmh,
                      double heading_deg, double hacc_m,
                      GpsTrackPoint *current_point)
{
    GpsTrackPoint point;
    double latitude_scale;
    double delta_latitude;
    double delta_longitude;

    if (track == NULL || current_point == NULL) {
        return false;
    }
    if (track->point_count >= GPS_TRACK_MAX_POINTS) {
        /* Keep the most recent half without allocating memory. */
        memmove(track->points, track->points + GPS_TRACK_MAX_POINTS / 2U,
                (GPS_TRACK_MAX_POINTS / 2U) * sizeof(track->points[0]));
        track->point_count = GPS_TRACK_MAX_POINTS / 2U;
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
    if (track->point_count > 0U) {
        const GpsTrackPoint *previous =
            &track->points[track->point_count - 1U];

        point.segment_distance_m = gps_track_distance(&point, previous);
        track->total_distance_m += point.segment_distance_m;
    }
    point.total_distance_m = track->total_distance_m;
    track->points[track->point_count++] = point;
    track->previous_latitude = latitude;
    track->previous_longitude = longitude;
    *current_point = point;
    return true;
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
    size_t index;
    size_t best_index = 0U;
    double best_score = -1.0;

    if (track == NULL || start_index == NULL || begin_index >= end_index ||
        end_index > track->point_count ||
        end_index - begin_index < GPS_TRACK_STRAIGHT_WINDOW + 2U) {
        return false;
    }
    for (index = begin_index + 1U;
         index + GPS_TRACK_STRAIGHT_WINDOW < end_index - 1U; ++index) {
        if (gps_track_is_straight_window(track, index, end_index)) {
            size_t last = index + GPS_TRACK_STRAIGHT_WINDOW - 1U;
            const GpsTrackPoint *first_point = gps_track_at(track, index);
            const GpsTrackPoint *last_point = gps_track_at(track, last);
            double score = first_point != NULL && last_point != NULL
                               ? gps_track_distance(first_point, last_point)
                               : 0.0;

            if (score > best_score) {
                best_score = score;
                best_index = index + GPS_TRACK_STRAIGHT_WINDOW / 2U;
            }
        }
    }
    if (best_score < 0.0) {
        best_index = begin_index + (end_index - begin_index) / 4U;
    }
    *start_index = best_index;
    return true;
}
