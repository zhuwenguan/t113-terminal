#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_decoder.h"
#include "console_log.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    uint8_t bus;
    uint32_t id;
    CanSignalDefinition motor_rpm;
    CanSignalDefinition torque_nm;
} MotorStatusDefinition;

typedef struct {
    uint8_t bus;
    uint32_t id;
    CanSignalDefinition motor_temperature;
    CanSignalDefinition controller_temperature;
} TemperatureStatusDefinition;

typedef struct {
    uint8_t bus;
    uint32_t id;
    CanSignalDefinition throttle_percent;
    CanSignalDefinition brake_percent;
    CanSignalDefinition mode;
    CanSignalDefinition drive_state;
} VcuStatusDefinition;

typedef struct {
    uint8_t bus;
    uint32_t id;
    CanSignalDefinition signal;
} SingleSignalDefinition;

/*
 * bit_offset uses LSB-first bit numbering within the CAN payload. Byte
 * numbers in the CAN matrix are 0-based, so Byte4 starts at bit 32 and
 * Byte6 starts at bit 48. The configured values below come from the current
 * CAN matrix.
 */
static const MotorStatusDefinition motor_status_definition = {
    CAN_DECODER_BUS_CAN0,
    CAN_DECODER_ID_MOTOR_STATUS,
    { 0U, 16U, 1.0, -15000.0, false }, /* motor_rpm */
    { 16U, 16U, 1.0, -5000.0, false } /* torque_nm */
};

static const TemperatureStatusDefinition temperature_status_definition = {
    CAN_DECODER_BUS_CAN0,
    CAN_DECODER_ID_TEMPERATURE_STATUS,
    { 0U, 8U, 1.0, -40.0, false }, /* motor_temperature_c */
    { 8U, 8U, 1.0, -40.0, false } /* controller_temperature_c */
};

static const VcuStatusDefinition vcu_status_definition = {
    CAN_DECODER_BUS_CAN0,
    CAN_DECODER_ID_VCU_STATUS,
    { 0U, 8U, 0.5, 0.0, false }, /* throttle_percent */
    { 8U, 8U, 0.5, 0.0, false }, /* brake_percent */
    { 16U, 8U, 1.0, 0.0, false }, /* mode */
    { 24U, 8U, 1.0, 0.0, false } /* drive_state */
};

static const SingleSignalDefinition battery_temperature_definition = {
    CAN_DECODER_BUS_CAN0,
    CAN_DECODER_ID_BATTERY_TEMPERATURE,
    { 24U, 8U, 1.0, 0.0, false } /* battery_temperature_c */
};

static const SingleSignalDefinition total_voltage_definition = {
    CAN_DECODER_BUS_CAN1,
    CAN_DECODER_ID_TOTAL_VOLTAGE,
    { 32U, 16U, 0.15, 0.0, false } /* hv_voltage_v */
};

static const SingleSignalDefinition dc_current_definition = {
    CAN_DECODER_BUS_CAN1,
    CAN_DECODER_ID_DC_CURRENT,
    { 48U, 16U, 0.02, -650.0, false } /* hv_current_a */
};

static const uint32_t can_decoder_source_masks[CAN_DECODER_SOURCE_COUNT] = {
    [CAN_DECODER_SOURCE_VCU_STATUS] =
        VEHICLE_STATE_VALID_THROTTLE_PERCENT |
        VEHICLE_STATE_VALID_BRAKE_PERCENT |
        VEHICLE_STATE_VALID_DRIVE_STATE | VEHICLE_STATE_VALID_MODE,
    [CAN_DECODER_SOURCE_MOTOR_STATUS] =
        VEHICLE_STATE_VALID_MOTOR_RPM | VEHICLE_STATE_VALID_TORQUE_NM,
    [CAN_DECODER_SOURCE_TEMPERATURE_STATUS] =
        VEHICLE_STATE_VALID_MOTOR_TEMPERATURE_C |
        VEHICLE_STATE_VALID_CONTROLLER_TEMPERATURE_C,
    [CAN_DECODER_SOURCE_BATTERY_TEMPERATURE] =
        VEHICLE_STATE_VALID_BATTERY_TEMPERATURE_C,
    [CAN_DECODER_SOURCE_TOTAL_VOLTAGE] = VEHICLE_STATE_VALID_HV_VOLTAGE_V,
    [CAN_DECODER_SOURCE_DC_CURRENT] = VEHICLE_STATE_VALID_HV_CURRENT_A
};

