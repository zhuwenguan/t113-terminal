#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lap_logic.h"
#include "console_log.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LAP_LOGIC_EARTH_RADIUS_M 6371000.0
#define LAP_LOGIC_PI 3.14159265358979323846
#define LAP_LOGIC_START_DELAY_US 5000000ULL
#define LAP_LOGIC_MIN_LAP_US 10000000ULL
#define LAP_LOGIC_MAX_HEADING_ERROR_DEG 45.0
#define LAP_LOGIC_LINE_HALF_LENGTH_M 30.0
#define LAP_LOGIC_SIDE_HYSTERESIS_M 1.0

static double lap_logic_degrees_to_radians(double degrees)
{
    return degrees * LAP_LOGIC_PI / 180.0;
}

static double lap_logic_normalize_heading(double heading_deg)
{
    while (heading_deg < 0.0) {
        heading_deg += 360.0;
    }
    while (heading_deg >= 360.0) {
        heading_deg -= 360.0;
    }
    return heading_deg;
}

static double lap_logic_heading_error(double actual_deg, double target_deg)
{
    double error = fabs(lap_logic_normalize_heading(actual_deg) -
                        lap_logic_normalize_heading(target_deg));

    return error > 180.0 ? 360.0 - error : error;
}

static double lap_logic_distance_m(double latitude_a, double longitude_a,
                                   double latitude_b, double longitude_b)
{
    double latitude_a_rad = lap_logic_degrees_to_radians(latitude_a);
    double latitude_b_rad = lap_logic_degrees_to_radians(latitude_b);
    double delta_lat = latitude_b_rad - latitude_a_rad;
    double delta_lon = lap_logic_degrees_to_radians(longitude_b - longitude_a);
    double sine_lat = sin(delta_lat / 2.0);
    double sine_lon = sin(delta_lon / 2.0);
    double haversine = sine_lat * sine_lat +
                       cos(latitude_a_rad) * cos(latitude_b_rad) *
                           sine_lon * sine_lon;

    if (haversine > 1.0) {
        haversine = 1.0;
    }
    return 2.0 * LAP_LOGIC_EARTH_RADIUS_M * asin(sqrt(haversine));
}

static void lap_logic_local_coordinates(const LapLogic *logic,
                                        double latitude, double longitude,
                                        double *side_m, double *along_m)
{
    double start_latitude_rad =
        lap_logic_degrees_to_radians(logic->track.start_lat);
    double north_m = lap_logic_degrees_to_radians(latitude -
                                                  logic->track.start_lat) *
                     LAP_LOGIC_EARTH_RADIUS_M;
    double east_m = lap_logic_degrees_to_radians(longitude -
                                                 logic->track.start_lon) *
                    LAP_LOGIC_EARTH_RADIUS_M * cos(start_latitude_rad);
    double heading_rad = lap_logic_degrees_to_radians(logic->track.heading_deg);
    double forward_north = cos(heading_rad);
    double forward_east = sin(heading_rad);

    /* The start line is perpendicular to the configured travel direction. */
    *side_m = north_m * forward_north + east_m * forward_east;
    *along_m = east_m * forward_north - north_m * forward_east;
}

static bool lap_logic_json_number(const char *json, const char *key,
                                  double *value)
{
    char name[64];
    const char *location;
    char *end;
    int written;

    written = snprintf(name, sizeof(name), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(name)) {
        return false;
    }
    location = strstr(json, name);
    if (location == NULL) {
        return false;
    }
    location = strchr(location + strlen(name), ':');
    if (location == NULL) {
        return false;
    }
    errno = 0;
    *value = strtod(location + 1, &end);
    return errno == 0 && end != location + 1 && isfinite(*value);
}

