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

static void json_append_bool_array(JsonWriter *writer, const bool *values,
                                   size_t count)
{
    size_t index;

    json_append(writer, "[");
    for (index = 0U; index < count; ++index) {
        json_append(writer, "%s%s", index == 0U ? "" : ",",
                    values[index] ? "true" : "false");
    }
    json_append(writer, "]");
}

static void json_append_float_array(JsonWriter *writer, const float *values,
                                    const bool *valid, size_t count,
                                    bool snapshot_live)
{
    size_t index;

    json_append(writer, "[");
    for (index = 0U; index < count; ++index) {
        bool value_valid = snapshot_live && valid[index];

        if (index != 0U) {
            json_append(writer, ",");
        }
        if (value_valid) {
            json_append(writer, "%.4f", (double)values[index]);
        } else {
            json_append(writer, "null");
        }
    }
    json_append(writer, "]");
}

static void json_append_valid_array(JsonWriter *writer, const bool *values,
                                    size_t count, bool snapshot_live)
{
    size_t index;

    json_append(writer, "[");
    for (index = 0U; index < count; ++index) {
        if (index != 0U) {
            json_append(writer, ",");
        }
        json_append(writer, "%s", snapshot_live && values[index] ?
                    "true" : "false");
    }
    json_append(writer, "]");
}

static bool json_fault_is_cell(BmsFaultType type)
{
    return type == BMS_FAULT_CELL_VOLTAGE_OPEN;
}

static bool json_fault_is_board_level(BmsFaultType type)
{
    return type == BMS_FAULT_LOW_NEGATIVE_VOLTAGE_OPEN ||
           type == BMS_FAULT_LOW_POSITIVE_VOLTAGE ||
           type == BMS_FAULT_HIGH_NEGATIVE_VOLTAGE_OPEN ||
           type == BMS_FAULT_HIGH_POSITIVE_VOLTAGE ||
           type == BMS_FAULT_LTC6804_LOW_COMM ||
           type == BMS_FAULT_LTC6804_HIGH_COMM;
}

static void json_append_float_or_null(JsonWriter *writer, float value,
                                      bool valid)
{
    if (valid) {
        json_append(writer, "%.4f", (double)value);
    } else {
        json_append(writer, "null");
    }
}

static unsigned int bms_board_fault_count(const BmsBoardState *board)
{
    unsigned int count = 0U;
    unsigned int index;

    if (board == NULL) {
        return 0U;
    }
    for (index = 0U; index < BMS_CELLS_PER_BOARD; ++index) {
        count += board->voltage_open[index] ? 1U : 0U;
        count += board->balance_stuck_open[index] ? 1U : 0U;
        count += board->balance_stuck_closed[index] ? 1U : 0U;
    }
    for (index = 0U; index < BMS_FAULT_TEMP_CHANNELS; ++index) {
        count += board->temp_open[index] ? 1U : 0U;
        count += board->temp_short[index] ? 1U : 0U;
    }
    count += board->low_negative_voltage_open ? 1U : 0U;
    count += board->low_positive_voltage_fault ? 1U : 0U;
    count += board->high_negative_voltage_open ? 1U : 0U;
    count += board->high_positive_voltage_fault ? 1U : 0U;
    count += board->ltc6804_low_comm_fault ? 1U : 0U;
    count += board->ltc6804_high_comm_fault ? 1U : 0U;
    return count;
}

