#define _POSIX_C_SOURCE 200809L

#include "bms_parser.h"

#include <linux/can.h>

#include <stddef.h>
#include <string.h>

#include <race/console_log.h>

static bool bms_timestamp_fresh(uint64_t now_mono_us,
                                uint64_t sample_mono_us)
{
    return sample_mono_us != 0U && now_mono_us >= sample_mono_us &&
           now_mono_us - sample_mono_us <= BMS_STALE_TIMEOUT_US;
}

static void bms_note_timestamp(BmsState *state, uint64_t timestamp_mono_us)
{
    if (timestamp_mono_us > state->timestamp_mono_us) {
        state->timestamp_mono_us = timestamp_mono_us;
    }
}

static uint16_t bms_load_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint64_t bms_load_le64(const uint8_t data[8])
{
    uint64_t raw = 0U;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        raw |= (uint64_t)data[index] << (index * 8U);
    }
    return raw;
}

static bool bms_raw_bit(uint64_t raw, unsigned int bit)
{
    return (raw & (UINT64_C(1) << bit)) != 0U;
}

static void bms_append_fault_event(BmsParser *parser, unsigned int board,
                                   BmsFaultType type, unsigned int channel,
                                   bool active, uint64_t timestamp_mono_us)
{
    BmsState *state = &parser->state;
    BmsFaultEvent *event;

    state->fault_event_sequence++;
    event = &state->fault_events[state->fault_event_head];
    event->timestamp_mono_us = timestamp_mono_us;
    event->sequence = state->fault_event_sequence;
    event->board = (uint8_t)board;
    event->channel = (uint8_t)channel;
    event->type = type;
    event->active = active;
    state->fault_event_head =
        (state->fault_event_head + 1U) % BMS_FAULT_EVENT_CAPACITY;
    if (state->fault_event_count < BMS_FAULT_EVENT_CAPACITY) {
        state->fault_event_count++;
    }
}

static void bms_set_fault(BmsParser *parser, unsigned int board,
                          BmsFaultType type, unsigned int channel,
                          bool *destination, bool active,
                          uint64_t timestamp_mono_us)
{
    if (*destination == active) {
        return;
    }
    *destination = active;
    bms_append_fault_event(parser, board, type, channel, active,
                           timestamp_mono_us);
}

static bool bms_valid_message(const CanMessage *message)
{
    if (message == NULL || message->bus != BMS_PARSER_BUS ||
        message->dlc != 8U || message->id > CAN_SFF_MASK) {
        return false;
    }
    /* A BMS frame must be a standard data frame.  Do not trust a lower
     * 11-bit ID copied from an extended or remote frame. */
    return (message->flags & CAN_MESSAGE_FLAG_STANDARD) != 0U &&
           (message->flags & (CAN_MESSAGE_FLAG_EXTENDED |
                              CAN_MESSAGE_FLAG_REMOTE |
                              CAN_MESSAGE_FLAG_ERROR)) == 0U;
}

static bool bms_board_for_range(uint32_t id, uint32_t base,
                                unsigned int *board)
{
    uint32_t suffix;

    if (base == BMS_PARSER_CELL_VOLTAGE_BASE) {
        uint32_t group_base = id & 0x0f0U;

        if ((id & 0xf00U) != 0x100U || group_base > 0x050U ||
            (group_base % 0x10U) != 0U) {
            return false;
        }
    } else if ((id & 0xff0U) != base) {
        return false;
    }
    suffix = id & 0x0fU;
    if (suffix >= BMS_BOARD_COUNT) {
        return false;
    }
    if (board != NULL) {
        *board = (unsigned int)suffix;
    }
    return true;
}

static bool bms_decode_cell_voltage(BmsParser *parser,
                                    const CanMessage *message)
{
    unsigned int board;
    unsigned int group;
    unsigned int cell;
    BmsBoardState *board_state;

    if (!bms_board_for_range(message->id, BMS_PARSER_CELL_VOLTAGE_BASE,
                             &board)) {
        return false;
    }
    group = (message->id & 0x0f0U) / 0x10U;
    if (group >= 6U) {
        return false;
    }
    board_state = &parser->state.board[board];
    if (board_state->cell_update_mono_us[group * 4U] != 0U &&
        message->timestamp_us <
            board_state->cell_update_mono_us[group * 4U]) {
        return true;
    }
    for (cell = 0U; cell < 4U; ++cell) {
        unsigned int index = group * 4U + cell;

        board_state->cell_voltage_v[index] =
            (float)bms_load_le16(&message->data[cell * 2U]) * 0.0001f;
        board_state->cell_valid[index] = true;
        board_state->cell_update_mono_us[index] = message->timestamp_us;
    }
    if (message->timestamp_us > board_state->last_update_mono_us) {
        board_state->last_update_mono_us = message->timestamp_us;
    }
    board_state->valid = true;
    bms_note_timestamp(&parser->state, message->timestamp_us);
    return true;
}

