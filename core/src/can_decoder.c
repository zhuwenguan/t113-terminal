#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_decoder.h"
#include "console_log.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t id;
    CanSignalDefinition motor_rpm;
    CanSignalDefinition torque_nm;
} MotorStatusDefinition;

typedef struct {
    uint32_t id;
    CanSignalDefinition signal;
} SingleSignalDefinition;

/*
 * bit_offset uses LSB-first bit numbering within the CAN payload. The
 * configured values below come from the currently available CAN matrix.
 */
static const MotorStatusDefinition motor_status_definition = {
    CAN_DECODER_ID_MOTOR_STATUS,
    { 0U, 16U, 1.0, -15000.0, false }, /* motor_rpm */
    { 16U, 16U, 1.0, -5000.0, false } /* torque_nm */
};

static const SingleSignalDefinition total_voltage_definition = {
    CAN_DECODER_ID_TOTAL_VOLTAGE,
    { 0U, 16U, 0.1, 0.0, false } /* hv_voltage_v */
};

static const SingleSignalDefinition dc_current_definition = {
    CAN_DECODER_ID_DC_CURRENT,
    { 16U, 16U, 1.0, -1000.0, false } /* hv_current_a */
};

static const uint32_t can_decoder_source_masks[CAN_DECODER_SOURCE_COUNT] = {
    [CAN_DECODER_SOURCE_MOTOR_STATUS] =
        VEHICLE_STATE_VALID_MOTOR_RPM | VEHICLE_STATE_VALID_TORQUE_NM,
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

    if (message->id == motor_status_definition.id &&
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
    } else if (message->id == total_voltage_definition.id &&
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
    } else if (message->id == dc_current_definition.id &&
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
