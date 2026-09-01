#define _POSIX_C_SOURCE 200809L

#include "webd_json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buffer;
    size_t size;
    size_t length;
    bool failed;
} JsonWriter;

static void json_append(JsonWriter *writer, const char *format, ...)
{
    va_list arguments;
    int written;

    if (writer == NULL || writer->failed || format == NULL ||
        writer->length >= writer->size) {
        if (writer != NULL) {
            writer->failed = true;
        }
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(writer->buffer + writer->length,
                        writer->size - writer->length, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= writer->size - writer->length) {
        writer->failed = true;
        return;
    }
    writer->length += (size_t)written;
}

static double json_number(double value)
{
    return isfinite(value) ? value : 0.0;
}

static void json_finish(JsonWriter *writer)
{
    if (writer != NULL && !writer->failed && writer->length < writer->size) {
        writer->buffer[writer->length] = '\0';
    }
}

int webd_json_state(char *buffer, size_t buffer_size,
                    const StateSnapshot *snapshot)
{
    JsonWriter writer;
    const VehicleState *state;

    if (buffer == NULL || buffer_size == 0U || snapshot == NULL ||
        !snapshot->available) {
        return -1;
    }
    writer = (JsonWriter){buffer, buffer_size, 0U, false};
    state = &snapshot->state;
    json_append(&writer,
                "{\"valid\":%s,\"source_sequence\":%u,"
                "\"timestamp_ms\":%llu,\"timestamp_us\":%llu,"
                "\"valid_mask\":%u,\"can_valid\":%s,\"gps_valid\":%s,"
                "\"speed_kph\":%.6f,\"heading_deg\":%.6f,"
                "\"latitude_deg\":%.9f,\"longitude_deg\":%.9f,"
                "\"acc_x_mps2\":%.6f,\"acc_y_mps2\":%.6f,"
                "\"acc_z_mps2\":%.6f,\"motor_rpm\":%.3f,"
                "\"torque_nm\":%.6f,\"motor_temperature_c\":%.6f,"
                "\"controller_temperature_c\":%.6f,"
                "\"battery_temperature_c\":%.6f,"
                "\"hv_voltage_v\":%.6f,\"hv_current_a\":%.6f,"
                "\"hv_power_w\":%.6f,\"hv_power_kw\":%.6f,"
                "\"hv_soc_percent\":%.6f,\"hv_discharge_ah\":%.6f,"
                "\"hv_software_discharge_ah\":%.6f,"
                "\"lv_voltage_v\":%.6f,\"throttle_percent\":%.6f,"
                "\"brake_percent\":%.6f,\"mode\":%u,"
                "\"drive_state\":\"%s\",\"event_mode\":\"%s\","
                "\"lap_number\":%u,\"lap_valid\":%s,"
                "\"lap_time_s\":%.6f,\"last_lap_time_s\":%.6f,"
                "\"best_lap_time_s\":%.6f,\"delta_s\":%.6f,"
                "\"track_distance_m\":%.6f,\"sector_number\":%u,"
                "\"accel_distance_m\":%.6f,\"accel_50m_time_s\":%.6f,"
                "\"accel_time_s\":%.6f,\"max_acceleration_mps2\":%.6f}",
                snapshot->fresh ? "true" : "false", snapshot->sequence,
                (unsigned long long)(state->timestamp_us / 1000ULL),
                (unsigned long long)state->timestamp_us, state->valid_mask,
                state->can_valid ? "true" : "false",
                state->gps_valid ? "true" : "false",
                json_number(state->speed_kmh), json_number(state->heading_deg),
                json_number(state->latitude), json_number(state->longitude),
                json_number(state->acc_x), json_number(state->acc_y),
                json_number(state->acc_z), json_number(state->motor_rpm),
                json_number(state->torque_nm),
                json_number(state->motor_temperature_c),
                json_number(state->controller_temperature_c),
                json_number(state->battery_temperature_c),
                json_number(state->hv_voltage_v), json_number(state->hv_current_a),
                json_number(state->hv_power_w), json_number(state->hv_power_w / 1000.0),
                json_number(state->hv_soc_percent), json_number(state->hv_discharge_ah),
                json_number(state->hv_software_discharge_ah),
                json_number(state->lv_voltage_v), json_number(state->throttle_percent),
                json_number(state->brake_percent), state->mode,
                vehicle_drive_state_name(state->drive_state),
                vehicle_state_event_mode_name(state->event_mode), state->lap_number,
                state->lap_valid ? "true" : "false", json_number(state->lap_time_s),
                json_number(state->last_lap_time_s),
                json_number(state->best_lap_time_s), json_number(state->delta_time_s),
                json_number(state->track_distance_m), state->sector_number,
                json_number(state->accel_distance_m),
                json_number(state->accel_50m_time_s), json_number(state->accel_time_s),
                json_number(state->max_acceleration));
    json_finish(&writer);
    return writer.failed ? -1 : (int)writer.length;
}

int webd_json_state_error(char *buffer, size_t buffer_size,
                          const char *error_message)
{
    int written;

    if (buffer == NULL || buffer_size == 0U || error_message == NULL) {
        return -1;
    }
    written = snprintf(buffer, buffer_size,
                       "{\"valid\":false,\"error\":\"%s\"}",
                       error_message);
    return written < 0 || (size_t)written >= buffer_size ? -1 : written;
}

int webd_json_system(char *buffer, size_t buffer_size,
                     uint64_t uptime_s, const StateSnapshot *snapshot,
                     unsigned int websocket_clients,
                     uint64_t now_mono_us)
{
    bool alive = snapshot != NULL && snapshot->available && snapshot->fresh;
    bool has_age = snapshot != NULL && snapshot->available &&
                   snapshot->published_mono_us != 0U &&
                   now_mono_us >= snapshot->published_mono_us;
    uint64_t age_ms = has_age ?
        (now_mono_us - snapshot->published_mono_us) / 1000ULL : 0U;
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return -1;
    }
    if (has_age) {
        written = snprintf(
            buffer, buffer_size,
            "{\"uptime_s\":%llu,\"vehicle_state_alive\":%s,"
            "\"vehicle_state_age_ms\":%llu,\"vehicle_state_sequence\":%u,"
            "\"websocket_clients\":%u}",
            (unsigned long long)uptime_s, alive ? "true" : "false",
            (unsigned long long)age_ms,
            snapshot->sequence, websocket_clients);
    } else {
        written = snprintf(
            buffer, buffer_size,
            "{\"uptime_s\":%llu,\"vehicle_state_alive\":%s,"
            "\"vehicle_state_age_ms\":null,\"vehicle_state_sequence\":0,"
            "\"websocket_clients\":%u}",
            (unsigned long long)uptime_s, alive ? "true" : "false",
            websocket_clients);
    }
    return written < 0 || (size_t)written >= buffer_size ? -1 : written;
}

int webd_json_capabilities(char *buffer, size_t buffer_size)
{
    static const char capabilities[] =
        "{\"mode\":\"local\",\"telemetry\":true,\"video\":false,"
        "\"logs\":false,\"config\":false,\"remote_video\":false}";
    size_t length = sizeof(capabilities) - 1U;

    if (buffer == NULL || buffer_size <= length) {
        return -1;
    }
    memcpy(buffer, capabilities, sizeof(capabilities));
    return (int)length;
}