static bool bms_decode_temperature(BmsParser *parser,
                                    const CanMessage *message)
{
    unsigned int board;
    unsigned int temperature;
    BmsBoardState *board_state;

    if (!bms_board_for_range(message->id, BMS_PARSER_TEMPERATURE_BASE,
                             &board)) {
        return false;
    }
    board_state = &parser->state.board[board];
    if (board_state->temperature_update_mono_us[0] != 0U &&
        message->timestamp_us < board_state->temperature_update_mono_us[0]) {
        return true;
    }
    for (temperature = 0U; temperature < BMS_TEMPS_PER_BOARD;
         ++temperature) {
        board_state->temperature_c[temperature] =
            (float)message->data[temperature] - 50.0f;
        board_state->temperature_valid[temperature] = true;
        board_state->temperature_update_mono_us[temperature] =
            message->timestamp_us;
    }
    if (message->timestamp_us > board_state->last_update_mono_us) {
        board_state->last_update_mono_us = message->timestamp_us;
    }
    board_state->valid = true;
    bms_note_timestamp(&parser->state, message->timestamp_us);
    return true;
}

static void bms_decode_voltage_faults(BmsParser *parser,
                                      BmsBoardState *board_state,
                                      unsigned int board, uint64_t raw,
                                      uint64_t timestamp_mono_us)
{
    unsigned int channel;

    for (channel = 0U; channel < 12U; ++channel) {
        bms_set_fault(parser, board, BMS_FAULT_CELL_VOLTAGE_OPEN, channel,
                      &board_state->voltage_open[channel],
                      bms_raw_bit(raw, 1U + channel), timestamp_mono_us);
        bms_set_fault(parser, board, BMS_FAULT_CELL_VOLTAGE_OPEN,
                      channel + 12U,
                      &board_state->voltage_open[channel + 12U],
                      bms_raw_bit(raw, 17U + channel), timestamp_mono_us);
    }
    bms_set_fault(parser, board, BMS_FAULT_LOW_NEGATIVE_VOLTAGE_OPEN, 0U,
                  &board_state->low_negative_voltage_open,
                  bms_raw_bit(raw, 0U), timestamp_mono_us);
    bms_set_fault(parser, board, BMS_FAULT_LOW_POSITIVE_VOLTAGE, 0U,
                  &board_state->low_positive_voltage_fault,
                  bms_raw_bit(raw, 13U), timestamp_mono_us);
    bms_set_fault(parser, board, BMS_FAULT_LTC6804_LOW_COMM, 0U,
                  &board_state->ltc6804_low_comm_fault,
                  bms_raw_bit(raw, 14U), timestamp_mono_us);
    bms_set_fault(parser, board, BMS_FAULT_LTC6804_HIGH_COMM, 0U,
                  &board_state->ltc6804_high_comm_fault,
                  bms_raw_bit(raw, 15U), timestamp_mono_us);
    bms_set_fault(parser, board, BMS_FAULT_HIGH_NEGATIVE_VOLTAGE_OPEN, 0U,
                  &board_state->high_negative_voltage_open,
                  bms_raw_bit(raw, 16U), timestamp_mono_us);
    bms_set_fault(parser, board, BMS_FAULT_HIGH_POSITIVE_VOLTAGE, 0U,
                  &board_state->high_positive_voltage_fault,
                  bms_raw_bit(raw, 29U), timestamp_mono_us);
}

