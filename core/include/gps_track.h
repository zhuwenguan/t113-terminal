#ifndef T113_GPS_TRACK_H
#define T113_GPS_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* At the circuit detector's 5 Hz storage rate this covers more than the
 * advertised 900-second maximum loop without unbounded allocation. */
#define GPS_TRACK_MAX_POINTS 4608U

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

/* Projects every input sample but stores at most one point per interval.
 * total_distance_m continues to accumulate from every raw sample. */
bool gps_track_sample(GpsTrack *track, uint64_t timestamp_us,
                      double latitude, double longitude, double speed_kmh,
                      double heading_deg, double hacc_m,
                      uint64_t minimum_store_interval_us,
                      GpsTrackPoint *current_point, bool *stored);

const GpsTrackPoint *gps_track_at(const GpsTrack *track, size_t index);

double gps_track_distance(const GpsTrackPoint *a, const GpsTrackPoint *b);

/* Finds a stable, low-curvature point inside a detected loop. */
bool gps_track_find_virtual_start(const GpsTrack *track, size_t begin_index,
                                  size_t end_index, size_t *start_index);

#endif
