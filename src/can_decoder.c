#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_decoder.h"
#include "console_log.h"

#include <stddef.h>

typedef struct {
    uint32_t id;
    CanSignalDefinition motor_rpm;
    CanSignalDefinition torque_nm;
} MotorStatusDefinition;

typedef struct {
    uint32_t id;
    CanSignalDefinition hv_voltage_v;
    CanSignalDefinition hv_current_a;
    CanSignalDefinition hv_power_w;
    CanSignalDefinition hv_soc_percent;
} BatteryStatusDefinition;

typedef struct {
    uint32_t id;
    CanSignalDefinition lv_voltage_v;
} LowVoltageStatusDefinition;

/*
 * Signal maps are deliberately unassigned until the vehicle CAN matrix is
 * confirmed. bit_offset uses LSB-first bit numbering within the CAN payload.
 */
#define CAN_SIGNAL_UNASSIGNED \
    { 0U, 0U, 1.0, 0.0, false }

static const MotorStatusDefinition motor_status_definition = {
    CAN_DECODER_ID_MOTOR_STATUS,
    CAN_SIGNAL_UNASSIGNED, /* TODO: motor_rpm bit/length/scale/offset */
    CAN_SIGNAL_UNASSIGNED  /* TODO: torque_nm bit/length/scale/offset */
};

static const BatteryStatusDefinition battery_status_definition = {
    CAN_DECODER_ID_BATTERY_STATUS,
    CAN_SIGNAL_UNASSIGNED, /* TODO: hv_voltage_v bit/length/scale/offset */
    CAN_SIGNAL_UNASSIGNED, /* TODO: hv_current_a bit/length/scale/offset */
    CAN_SIGNAL_UNASSIGNED, /* TODO: hv_power_w bit/length/scale/offset */
    CAN_SIGNAL_UNASSIGNED  /* TODO: hv_soc_percent bit/length/scale/offset */
};

static const LowVoltageStatusDefinition low_voltage_status_definition = {
    CAN_DECODER_ID_LOW_VOLTAGE_STATUS,
    CAN_SIGNAL_UNASSIGNED /* TODO: lv_voltage_v bit/length/scale/offset */
};

static bool can_decoder_read_signal(const CanMessage *message,
                                    const CanSignalDefinition *definition,
                                    double *value)
{
    uint64_t raw_value = 0;
    uint64_t mask;
    uint8_t bit;
    uint16_t available_bits;

    if (message == NULL || definition == NULL || value == NULL ||
        definition->bit_length == 0U || definition->bit_length > 64U) {
        return false;
    }

    available_bits = (uint16_t)message->dlc * 8U;
    if ((uint16_t)definition->bit_offset + definition->bit_length >
        available_bits) {
        return false;
    }

    for (bit = 0U; bit < definition->bit_length; ++bit) {
        uint16_t payload_bit = (uint16_t)definition->bit_offset + bit;
        uint8_t byte_index = (uint8_t)(payload_bit / 8U);
        uint8_t bit_index = (uint8_t)(payload_bit % 8U);

        if ((message->data[byte_index] & (uint8_t)(1U << bit_index)) != 0U) {
            raw_value |= 1ULL << bit;
        }
    }

    mask = definition->bit_length == 64U
               ? UINT64_MAX
               : (1ULL << definition->bit_length) - 1ULL;
    raw_value &= mask;

    if (definition->is_signed && definition->bit_length < 64U &&
        (raw_value & (1ULL << (definition->bit_length - 1U))) != 0U) {
        raw_value |= ~mask;
        *value = (double)(int64_t)raw_value * definition->scale +
                 definition->value_offset;
    } else if (definition->is_signed) {
        *value = (double)(int64_t)raw_value * definition->scale +
                 definition->value_offset;
    } else {
        *value = (double)raw_value * definition->scale +
                 definition->value_offset;
    }

    return true;
}

static bool can_decoder_apply_signal(const CanMessage *message,
                                     const CanSignalDefinition *definition,
                                     uint32_t valid_bit, double *destination,
                                     VehicleState *state)
{
    double value;

    if (message->id == CAN_DECODER_ID_UNASSIGNED ||
        !can_decoder_read_signal(message, definition, &value)) {
        return false;
    }

    *destination = value;
    state->valid_mask |= valid_bit;
    return true;
}

bool can_decoder_process(const CanMessage *message, VehicleState *state)
{
    bool updated = false;

    if (message == NULL || state == NULL ||
        message->dlc > sizeof(message->data) ||
        (message->flags & (CAN_MESSAGE_FLAG_ERROR | CAN_MESSAGE_FLAG_REMOTE)) !=
            0U) {
        return false;
    }

    if (message->id == motor_status_definition.id &&
        message->id != CAN_DECODER_ID_UNASSIGNED) {
        bool rpm_updated = can_decoder_apply_signal(
            message, &motor_status_definition.motor_rpm,
            VEHICLE_STATE_VALID_MOTOR_RPM, &state->motor_rpm, state);
        bool torque_updated = can_decoder_apply_signal(
            message, &motor_status_definition.torque_nm,
            VEHICLE_STATE_VALID_TORQUE_NM, &state->torque_nm, state);

        updated = rpm_updated || torque_updated;
        if (rpm_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> motor_rpm=%.3f",
                      message->id, state->motor_rpm);
        }
        if (torque_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> torque=%.3fNm",
                      message->id, state->torque_nm);
        }
    } else if (message->id == battery_status_definition.id &&
               message->id != CAN_DECODER_ID_UNASSIGNED) {
        bool voltage_updated = can_decoder_apply_signal(
            message, &battery_status_definition.hv_voltage_v,
            VEHICLE_STATE_VALID_HV_VOLTAGE_V, &state->hv_voltage_v, state);
        bool current_updated = can_decoder_apply_signal(
            message, &battery_status_definition.hv_current_a,
            VEHICLE_STATE_VALID_HV_CURRENT_A, &state->hv_current_a, state);
        bool power_updated = can_decoder_apply_signal(
            message, &battery_status_definition.hv_power_w,
            VEHICLE_STATE_VALID_HV_POWER_W, &state->hv_power_w, state);
        bool soc_updated = can_decoder_apply_signal(
            message, &battery_status_definition.hv_soc_percent,
            VEHICLE_STATE_VALID_HV_SOC_PERCENT, &state->hv_soc_percent, state);

        updated = voltage_updated || current_updated || power_updated ||
                  soc_updated;
        if (voltage_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_voltage=%.3fV",
                      message->id, state->hv_voltage_v);
        }
        if (current_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_current=%.3fA",
                      message->id, state->hv_current_a);
        }
        if (power_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_power=%.3fW",
                      message->id, state->hv_power_w);
        }
        if (soc_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_soc=%.3f%%",
                      message->id, state->hv_soc_percent);
        }
    } else if (message->id == low_voltage_status_definition.id &&
               message->id != CAN_DECODER_ID_UNASSIGNED) {
        updated = can_decoder_apply_signal(
            message, &low_voltage_status_definition.lv_voltage_v,
            VEHICLE_STATE_VALID_LV_VOLTAGE_V, &state->lv_voltage_v, state);
        if (updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> lv_voltage=%.3fV",
                      message->id, state->lv_voltage_v);
        }
    }

    if (updated) {
        state->timestamp_us = message->timestamp_us;
    }

    return updated;
}