static int lap_logic_load_config(const char *path, LapTrackConfig *config)
{
    char json[512];
    FILE *file;
    size_t length;
    bool read_error;

    if (path == NULL || config == NULL) {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return 1;
    }
    length = fread(json, 1U, sizeof(json) - 1U, file);
    read_error = ferror(file) != 0;
    fclose(file);
    json[length] = '\0';
    if (read_error || !lap_logic_json_number(json, "start_lat",
                                               &config->start_lat) ||
        !lap_logic_json_number(json, "start_lon", &config->start_lon) ||
        !lap_logic_json_number(json, "heading_deg", &config->heading_deg)) {
        return -1;
    }
    config->heading_deg = lap_logic_normalize_heading(config->heading_deg);
    return 0;
}

static void lap_logic_set_current_state(const LapLogic *logic,
                                        uint64_t timestamp_us,
                                        double delta_time_s,
                                        VehicleState *state)
{
    double current_lap_time_s = logic->lap_active
                                    ? (double)(timestamp_us -
                                               logic->lap_start_timestamp_us) /
                                          1000000.0
                                    : 0.0;

    state->lap_number = logic->lap_number;
    state->lap_valid = logic->lap_active;
    state->lap_time_s = current_lap_time_s;
    state->current_lap_time_s = current_lap_time_s;
    state->delta_time_s = delta_time_s;
    state->track_distance_m = logic->track_distance_m;
}

static double lap_logic_best_time_at_distance(const LapLogic *logic,
                                              double distance_m)
{
    size_t index;

    if (logic->best_trace_count == 0U) {
        return 0.0;
    }
    if (logic->best_trace_count == 1U ||
        distance_m <= logic->best_trace[0].distance_m) {
        return logic->best_trace[0].time_s;
    }
    for (index = 1U; index < logic->best_trace_count; ++index) {
        const LapTracePoint *previous = &logic->best_trace[index - 1U];
        const LapTracePoint *current = &logic->best_trace[index];

        if (distance_m <= current->distance_m) {
            double distance_delta = current->distance_m - previous->distance_m;

            if (distance_delta <= 0.001) {
                return current->time_s;
            }
            return previous->time_s +
                   (current->time_s - previous->time_s) *
                       (distance_m - previous->distance_m) / distance_delta;
        }
    }
    return logic->best_trace[logic->best_trace_count - 1U].time_s;
}

static double lap_logic_current_delta(const LapLogic *logic,
                                      double current_time_s)
{
    if (logic->best_trace_count == 0U) {
        return 0.0;
    }
    return current_time_s -
           lap_logic_best_time_at_distance(logic, logic->track_distance_m);
}

static void lap_logic_write_point(LapLogic *logic, const LapTracePoint *point)
{
    if (logic->lap_file != NULL &&
        fprintf(logic->lap_file, "%.3f,%.3f,%.8f,%.8f,%.3f\n",
                point->distance_m, point->time_s, point->latitude,
                point->longitude, point->speed_kmh) < 0) {
        LOG_WARN("[LAP] cannot write lap trace");
        fclose(logic->lap_file);
        logic->lap_file = NULL;
    }
}

static void lap_logic_append_point(LapLogic *logic, uint64_t timestamp_us,
                                   const LapProState *sample)
{
    LapTracePoint point;

    point.distance_m = logic->track_distance_m;
    point.time_s = (double)(timestamp_us - logic->lap_start_timestamp_us) /
                   1000000.0;
    point.latitude = sample->latitude;
    point.longitude = sample->longitude;
    point.speed_kmh = sample->speed_kmh;
    if (logic->current_trace_count < LAP_LOGIC_MAX_TRACE_POINTS) {
        logic->current_trace[logic->current_trace_count++] = point;
    } else if (!logic->trace_truncated) {
        LOG_WARN("[LAP] trace point limit reached (%u)",
                 LAP_LOGIC_MAX_TRACE_POINTS);
        logic->trace_truncated = true;
    }
    lap_logic_write_point(logic, &point);
}

static void lap_logic_close_trace(LapLogic *logic)
{
    if (logic->lap_file != NULL) {
        fflush(logic->lap_file);
        fclose(logic->lap_file);
        logic->lap_file = NULL;
    }
}