static bool bms_decode_fault_30(BmsParser *parser,
                                const CanMessage *message)
{
    unsigned int board;
    unsigned int channel;
    uint64_t raw;
    BmsBoardState *board_state;

    if (!bms_board_for_range(message->id, BMS_PARSER_FAULT_30_BASE, &board)) {
        return false;
    }
    raw = bms_load_le64(message->data);
    board_state = &parser->state.board[board];
    if (board_state->fault_30_update_mono_us != 0U &&
        message->timestamp_us < board_state->fault_30_update_mono_us) {
        return true;
    }
    board_state->fault_30_raw = raw;
    board_state->fault_30_update_mono_us = message->timestamp_us;
    bms_decode_voltage_faults(parser, board_state, board, raw,
                              message->timestamp_us);
    for (channel = 0U; channel < BMS_FAULT_TEMP_CHANNELS; ++channel) {
        if (channel < 24U) {
            bms_set_fault(parser, board, BMS_FAULT_TEMP_OPEN, channel,
                          &board_state->temp_open[channel],
                          bms_raw_bit(raw, 32U + channel),
                          message->timestamp_us);
        }
        if (channel < 8U) {
            bms_set_fault(parser, board, BMS_FAULT_TEMP_SHORT, channel,
                          &board_state->temp_short[channel],
                          bms_raw_bit(raw, 56U + channel),
                          message->timestamp_us);
        }
    }
    if (message->timestamp_us > board_state->last_fault_update_mono_us) {
        board_state->last_fault_update_mono_us = message->timestamp_us;
    }
    if (message->timestamp_us > board_state->last_update_mono_us) {
        board_state->last_update_mono_us = message->timestamp_us;
    }
    board_state->fault_valid = true;
    board_state->valid = true;
    bms_note_timestamp(&parser->state, message->timestamp_us);
    return true;
}

static bool bms_decode_fault_31(BmsParser *parser,
                                const CanMessage *message)
{
    unsigned int board;
    unsigned int channel;
    uint64_t raw;
    BmsBoardState *board_state;

    if (!bms_board_for_range(message->id, BMS_PARSER_FAULT_31_BASE, &board)) {
        return false;
    }
    raw = bms_load_le64(message->data);
    board_state = &parser->state.board[board];
    if (board_state->fault_31_update_mono_us != 0U &&
        message->timestamp_us < board_state->fault_31_update_mono_us) {
        return true;
    }
    board_state->fault_31_raw = raw;
    board_state->fault_31_update_mono_us = message->timestamp_us;
    for (channel = 0U; channel < 16U; ++channel) {
        bms_set_fault(parser, board, BMS_FAULT_TEMP_SHORT, channel + 8U,
                      &board_state->temp_short[channel + 8U],
                      bms_raw_bit(raw, channel), message->timestamp_us);
    }
    for (channel = 0U; channel < BMS_CELLS_PER_BOARD; ++channel) {
        bms_set_fault(parser, board, BMS_FAULT_BALANCE_STUCK_OPEN, channel,
                      &board_state->balance_stuck_open[channel],
                      bms_raw_bit(raw, 16U + channel),
                      message->timestamp_us);
        bms_set_fault(parser, board, BMS_FAULT_BALANCE_STUCK_CLOSED, channel,
                      &board_state->balance_stuck_closed[channel],
                      bms_raw_bit(raw, 40U + channel),
                      message->timestamp_us);
    }
    if (message->timestamp_us > board_state->last_fault_update_mono_us) {
        board_state->last_fault_update_mono_us = message->timestamp_us;
    }
    if (message->timestamp_us > board_state->last_update_mono_us) {
        board_state->last_update_mono_us = message->timestamp_us;
    }
    board_state->fault_valid = true;
    board_state->valid = true;
    bms_note_timestamp(&parser->state, message->timestamp_us);
    return true;
}

static bool bms_decode_meter(BmsParser *parser, const CanMessage *message)
{
    if (message->id == BMS_PARSER_ID_KPE_UI) {
        if (parser->state.meter_ui_update_mono_us != 0U &&
            message->timestamp_us < parser->state.meter_ui_update_mono_us) {
            return true;
        }
        parser->state.hv2_v = (float)bms_load_le16(&message->data[0]) * 0.15f;
        parser->state.hv1_v = (float)bms_load_le16(&message->data[2]) * 0.15f;
        parser->state.hv3_v = (float)bms_load_le16(&message->data[4]) * 0.15f;
        parser->state.current_a =
            (float)bms_load_le16(&message->data[6]) * 0.02f - 650.0f;
        parser->state.meter_ui_update_mono_us = message->timestamp_us;
    } else if (message->id == BMS_PARSER_ID_KPE_AP) {
        if (parser->state.meter_ap_update_mono_us != 0U &&
            message->timestamp_us < parser->state.meter_ap_update_mono_us) {
            return true;
        }
        /* Bytes 0..3 are AH/PH.  HV4/HV5 intentionally start at Byte4. */
        parser->state.hv4_v =
            (float)bms_load_le16(&message->data[4]) * 0.15f;
        parser->state.hv5_v =
            (float)bms_load_le16(&message->data[6]) * 0.15f;
        parser->state.meter_ap_update_mono_us = message->timestamp_us;
    } else {
        return false;
    }
    if (message->timestamp_us > parser->state.meter_last_update_mono_us) {
        parser->state.meter_last_update_mono_us = message->timestamp_us;
    }
    parser->state.meter_ui_valid = bms_timestamp_fresh(
        message->timestamp_us, parser->state.meter_ui_update_mono_us);
    parser->state.meter_ap_valid = bms_timestamp_fresh(
        message->timestamp_us, parser->state.meter_ap_update_mono_us);
    parser->state.meter_valid = parser->state.meter_ui_valid &&
                                parser->state.meter_ap_valid;
    bms_note_timestamp(&parser->state, message->timestamp_us);
    return true;
}

