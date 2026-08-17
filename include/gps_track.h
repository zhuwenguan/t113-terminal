#ifndef T113_GPS_TRACK_H
#define T113_GPS_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_TRACK_MAX_POINTS 2048U

typedef struct {
    uint64_t timestamp_us;
    double x_m;
    double y_m;
    double segment_distance_m;
    double total_distance_m;
    double speed_kmh;
    double heading_deg;
    double hacc_m;
} GpsTrackPoint;

typedef struct {
    bool initialized;
    double origin_latitude;
    double origin_longitude;
    double previous_latitude;
    double previous_longitude;
    double total_distance_m;
    GpsTrackPoint points[GPS_TRACK_MAX_POINTS];
    size_t point_count;
} GpsTrack;

void gps_track_init(GpsTrack *track);

bool gps_track_append(GpsTrack *track, uint64_t timestamp_us,
                      double latitude, double longitude, double speed_kmh,
                      double heading_deg, double hacc_m,
                      GpsTrackPoint *current_point);

const GpsTrackPoint *gps_track_at(const GpsTrack *track, size_t index);

double gps_track_distance(const GpsTrackPoint *a, const GpsTrackPoint *b);

/* Finds a stable, low-curvature point inside a detected loop. */
bool gps_track_find_virtual_start(const GpsTrack *track, size_t begin_index,
                                  size_t end_index, size_t *start_index);

#endif