static void lap_logic_open_trace(LapLogic *logic)
{
    char path[LAP_LOGIC_PATH_LENGTH];
    int written;

    lap_logic_close_trace(logic);
    if (logic->trace_directory[0] == '\0') {
        return;
    }
    written = snprintf(path, sizeof(path), "%s/lap_%03u.csv",
                       logic->trace_directory, logic->lap_number);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        LOG_WARN("[LAP] lap trace path is too long");
        return;
    }
    logic->lap_file = fopen(path, "w");
    if (logic->lap_file == NULL) {
        LOG_WARN("[LAP] cannot open trace %s: %s", path, strerror(errno));
        return;
    }
    setvbuf(logic->lap_file, NULL, _IOFBF, 4096U);
    if (fprintf(logic->lap_file,
                "distance_m,time_s,latitude,longitude,speed_kmh\n") < 0) {
        lap_logic_close_trace(logic);
    }
}

static void lap_logic_start_lap(LapLogic *logic, uint64_t timestamp_us,
                                const LapProState *sample, VehicleState *state)
{
    logic->lap_active = true;
    logic->lap_number = 1U;
    logic->lap_start_timestamp_us = timestamp_us;
    logic->track_distance_m = 0.0;
    logic->current_trace_count = 0U;
    logic->trace_truncated = false;
    lap_logic_open_trace(logic);
    lap_logic_append_point(logic, timestamp_us, sample);
    lap_logic_set_current_state(logic, timestamp_us, 0.0, state);
}

static void lap_logic_finish_lap(LapLogic *logic, uint64_t timestamp_us,
                                 const LapProState *sample, VehicleState *state)
{
    double lap_time_s = (double)(timestamp_us -
                                 logic->lap_start_timestamp_us) /
                        1000000.0;
    double delta_time_s = lap_logic_current_delta(logic, lap_time_s);
    bool new_best = state->best_lap_time_s <= 0.0 ||
                    lap_time_s < state->best_lap_time_s;
    uint32_t completed_lap = logic->lap_number;

    state->last_lap_time_s = lap_time_s;
    if (state->best_lap_time_s <= 0.0 || lap_time_s < state->best_lap_time_s) {
        state->best_lap_time_s = lap_time_s;
        new_best = true;
    }
    if (new_best) {
        memcpy(logic->best_trace, logic->current_trace,
               logic->current_trace_count * sizeof(logic->current_trace[0]));
        logic->best_trace_count = logic->current_trace_count;
    }
    LOG_INFO("[LAP] lap=%u time=%.2fs best=%.2fs delta=%+.2fs",
             completed_lap, lap_time_s, state->best_lap_time_s, delta_time_s);
    lap_logic_close_trace(logic);

    logic->lap_number++;
    logic->lap_start_timestamp_us = timestamp_us;
    logic->track_distance_m = 0.0;
    logic->current_trace_count = 0U;
    logic->trace_truncated = false;
    lap_logic_open_trace(logic);
    lap_logic_append_point(logic, timestamp_us, sample);
    lap_logic_set_current_state(logic, timestamp_us, 0.0, state);
}

static bool lap_logic_crossed_start_line(const LapLogic *logic,
                                         const LapProState *sample,
                                         uint64_t timestamp_us,
                                         double side_m, double along_m)
{
    bool crossed;

    if (!logic->have_previous_position ||
        lap_logic_heading_error(sample->heading_deg,
                                logic->track.heading_deg) >
            LAP_LOGIC_MAX_HEADING_ERROR_DEG ||
        fabs(along_m) > LAP_LOGIC_LINE_HALF_LENGTH_M ||
        fabs(logic->previous_along_m) > LAP_LOGIC_LINE_HALF_LENGTH_M) {
        return false;
    }
    crossed = (logic->previous_side_m <= -LAP_LOGIC_SIDE_HYSTERESIS_M &&
               side_m >= LAP_LOGIC_SIDE_HYSTERESIS_M) ||
              (logic->previous_side_m >= LAP_LOGIC_SIDE_HYSTERESIS_M &&
               side_m <= -LAP_LOGIC_SIDE_HYSTERESIS_M);
    if (!crossed || timestamp_us - logic->first_valid_timestamp_us <
                        LAP_LOGIC_START_DELAY_US) {
        return false;
    }
    if (logic->lap_active &&
        timestamp_us - logic->lap_start_timestamp_us < LAP_LOGIC_MIN_LAP_US) {
        return false;
    }
    return true;
}

