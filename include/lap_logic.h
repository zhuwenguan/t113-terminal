#ifndef T113_LAP_LOGIC_H
#define T113_LAP_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lappro.h"
#include "vehicle_state.h"

#define LAP_LOGIC_MAX_TRACE_POINTS 1024U
#define LAP_LOGIC_PATH_LENGTH 256U

typedef struct {
    double start_lat;
    double start_lon;
    double heading_deg;
} LapTrackConfig;

typedef struct {
    double distance_m;
    double time_s;
    double latitude;
    double longitude;
    double speed_kmh;
} LapTracePoint;

typedef struct {
    bool enabled;
    LapTrackConfig track;
    char trace_directory[LAP_LOGIC_PATH_LENGTH];
    FILE *lap_file;
    LapTracePoint current_trace[LAP_LOGIC_MAX_TRACE_POINTS];
    LapTracePoint best_trace[LAP_LOGIC_MAX_TRACE_POINTS];
    size_t current_trace_count;
    size_t best_trace_count;
    uint64_t first_valid_timestamp_us;
    uint64_t lap_start_timestamp_us;
    uint64_t last_report_timestamp_us;
    uint64_t last_input_timestamp_us;
    uint32_t lap_number;
    double track_distance_m;
    double previous_latitude;
    double previous_longitude;
    double previous_side_m;
    double previous_along_m;
    bool have_previous_position;
    bool lap_active;
    bool trace_truncated;
} LapLogic;

/* Returns 0 when enabled, 1 when the optional config is unavailable. */
int lap_logic_init(LapLogic *logic, const char *track_config_path,
                   const char *trace_directory);

void lap_logic_update(LapLogic *logic, uint64_t timestamp_us,
                      const LapProState *sample, VehicleState *state);

void lap_logic_close(LapLogic *logic);

#endif
