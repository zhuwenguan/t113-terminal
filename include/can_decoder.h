#ifndef T113_CAN_DECODER_H
#define T113_CAN_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#include "can_receiver.h"
#include "vehicle_state.h"

/* Replace these placeholders when the vehicle CAN matrix is available. */
#define CAN_DECODER_ID_UNASSIGNED 0xffffffffU
#define CAN_DECODER_ID_MOTOR_STATUS CAN_DECODER_ID_UNASSIGNED
#define CAN_DECODER_ID_BATTERY_STATUS CAN_DECODER_ID_UNASSIGNED
#define CAN_DECODER_ID_LOW_VOLTAGE_STATUS CAN_DECODER_ID_UNASSIGNED

typedef struct {
    uint8_t bit_offset;
    uint8_t bit_length;
    double scale;
    double value_offset;
    bool is_signed;
} CanSignalDefinition;

/*
 * Decode one raw CAN message into the accumulated vehicle state.
 * Returns true when at least one state field was updated.
 */
bool can_decoder_process(const CanMessage *message, VehicleState *state);

#endif
