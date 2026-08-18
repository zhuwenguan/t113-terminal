#ifndef T113_CAN_DECODER_H
#define T113_CAN_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#include "can_receiver.h"
#include "vehicle_state.h"

/* CAN IDs are centralized here; add future signals as the matrix expands. */
#define CAN_DECODER_ID_UNASSIGNED 0xffffffffU
#define CAN_DECODER_ID_MOTOR_STATUS 0x121U
#define CAN_DECODER_ID_TOTAL_VOLTAGE 0x201U
#define CAN_DECODER_ID_DC_CURRENT 0x123U
#define CAN_DECODER_FRESHNESS_TIMEOUT_US 1000000ULL

typedef enum {
    CAN_DECODER_SOURCE_MOTOR_STATUS = 0,
    CAN_DECODER_SOURCE_TOTAL_VOLTAGE,
    CAN_DECODER_SOURCE_DC_CURRENT,
    CAN_DECODER_SOURCE_COUNT
} CanDecoderSource;

typedef struct {
    uint64_t source_timestamp_us[CAN_DECODER_SOURCE_COUNT];
    bool source_seen[CAN_DECODER_SOURCE_COUNT];
} CanDecoder;

typedef struct {
    uint8_t bit_offset;
    uint8_t bit_length;
    double scale;
    double value_offset;
    bool is_signed;
} CanSignalDefinition;

void can_decoder_init(CanDecoder *decoder);

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
