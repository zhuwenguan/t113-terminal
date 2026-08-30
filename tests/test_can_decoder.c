#include "can_decoder.h"

#include "test_check.h"
#include <stddef.h>
#include <string.h>

static CanMessage make_message(uint64_t timestamp_us, uint8_t bus, uint32_t id)
{
    CanMessage message;

    memset(&message, 0, sizeof(message));
    message.timestamp_us = timestamp_us;
    message.bus = bus;
    message.id = id;
    message.flags = CAN_MESSAGE_FLAG_STANDARD;
    message.dlc = 8U;
    return message;
}

static void put_u16_le(CanMessage *message, size_t byte_offset,
                       uint16_t value)
{
    message->data[byte_offset] = (uint8_t)value;
    message->data[byte_offset + 1U] = (uint8_t)(value >> 8U);
}

int main(void)
{
    CanDecoder decoder;
    VehicleState state;
    CanMessage motor;
    CanMessage temperatures;
    CanMessage battery_temperature;
    CanMessage voltage;
    CanMessage current;
    CanMessage wrong_bus_voltage;
    const uint64_t start_us = 10000000ULL;

    memset(&state, 0, sizeof(state));
    can_decoder_init(&decoder);
    motor = make_message(start_us, CAN_DECODER_BUS_CAN0,
                         CAN_DECODER_ID_MOTOR_STATUS);
    put_u16_le(&motor, 0U, 15001U);
    put_u16_le(&motor, 2U, 5002U);
    temperatures = make_message(start_us + 50U, CAN_DECODER_BUS_CAN0,
                                CAN_DECODER_ID_TEMPERATURE_STATUS);
    temperatures.data[0] = 72U;
    temperatures.data[1] = 85U;
    battery_temperature = make_message(start_us + 75U,
                                       CAN_DECODER_BUS_CAN0,
                                       CAN_DECODER_ID_BATTERY_TEMPERATURE);
    battery_temperature.data[3] = 35U;
    voltage = make_message(start_us + 100U, CAN_DECODER_BUS_CAN1,
                           CAN_DECODER_ID_TOTAL_VOLTAGE);
    put_u16_le(&voltage, 4U, 2400U);
    current = make_message(start_us + 200U, CAN_DECODER_BUS_CAN1,
                           CAN_DECODER_ID_DC_CURRENT);
    put_u16_le(&current, 6U, 33000U);
    wrong_bus_voltage = voltage;
    wrong_bus_voltage.bus = CAN_DECODER_BUS_CAN0;

    TEST_CHECK(!can_decoder_process(&decoder, &wrong_bus_voltage, &state));

    TEST_CHECK(can_decoder_process(&decoder, &motor, &state));
    TEST_CHECK(can_decoder_process(&decoder, &temperatures, &state));
    TEST_CHECK(can_decoder_process(&decoder, &battery_temperature, &state));
    TEST_CHECK(can_decoder_process(&decoder, &voltage, &state));
    TEST_CHECK(can_decoder_process(&decoder, &current, &state));
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_CAN_REQUIRED_MASK) ==
           VEHICLE_STATE_CAN_REQUIRED_MASK);
    TEST_CHECK(state.motor_rpm == 1.0);
    TEST_CHECK(state.torque_nm == 2.0);
    TEST_CHECK(state.motor_temperature_c == 32.0);
    TEST_CHECK(state.controller_temperature_c == 45.0);
    TEST_CHECK(state.battery_temperature_c == 35.0);
    TEST_CHECK(state.hv_voltage_v == 360.0);
    TEST_CHECK(state.hv_current_a == 10.0);
    TEST_CHECK(state.hv_power_w == 3600.0);

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