static void json_append_board_faults(JsonWriter *writer,
                                     const BmsBoardState *board)
{
    json_append(writer, "\"faults\":{\"voltage_open\":");
    json_append_bool_array(writer, board->voltage_open,
                           BMS_CELLS_PER_BOARD);
    json_append(writer, ",\"temp_open\":");
    json_append_bool_array(writer, board->temp_open, BMS_FAULT_TEMP_CHANNELS);
    json_append(writer, ",\"temp_short\":");
    json_append_bool_array(writer, board->temp_short,
                           BMS_FAULT_TEMP_CHANNELS);
    json_append(writer, ",\"balance_stuck_open\":");
    json_append_bool_array(writer, board->balance_stuck_open,
                           BMS_CELLS_PER_BOARD);
    json_append(writer, ",\"balance_stuck_closed\":");
    json_append_bool_array(writer, board->balance_stuck_closed,
                           BMS_CELLS_PER_BOARD);
    json_append(writer,
                ",\"low_negative_voltage_open\":%s"
                ",\"low_positive_voltage_fault\":%s"
                ",\"high_negative_voltage_open\":%s"
                ",\"high_positive_voltage_fault\":%s"
                ",\"ltc6804_low_comm_fault\":%s"
                ",\"ltc6804_high_comm_fault\":%s"
                ",\"fault_30_raw\":\"0x%016llx\""
                ",\"fault_31_raw\":\"0x%016llx\"}",
                board->low_negative_voltage_open ? "true" : "false",
                board->low_positive_voltage_fault ? "true" : "false",
                board->high_negative_voltage_open ? "true" : "false",
                board->high_positive_voltage_fault ? "true" : "false",
                board->ltc6804_low_comm_fault ? "true" : "false",
                board->ltc6804_high_comm_fault ? "true" : "false",
                (unsigned long long)board->fault_30_raw,
                (unsigned long long)board->fault_31_raw);
}

static void json_append_bms_event(JsonWriter *writer,
                                  const BmsFaultEvent *event)
{
    bool board_level;

    if (event == NULL) {
        return;
    }
    board_level = json_fault_is_board_level(event->type);
    if (board_level) {
        json_append(writer,
                    "{\"timestamp_us\":%llu,\"board\":%u,"
                    "\"board_number\":%u,\"channel\":null,"
                    "\"global_cell\":null,\"type\":\"%s\","
                    "\"state\":\"%s\"}",
                    (unsigned long long)event->timestamp_mono_us,
                    (unsigned int)event->board,
                    (unsigned int)event->board + 1U,
                    bms_fault_type_name(event->type),
                    event->active ? "FAULT" : "RECOVERED");
    } else if (json_fault_is_cell(event->type)) {
        json_append(writer,
                    "{\"timestamp_us\":%llu,\"board\":%u,"
                    "\"board_number\":%u,\"channel\":%u,"
                    "\"global_cell\":%u,\"type\":\"%s\","
                    "\"state\":\"%s\"}",
                    (unsigned long long)event->timestamp_mono_us,
                    (unsigned int)event->board,
                    (unsigned int)event->board + 1U,
                    (unsigned int)event->channel + 1U,
                    (unsigned int)event->board * BMS_CELLS_PER_BOARD +
                        (unsigned int)event->channel + 1U,
                    bms_fault_type_name(event->type),
                    event->active ? "FAULT" : "RECOVERED");
    } else {
        json_append(writer,
                    "{\"timestamp_us\":%llu,\"board\":%u,"
                    "\"board_number\":%u,\"channel\":%u,"
                    "\"global_cell\":null,\"type\":\"%s\","
                    "\"state\":\"%s\"}",
                    (unsigned long long)event->timestamp_mono_us,
                    (unsigned int)event->board,
                    (unsigned int)event->board + 1U,
                    (unsigned int)event->channel + 1U,
                    bms_fault_type_name(event->type),
                    event->active ? "FAULT" : "RECOVERED");
    }
}

