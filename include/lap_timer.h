#ifndef T113_LAP_TIMER_H
#define T113_LAP_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gps_track.h"
#include "vehicle_state.h"

typedef struct {
    uint64_t timestamp_us;
    double x_m;
    double y_m;
    double distance_s_m;
    double time_s;
    double speed_kmh;
    double heading_deg;
} LapReferencePoint;

typedef struct {
    bool configured;
    bool have_projection;
    bool waiting_for_initial_crossing;
    bool previous_projection_valid;
    char trace_directory[256];
    FILE *trace_file;
    LapReferencePoint reference[GPS_TRACK_MAX_POINTS];
    size_t reference_count;
    size_t previous_projection_index;
    double reference_lap_time_s;
    double reference_loop_distance_s_m;
    double current_distance_s_m;
    double current_time_s;
    double delta_time_s;
    double estimated_lap_time_s;
    uint64_t previous_projection_timestamp_us;
    uint64_t lap_start_timestamp_us;
    uint64_t last_report_timestamp_us;
    uint32_t lap_number;
    double best_lap_time_s;
    double last_lap_time_s;
} LapTimer;

void lap_timer_init(LapTimer *timer, const char *trace_directory);

bool lap_timer_configure_from_loop(LapTimer *timer, const GpsTrack *track,
                                   size_t loop_begin, size_t loop_end,
                                   size_t virtual_start,
                                   uint64_t initial_lap_start_timestamp_us,
                                   const GpsTrackPoint *current_point,
                                   VehicleState *state);

void lap_timer_update(LapTimer *timer, const GpsTrackPoint *point,
                      VehicleState *state);

void lap_timer_close(LapTimer *timer);

#endif