static bool bms_id_is_meter(uint32_t id)
{
    return id == BMS_PARSER_ID_KPE_UI || id == BMS_PARSER_ID_KPE_AP;
}

void bms_parser_init(BmsParser *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

void bms_parser_expire_stale(BmsParser *parser, uint64_t now_mono_us)
{
    unsigned int board;
    unsigned int channel;

    if (parser == NULL) {
        return;
    }
    parser->state.meter_ui_valid = bms_timestamp_fresh(
        now_mono_us, parser->state.meter_ui_update_mono_us);
    parser->state.meter_ap_valid = bms_timestamp_fresh(
        now_mono_us, parser->state.meter_ap_update_mono_us);
    parser->state.meter_valid = parser->state.meter_ui_valid &&
                                parser->state.meter_ap_valid;
    parser->state.valid = parser->state.meter_ui_valid ||
                          parser->state.meter_ap_valid;
    for (board = 0U; board < BMS_BOARD_COUNT; ++board) {
        BmsBoardState *board_state = &parser->state.board[board];
        bool board_valid = false;

        for (channel = 0U; channel < BMS_CELLS_PER_BOARD; ++channel) {
            board_state->cell_valid[channel] = bms_timestamp_fresh(
                now_mono_us, board_state->cell_update_mono_us[channel]);
            board_valid = board_valid || board_state->cell_valid[channel];
        }
        for (channel = 0U; channel < BMS_TEMPS_PER_BOARD; ++channel) {
            board_state->temperature_valid[channel] = bms_timestamp_fresh(
                now_mono_us, board_state->temperature_update_mono_us[channel]);
            board_valid = board_valid || board_state->temperature_valid[channel];
        }
        board_state->fault_valid = bms_timestamp_fresh(
            now_mono_us, board_state->last_fault_update_mono_us);
        board_valid = board_valid || board_state->fault_valid;
        board_state->valid = board_valid;
        parser->state.valid = parser->state.valid || board_valid;
    }
}

bool bms_parser_process(BmsParser *parser, const CanMessage *message)
{
    bool accepted = false;

    if (parser == NULL || !bms_valid_message(message)) {
        return false;
    }
    if (bms_board_for_range(message->id, BMS_PARSER_CELL_VOLTAGE_BASE, NULL)) {
        accepted = bms_decode_cell_voltage(parser, message);
    } else if (bms_board_for_range(message->id,
                                   BMS_PARSER_TEMPERATURE_BASE, NULL)) {
        accepted = bms_decode_temperature(parser, message);
    } else if (bms_board_for_range(message->id, BMS_PARSER_FAULT_30_BASE,
                                   NULL)) {
        accepted = bms_decode_fault_30(parser, message);
    } else if (bms_board_for_range(message->id, BMS_PARSER_FAULT_31_BASE,
                                   NULL)) {
        accepted = bms_decode_fault_31(parser, message);
    } else if (bms_id_is_meter(message->id)) {
        accepted = bms_decode_meter(parser, message);
    }
    if (!accepted) {
        return false;
    }
    bms_parser_expire_stale(parser, parser->state.timestamp_mono_us);
    return true;
}

void bms_parser_snapshot(BmsParser *parser, uint64_t now_mono_us,
                         BmsState *state)
{
    if (parser == NULL || state == NULL) {
        return;
    }
    bms_parser_expire_stale(parser, now_mono_us);
    *state = parser->state;
    state->timestamp_mono_us = now_mono_us;
}