int lap_logic_init(LapLogic *logic, const char *track_config_path,
                   const char *trace_directory)
{
    int result;

    if (logic == NULL || track_config_path == NULL) {
        return -1;
    }
    memset(logic, 0, sizeof(*logic));
    result = lap_logic_load_config(track_config_path, &logic->track);
    if (result == 1) {
        LOG_WARN("[LAP] track config not found: %s", track_config_path);
        return 1;
    }
    if (result < 0) {
        LOG_WARN("[LAP] invalid track config: %s", track_config_path);
        return 1;
    }
    if (trace_directory != NULL) {
        snprintf(logic->trace_directory, sizeof(logic->trace_directory), "%s",
                 trace_directory);
    }
    logic->enabled = true;
    LOG_INFO("[LAP] track start=(%.7f,%.7f) heading=%.1f",
             logic->track.start_lat, logic->track.start_lon,
             logic->track.heading_deg);
    return 0;
}

void lap_logic_update(LapLogic *logic, uint64_t timestamp_us,
                      const LapProState *sample, VehicleState *state)
{
    double side_m;
    double along_m;
    bool crossed;

    if (logic == NULL || !logic->enabled || sample == NULL || state == NULL ||
        sample->fix_type < 3U ||
        (logic->last_input_timestamp_us != 0U &&
         timestamp_us < logic->last_input_timestamp_us)) {
        return;
    }
    logic->last_input_timestamp_us = timestamp_us;
    lap_logic_local_coordinates(logic, sample->latitude, sample->longitude,
                                &side_m, &along_m);
    if (logic->first_valid_timestamp_us == 0U) {
        logic->first_valid_timestamp_us = timestamp_us;
    }

    if (logic->lap_active && logic->have_previous_position) {
        logic->track_distance_m += lap_logic_distance_m(
            logic->previous_latitude, logic->previous_longitude,
            sample->latitude, sample->longitude);
    }
    crossed = lap_logic_crossed_start_line(logic, sample, timestamp_us, side_m,
                                           along_m);

    if (logic->lap_active) {
        double current_time_s = (double)(timestamp_us -
                                          logic->lap_start_timestamp_us) /
                                1000000.0;
        double delta_time_s;

        lap_logic_append_point(logic, timestamp_us, sample);
        delta_time_s = lap_logic_current_delta(logic, current_time_s);
        lap_logic_set_current_state(logic, timestamp_us, delta_time_s, state);
        if (crossed) {
            lap_logic_finish_lap(logic, timestamp_us, sample, state);
        }
        if (logic->last_report_timestamp_us == 0U ||
            timestamp_us - logic->last_report_timestamp_us >= 1000000ULL) {
            LOG_INFO("[LAP] lap=%u distance=%.1fm delta=%+.2fs",
                     state->lap_number, state->track_distance_m,
                     state->delta_time_s);
            logic->last_report_timestamp_us = timestamp_us;
        }
    } else if (crossed) {
        lap_logic_start_lap(logic, timestamp_us, sample, state);
        logic->last_report_timestamp_us = timestamp_us;
    }

    logic->previous_latitude = sample->latitude;
    logic->previous_longitude = sample->longitude;
    logic->previous_side_m = side_m;
    logic->previous_along_m = along_m;
    logic->have_previous_position = true;
}

void lap_logic_close(LapLogic *logic)
{
    if (logic == NULL) {
        return;
    }
    lap_logic_close_trace(logic);
    logic->enabled = false;
}