int webd_json_bms(char *buffer, size_t buffer_size,
                  const BmsStateSnapshot *snapshot)
{
    JsonWriter writer;
    const BmsState *state;
    bool snapshot_live;
    const char *status;
    unsigned int board_index;
    unsigned int event_index;

    if (buffer == NULL || buffer_size == 0U || snapshot == NULL ||
        !snapshot->available) {
        return -1;
    }
    state = &snapshot->state;
    snapshot_live = snapshot->fresh;
    status = !snapshot_live ? "STALE" :
             state->valid ? "LIVE" : "WAITING";
    writer = (JsonWriter){buffer, buffer_size, 0U, false};
    json_append(&writer,
                "{\"valid\":%s,\"status\":\"%s\","
                "\"timestamp_us\":%llu,\"source_sequence\":%u,"
                "\"age_ms\":%llu,\"meter\":{\"valid\":%s,"
                "\"ui_valid\":%s,\"ap_valid\":%s,\"hv1_v\":",
                snapshot_live && state->valid ? "true" : "false", status,
                (unsigned long long)state->timestamp_mono_us,
                snapshot->sequence,
                (unsigned long long)(snapshot->age_us / 1000U),
                snapshot_live && state->meter_valid ? "true" : "false",
                snapshot_live && state->meter_ui_valid ? "true" : "false",
                snapshot_live && state->meter_ap_valid ? "true" : "false");
    json_append_float_or_null(&writer, state->hv1_v,
                             snapshot_live && state->meter_ui_valid);
    json_append(&writer, ",\"hv2_v\":");
    json_append_float_or_null(&writer, state->hv2_v,
                             snapshot_live && state->meter_ui_valid);
    json_append(&writer, ",\"hv3_v\":");
    json_append_float_or_null(&writer, state->hv3_v,
                             snapshot_live && state->meter_ui_valid);
    json_append(&writer, ",\"hv4_v\":");
    json_append_float_or_null(&writer, state->hv4_v,
                             snapshot_live && state->meter_ap_valid);
    json_append(&writer, ",\"hv5_v\":");
    json_append_float_or_null(&writer, state->hv5_v,
                             snapshot_live && state->meter_ap_valid);
    json_append(&writer, ",\"current_a\":");
    json_append_float_or_null(&writer, state->current_a,
                             snapshot_live && state->meter_ui_valid);
    json_append(&writer, "},\"boards\":[");
    for (board_index = 0U; board_index < BMS_BOARD_COUNT; ++board_index) {
        const BmsBoardState *board = &state->board[board_index];
        bool board_live = snapshot_live && board->valid;
        const char *board_status = board_live ? "LIVE" :
                                    (snapshot_live ? "OFFLINE" : "STALE");
        uint64_t board_age_us = 0U;

        if (state->timestamp_mono_us >= board->last_update_mono_us &&
            board->last_update_mono_us != 0U) {
            board_age_us = state->timestamp_mono_us -
                           board->last_update_mono_us;
        }
        if (board_index != 0U) {
            json_append(&writer, ",");
        }
        json_append(&writer,
                    "{\"id\":%u,\"board_number\":%u,"
                    "\"valid\":%s,\"status\":\"%s\","
                    "\"age_ms\":%llu,\"cells_v\":",
                    board_index, board_index + 1U, board_live ? "true" :
                    "false", board_status,
                    (unsigned long long)(board_age_us / 1000U));
        json_append_float_array(&writer, board->cell_voltage_v,
                                board->cell_valid, BMS_CELLS_PER_BOARD,
                                snapshot_live);
        json_append(&writer, ",\"cell_valid\":");
        json_append_valid_array(&writer, board->cell_valid,
                                BMS_CELLS_PER_BOARD, snapshot_live);
        json_append(&writer, ",\"temps_c\":");
        json_append_float_array(&writer, board->temperature_c,
                                board->temperature_valid,
                                BMS_TEMPS_PER_BOARD, snapshot_live);
        json_append(&writer, ",\"temp_valid\":");
        json_append_valid_array(&writer, board->temperature_valid,
                                BMS_TEMPS_PER_BOARD, snapshot_live);
        json_append(&writer, ",\"faults_valid\":%s,\"fault_count\":%u,",
                    snapshot_live && board->fault_valid ? "true" : "false",
                    bms_board_fault_count(board));
        json_append_board_faults(&writer, board);
        json_append(&writer, "}");
    }
    json_append(&writer, "],\"fault_events\":[");
    for (event_index = 0U; event_index < state->fault_event_count;
         ++event_index) {
        unsigned int position =
            (state->fault_event_head + BMS_FAULT_EVENT_CAPACITY - 1U -
             event_index) % BMS_FAULT_EVENT_CAPACITY;

        if (event_index != 0U) {
            json_append(&writer, ",");
        }
        json_append_bms_event(&writer, &state->fault_events[position]);
    }
    json_append(&writer, "]}");
    json_finish(&writer);
    return writer.failed ? -1 : (int)writer.length;
}

int webd_json_bms_error(char *buffer, size_t buffer_size,
                        const char *error_message)
{
    int written;

    if (buffer == NULL || buffer_size == 0U || error_message == NULL) {
        return -1;
    }
    written = snprintf(buffer, buffer_size,
                       "{\"valid\":false,\"status\":\"OFFLINE\","
                       "\"error\":\"%s\"}", error_message);
    return written < 0 || (size_t)written >= buffer_size ? -1 : written;
}

