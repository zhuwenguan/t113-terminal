#include "can_decoder.h"

#include "test_check.h"
#include <string.h>

static CanMessage make_message(uint64_t timestamp_us, uint32_t id,
                               uint16_t first, uint16_t second)
{
    CanMessage message;

    memset(&message, 0, sizeof(message));
    message.timestamp_us = timestamp_us;
    message.id = id;
    message.flags = CAN_MESSAGE_FLAG_STANDARD;
    message.dlc = 4U;
    message.data[0] = (uint8_t)first;
    message.data[1] = (uint8_t)(first >> 8U);
    message.data[2] = (uint8_t)second;
    message.data[3] = (uint8_t)(second >> 8U);
    return message;
}

int main(void)
{
    CanDecoder decoder;
    VehicleState state;
    CanMessage motor;
    CanMessage voltage;
    CanMessage current;
    const uint64_t start_us = 10000000ULL;

    memset(&state, 0, sizeof(state));
    can_decoder_init(&decoder);
    motor = make_message(start_us, CAN_DECODER_ID_MOTOR_STATUS, 15001U,
                         5002U);
    voltage = make_message(start_us + 100U, CAN_DECODER_ID_TOTAL_VOLTAGE,
                           4000U, 0U);
    current = make_message(start_us + 200U, CAN_DECODER_ID_DC_CURRENT, 0U,
                           1010U);

    TEST_CHECK(can_decoder_process(&decoder, &motor, &state));
    TEST_CHECK(can_decoder_process(&decoder, &voltage, &state));
    TEST_CHECK(can_decoder_process(&decoder, &current, &state));
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_CAN_REQUIRED_MASK) ==
           VEHICLE_STATE_CAN_REQUIRED_MASK);
    TEST_CHECK(state.motor_rpm == 1.0);
    TEST_CHECK(state.torque_nm == 2.0);
    TEST_CHECK(state.hv_voltage_v == 400.0);
    TEST_CHECK(state.hv_current_a == 10.0);
    TEST_CHECK(state.hv_power_w == 4000.0);

    can_decoder_expire_stale(
        &decoder, start_us + CAN_DECODER_FRESHNESS_TIMEOUT_US + 201U, &state);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_CAN_REQUIRED_MASK) == 0U);

    voltage.timestamp_us = start_us + CAN_DECODER_FRESHNESS_TIMEOUT_US + 300U;
    TEST_CHECK(can_decoder_process(&decoder, &voltage, &state));
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_HV_VOLTAGE_V) != 0U);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_HV_POWER_W) == 0U);

    /* A delayed callback with an older timestamp must not expire newer CAN. */
    can_decoder_expire_stale(&decoder, 1U, &state);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_HV_VOLTAGE_V) != 0U);

    return 0;
}