/* No CAN mapping has been supplied yet for HV SOC or LV voltage. */

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

static bool can_decoder_read_integer_range(
    const CanMessage *message, const CanSignalDefinition *definition,
    uint32_t minimum, uint32_t maximum, uint32_t *value)
{
    double decoded;

    if (value == NULL || !can_decoder_read_signal(message, definition,
                                                   &decoded) ||
        decoded < (double)minimum || decoded > (double)maximum ||
        decoded != (double)(uint32_t)decoded) {
        return false;
    }

    *value = (uint32_t)decoded;
    return true;
}

void can_decoder_init(CanDecoder *decoder)
{
    if (decoder != NULL) {
        memset(decoder, 0, sizeof(*decoder));
    }
}

static bool can_decoder_source_is_fresh(const CanDecoder *decoder,
                                        CanDecoderSource source,
                                        uint64_t timestamp_us)
{
    uint64_t source_timestamp_us;

    if (decoder == NULL || source >= CAN_DECODER_SOURCE_COUNT ||
        !decoder->source_seen[source]) {
        return false;
    }
    source_timestamp_us = decoder->source_timestamp_us[source];
    /* Concurrent producers timestamp samples before taking the shared state
     * lock. A slightly older LapPro sample can therefore be processed after a
     * newer CAN frame; that must not invalidate the newer CAN value. */
    return timestamp_us < source_timestamp_us ||
           timestamp_us - source_timestamp_us <=
               CAN_DECODER_FRESHNESS_TIMEOUT_US;
}

void can_decoder_expire_stale(CanDecoder *decoder, uint64_t timestamp_us,
                              VehicleState *state)
{
    unsigned int source;
    bool voltage_fresh;
    bool current_fresh;

    if (decoder == NULL || state == NULL) {
        return;
    }
    for (source = 0U; source < CAN_DECODER_SOURCE_COUNT; ++source) {
        if (!can_decoder_source_is_fresh(decoder, (CanDecoderSource)source,
                                         timestamp_us)) {
            state->valid_mask &= ~can_decoder_source_masks[source];
        }
    }
    voltage_fresh = can_decoder_source_is_fresh(
        decoder, CAN_DECODER_SOURCE_TOTAL_VOLTAGE, timestamp_us);
    current_fresh = can_decoder_source_is_fresh(
        decoder, CAN_DECODER_SOURCE_DC_CURRENT, timestamp_us);
    if (!voltage_fresh || !current_fresh) {
        state->valid_mask &= ~VEHICLE_STATE_VALID_HV_POWER_W;
    }
}

static void can_decoder_mark_source(CanDecoder *decoder,
                                    CanDecoderSource source,
                                    uint64_t timestamp_us)
{
    decoder->source_timestamp_us[source] = timestamp_us;
    decoder->source_seen[source] = true;
}