int webd_json_system(char *buffer, size_t buffer_size,
                     uint64_t uptime_s, const StateSnapshot *snapshot,
                     const WebdVideoSnapshot *video,
                     unsigned int websocket_clients, unsigned int video_clients,
                     uint64_t now_mono_us)
{
    bool alive = snapshot != NULL && snapshot->available && snapshot->fresh;
    bool has_age = snapshot != NULL && snapshot->available &&
                   snapshot->published_mono_us != 0U &&
                   now_mono_us >= snapshot->published_mono_us;
    uint64_t age_ms = has_age ?
        (now_mono_us - snapshot->published_mono_us) / 1000ULL : 0U;
    bool video_has_frame = video != NULL && video->sequence != 0U;
    bool video_live = video != NULL && video->available && video->fresh;
    const char *video_status = video_live ? "live" :
                               (video_has_frame ? "stale" : "offline");
    char video_age[32];
    const char *video_age_text = "null";
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return -1;
    }
    if (video != NULL && video_has_frame) {
        (void)snprintf(video_age, sizeof(video_age), "%llu",
                       (unsigned long long)(video->frame_age_us / 1000ULL));
        video_age_text = video_age;
    }
    if (has_age) {
        written = snprintf(
            buffer, buffer_size,
            "{\"uptime_s\":%llu,\"vehicle_state_alive\":%s,"
            "\"vehicle_state_age_ms\":%llu,\"vehicle_state_sequence\":%u,"
            "\"websocket_clients\":%u,\"video_clients\":%u,"
            "\"video\":{\"available\":%s,\"status\":\"%s\","
            "\"width\":%u,\"height\":%u,\"source_fps\":%.3f,"
            "\"web_publish_fps\":%.3f,\"frame_age_ms\":%s,"
            "\"jpeg_size_bytes\":%u,\"dropped_by_rate_limit\":%llu}}",
            (unsigned long long)uptime_s, alive ? "true" : "false",
            (unsigned long long)age_ms,
            snapshot->sequence, websocket_clients, video_clients,
            video_live ? "true" : "false", video_status,
            video != NULL ? video->width : 0U,
            video != NULL ? video->height : 0U,
            video != NULL ? json_number(video->source_fps) : 0.0,
            video != NULL ? json_number(video->web_publish_fps) : 0.0,
            video_age_text,
            video != NULL ? video->jpeg_size : 0U,
            (unsigned long long)(video != NULL ?
                                 video->dropped_by_rate_limit : 0U));
    } else {
        written = snprintf(
            buffer, buffer_size,
            "{\"uptime_s\":%llu,\"vehicle_state_alive\":%s,"
            "\"vehicle_state_age_ms\":null,\"vehicle_state_sequence\":0,"
            "\"websocket_clients\":%u,\"video_clients\":%u,"
            "\"video\":{\"available\":%s,\"status\":\"%s\","
            "\"width\":%u,\"height\":%u,\"source_fps\":%.3f,"
            "\"web_publish_fps\":%.3f,\"frame_age_ms\":%s,"
            "\"jpeg_size_bytes\":%u,\"dropped_by_rate_limit\":%llu}}",
            (unsigned long long)uptime_s, alive ? "true" : "false",
            websocket_clients, video_clients,
            video_live ? "true" : "false", video_status,
            video != NULL ? video->width : 0U,
            video != NULL ? video->height : 0U,
            video != NULL ? json_number(video->source_fps) : 0.0,
            video != NULL ? json_number(video->web_publish_fps) : 0.0,
            video_age_text, video != NULL ? video->jpeg_size : 0U,
            (unsigned long long)(video != NULL ?
                                 video->dropped_by_rate_limit : 0U));
    }
    return written < 0 || (size_t)written >= buffer_size ? -1 : written;
}

int webd_json_capabilities(char *buffer, size_t buffer_size)
{
    static const char capabilities[] =
        "{\"mode\":\"local\",\"telemetry\":true,\"video\":true,"
        "\"bms\":true,\"logs\":false,\"config\":false,"
        "\"remote_video\":false}";
    size_t length = sizeof(capabilities) - 1U;

    if (buffer == NULL || buffer_size <= length) {
        return -1;
    }
    memcpy(buffer, capabilities, sizeof(capabilities));
    return (int)length;
}
