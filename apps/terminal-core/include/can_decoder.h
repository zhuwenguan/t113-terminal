#ifndef T113_CAN_DECODER_H
#define T113_CAN_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#include "can_receiver.h"
#include <race/vehicle_state.h>

/* CAN buses and IDs are centralized here; add future signals as the matrix
 * expands. */
#define CAN_DECODER_BUS_CAN0 0U
#define CAN_DECODER_BUS_CAN1 1U
#define CAN_DECODER_ID_UNASSIGNED 0xffffffffU
#define CAN_DECODER_ID_VCU_STATUS 0x120U
#define CAN_DECODER_ID_MOTOR_STATUS 0x121U
#define CAN_DECODER_ID_TEMPERATURE_STATUS 0x122U
#define CAN_DECODER_ID_BATTERY_TEMPERATURE 0x201U
#define CAN_DECODER_ID_TOTAL_VOLTAGE 0x01eU
#define CAN_DECODER_ID_DC_CURRENT 0x00eU
#define CAN_DECODER_ID_METER_CHARGE_DELTA CAN_DECODER_ID_UNASSIGNED
#define CAN_DECODER_FRESHNESS_TIMEOUT_US 1000000ULL
#define CAN_DECODER_METER_DELTA_MAS_OFFSET (-6000.0)
#define CAN_DECODER_MAS_PER_AH 3600000.0
#define CAN_DECODER_DEFAULT_SOC_INITIAL_PERCENT 100.0

/* The current authoritative KPE601 matrix uses every field of 0x00e for
 * HV2/HV1/HV3/current, so there is no confirmed charge-delta signal. Keep
 * the legacy accumulator disabled rather than interpreting HV2 as charge. */

typedef enum {
    CAN_DECODER_SOC_INITIALIZATION_CONFIG = 0,
    CAN_DECODER_SOC_INITIALIZATION_BMS,
    CAN_DECODER_SOC_INITIALIZATION_FULL_CHARGE,
    CAN_DECODER_SOC_INITIALIZATION_OCV,
    CAN_DECODER_SOC_INITIALIZATION_MANUAL
} CanDecoderSocInitialization;

typedef struct {
    double soc_initial_percent;
    double battery_usable_capacity_ah;
    CanDecoderSocInitialization soc_initialization;
    /* Parser-layer signs normalize device polarity to discharge-positive. */
    double current_discharge_sign;
    double meter_delta_discharge_sign;
} CanDecoderConfig;

typedef enum {
    CAN_DECODER_SOURCE_VCU_STATUS = 0,
    CAN_DECODER_SOURCE_MOTOR_STATUS,
    CAN_DECODER_SOURCE_TEMPERATURE_STATUS,
    CAN_DECODER_SOURCE_BATTERY_TEMPERATURE,
    CAN_DECODER_SOURCE_TOTAL_VOLTAGE,
    CAN_DECODER_SOURCE_DC_CURRENT,
    CAN_DECODER_SOURCE_METER_CHARGE_DELTA,
    CAN_DECODER_SOURCE_COUNT
} CanDecoderSource;

typedef struct {
    uint64_t source_timestamp_us[CAN_DECODER_SOURCE_COUNT];
    bool source_seen[CAN_DECODER_SOURCE_COUNT];
    CanDecoderConfig config;
    double meter_session_ah;
    double software_session_ah;
    bool software_current_seen;
    uint64_t software_current_timestamp_us;
    bool meter_frame_seen;
    CanMessage meter_last_frame;
    bool soc_config_valid;
} CanDecoder;

typedef struct {
    uint8_t bit_offset;
    uint8_t bit_length;
    double scale;
    double value_offset;
    bool is_signed;
} CanSignalDefinition;

void can_decoder_init(CanDecoder *decoder);

void can_decoder_config_default(CanDecoderConfig *config);

void can_decoder_init_with_config(CanDecoder *decoder,
                                  const CanDecoderConfig *config);

/*
 * Decode one raw CAN message into the accumulated vehicle state.
 * Returns true when at least one state field was updated. Signal validity is
 * cleared when its source has not been received within the freshness timeout.
 */
bool can_decoder_process(CanDecoder *decoder, const CanMessage *message,
                         VehicleState *state);

/* Expires stale signal groups even when the current event is not a CAN frame. */
void can_decoder_expire_stale(CanDecoder *decoder, uint64_t timestamp_us,
                              VehicleState *state);

#endif