bool can_decoder_process(CanDecoder *decoder, const CanMessage *message,
                         VehicleState *state)
{
    bool updated = false;

    if (decoder == NULL || message == NULL || state == NULL ||
        message->dlc > sizeof(message->data) ||
        (message->flags & (CAN_MESSAGE_FLAG_ERROR | CAN_MESSAGE_FLAG_REMOTE)) !=
            0U) {
        return false;
    }

    can_decoder_expire_stale(decoder, message->timestamp_us, state);

    if (message->bus == vcu_status_definition.bus &&
        message->id == vcu_status_definition.id &&
        message->id != CAN_DECODER_ID_UNASSIGNED) {
        double throttle_value;
        double brake_value;
        uint32_t mode_value = 0U;
        uint32_t drive_state_value = 0U;
        bool throttle_updated = can_decoder_read_signal(
            message, &vcu_status_definition.throttle_percent,
            &throttle_value);
        bool brake_updated = can_decoder_read_signal(
            message, &vcu_status_definition.brake_percent, &brake_value);
        bool mode_updated = can_decoder_read_integer_range(
            message, &vcu_status_definition.mode, 0U, 254U, &mode_value);
        bool drive_state_updated = can_decoder_read_integer_range(
            message, &vcu_status_definition.drive_state, 0U, 1U,
            &drive_state_value);

        /* The physical range is 0..100 percent; raw 0xff is therefore
         * reserved as invalid for the two one-byte percentage signals. */
        if (throttle_updated &&
            (throttle_value < 0.0 || throttle_value > 100.0)) {
            throttle_updated = false;
        }
        if (brake_updated && (brake_value < 0.0 || brake_value > 100.0)) {
            brake_updated = false;
        }
        if (throttle_updated) {
            state->throttle_percent = throttle_value;
            state->valid_mask |= VEHICLE_STATE_VALID_THROTTLE_PERCENT;
        } else {
            state->valid_mask &= ~VEHICLE_STATE_VALID_THROTTLE_PERCENT;
        }
        if (brake_updated) {
            state->brake_percent = brake_value;
            state->valid_mask |= VEHICLE_STATE_VALID_BRAKE_PERCENT;
        } else {
            state->valid_mask &= ~VEHICLE_STATE_VALID_BRAKE_PERCENT;
        }
        if (mode_updated) {
            state->mode = mode_value;
            state->valid_mask |= VEHICLE_STATE_VALID_MODE;
        } else {
            state->valid_mask &= ~VEHICLE_STATE_VALID_MODE;
        }
        if (drive_state_updated) {
            state->drive_state = (VehicleDriveState)drive_state_value;
            state->valid_mask |= VEHICLE_STATE_VALID_DRIVE_STATE;
        } else {
            state->valid_mask &= ~VEHICLE_STATE_VALID_DRIVE_STATE;
        }

        updated = throttle_updated || brake_updated || mode_updated ||
                  drive_state_updated;
        if (updated) {
            can_decoder_mark_source(decoder, CAN_DECODER_SOURCE_VCU_STATUS,
                                    message->timestamp_us);
        }
        if (throttle_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> throttle=%.3f%%",
                      message->id, state->throttle_percent);
        }
        if (brake_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> brake=%.3f%%",
                      message->id, state->brake_percent);
        }
        if (mode_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> mode=%u", message->id,
                      state->mode);
        }
        if (drive_state_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> drive_state=%s",
                      message->id,
                      vehicle_drive_state_name(state->drive_state));
        }
    } else if (message->bus == motor_status_definition.bus &&
        message->id == motor_status_definition.id &&
        message->id != CAN_DECODER_ID_UNASSIGNED) {
        bool rpm_updated = can_decoder_apply_signal(
            message, &motor_status_definition.motor_rpm,
            VEHICLE_STATE_VALID_MOTOR_RPM, &state->motor_rpm, state);
        bool torque_updated = can_decoder_apply_signal(
            message, &motor_status_definition.torque_nm,
            VEHICLE_STATE_VALID_TORQUE_NM, &state->torque_nm, state);

        updated = rpm_updated || torque_updated;
        if (updated) {
            can_decoder_mark_source(decoder, CAN_DECODER_SOURCE_MOTOR_STATUS,
                                    message->timestamp_us);
        }
        if (rpm_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> motor_rpm=%.3f",
                      message->id, state->motor_rpm);
        }
        if (torque_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> torque=%.3fNm",
                      message->id, state->torque_nm);
        }
    } else if (message->bus == temperature_status_definition.bus &&
               message->id == temperature_status_definition.id &&
               message->id != CAN_DECODER_ID_UNASSIGNED) {
        bool motor_temperature_updated = can_decoder_apply_signal(
            message, &temperature_status_definition.motor_temperature,
            VEHICLE_STATE_VALID_MOTOR_TEMPERATURE_C,
            &state->motor_temperature_c, state);
        bool controller_temperature_updated = can_decoder_apply_signal(
            message, &temperature_status_definition.controller_temperature,
            VEHICLE_STATE_VALID_CONTROLLER_TEMPERATURE_C,
            &state->controller_temperature_c, state);

        updated = motor_temperature_updated || controller_temperature_updated;
        if (updated) {
            can_decoder_mark_source(decoder,
                                    CAN_DECODER_SOURCE_TEMPERATURE_STATUS,
                                    message->timestamp_us);
        }
        if (motor_temperature_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> motor_temperature=%.3fC",
                      message->id, state->motor_temperature_c);
        }
        if (controller_temperature_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> controller_temperature=%.3fC",
                      message->id, state->controller_temperature_c);
        }
    } else if (message->bus == battery_temperature_definition.bus &&
               message->id == battery_temperature_definition.id &&
               message->id != CAN_DECODER_ID_UNASSIGNED) {
        bool battery_temperature_updated = can_decoder_apply_signal(
            message, &battery_temperature_definition.signal,
            VEHICLE_STATE_VALID_BATTERY_TEMPERATURE_C,
            &state->battery_temperature_c, state);

        updated = battery_temperature_updated;
        if (battery_temperature_updated) {
            can_decoder_mark_source(decoder,
                                    CAN_DECODER_SOURCE_BATTERY_TEMPERATURE,
                                    message->timestamp_us);
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> battery_temperature=%.3fC",
                      message->id, state->battery_temperature_c);
        }
    } else if (message->bus == total_voltage_definition.bus &&
               message->id == total_voltage_definition.id &&
               message->id != CAN_DECODER_ID_UNASSIGNED) {
        bool voltage_updated = can_decoder_apply_signal(
            message, &total_voltage_definition.signal,
            VEHICLE_STATE_VALID_HV_VOLTAGE_V, &state->hv_voltage_v, state);
        bool power_updated = false;

        if ((state->valid_mask & VEHICLE_STATE_VALID_HV_CURRENT_A) != 0U) {
            state->hv_power_w = state->hv_voltage_v * state->hv_current_a;
            state->valid_mask |= VEHICLE_STATE_VALID_HV_POWER_W;
            power_updated = voltage_updated;
        }
        updated = voltage_updated;
        if (voltage_updated) {
            can_decoder_mark_source(decoder, CAN_DECODER_SOURCE_TOTAL_VOLTAGE,
                                    message->timestamp_us);
        }
        if (voltage_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_voltage=%.3fV",
                      message->id, state->hv_voltage_v);
        }
        if (power_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_power=%.3fW",
                      message->id, state->hv_power_w);
        }
    } else if (message->bus == dc_current_definition.bus &&
               message->id == dc_current_definition.id &&
               message->id != CAN_DECODER_ID_UNASSIGNED) {
        bool current_updated = can_decoder_apply_signal(
            message, &dc_current_definition.signal,
            VEHICLE_STATE_VALID_HV_CURRENT_A, &state->hv_current_a, state);
        bool power_updated = false;

        if ((state->valid_mask & VEHICLE_STATE_VALID_HV_VOLTAGE_V) != 0U) {
            state->hv_power_w = state->hv_voltage_v * state->hv_current_a;
            state->valid_mask |= VEHICLE_STATE_VALID_HV_POWER_W;
            power_updated = current_updated;
        }
        updated = current_updated;
        if (current_updated) {
            can_decoder_mark_source(decoder, CAN_DECODER_SOURCE_DC_CURRENT,
                                    message->timestamp_us);
        }
        if (current_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_current=%.3fA",
                      message->id, state->hv_current_a);
        }
        if (power_updated) {
            LOG_DEBUG("DECODER: CAN ID=0x%08x -> hv_power=%.3fW",
                      message->id, state->hv_power_w);
        }
    }

    if (updated) {
        can_decoder_expire_stale(decoder, message->timestamp_us, state);
        if ((state->valid_mask & (VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                                  VEHICLE_STATE_VALID_HV_CURRENT_A)) ==
            (VEHICLE_STATE_VALID_HV_VOLTAGE_V |
             VEHICLE_STATE_VALID_HV_CURRENT_A)) {
            state->hv_power_w = state->hv_voltage_v * state->hv_current_a;
            state->valid_mask |= VEHICLE_STATE_VALID_HV_POWER_W;
        }
        state->timestamp_us = message->timestamp_us;
    }

    return updated;
}
