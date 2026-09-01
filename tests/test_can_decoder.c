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

static void test_kpe_current_does_not_fake_meter_soc(void)
{
    CanDecoderConfig config;
    CanDecoder decoder;
    VehicleState state;
    CanMessage message;

    can_decoder_config_default(&config);
    config.battery_usable_capacity_ah = 20.0;
    memset(&state, 0, sizeof(state));
    can_decoder_init_with_config(&decoder, &config);
    message = make_message(20000000ULL, CAN_DECODER_BUS_CAN1,
                           CAN_DECODER_ID_DC_CURRENT);
    put_u16_le(&message, 0U, 1000U); /* KPE601 HV2, not meter delta. */
    put_u16_le(&message, 6U, 32500U); /* KPE601 current. */
    TEST_CHECK(can_decoder_process(&decoder, &message, &state));
    TEST_CHECK(state.hv_discharge_ah == 0.0);
    TEST_CHECK(state.hv_software_discharge_ah == 0.0);
    TEST_CHECK(!state.hv_soc_valid);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_HV_SOC_PERCENT) == 0U);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_HV_CURRENT_A) != 0U);
}

int main(void)
{
    CanDecoder decoder;
    VehicleState state;
    CanMessage motor;
    CanMessage vcu_status;
    CanMessage temperatures;
    CanMessage battery_temperature;
    CanMessage voltage;
    CanMessage current;
    CanMessage wrong_bus_voltage;
    const uint64_t start_us = 10000000ULL;

    test_kpe_current_does_not_fake_meter_soc();
    memset(&state, 0, sizeof(state));
    can_decoder_init(&decoder);
    motor = make_message(start_us, CAN_DECODER_BUS_CAN0,
                         CAN_DECODER_ID_MOTOR_STATUS);
    put_u16_le(&motor, 0U, 15001U);
    put_u16_le(&motor, 2U, 5002U);
    vcu_status = make_message(start_us + 25U, CAN_DECODER_BUS_CAN0,
                              CAN_DECODER_ID_VCU_STATUS);
    vcu_status.data[0] = 125U;
    vcu_status.data[1] = 18U;
    vcu_status.data[2] = 2U;
    vcu_status.data[3] = 1U;
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
    TEST_CHECK(can_decoder_process(&decoder, &vcu_status, &state));
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
    TEST_CHECK(state.throttle_percent == 62.5);
    TEST_CHECK(state.brake_percent == 9.0);
    TEST_CHECK(state.mode == 2U);
    TEST_CHECK(state.drive_state == VEHICLE_DRIVE_STATE_READY);

    vcu_status.timestamp_us = start_us + 250U;
    vcu_status.data[0] = 0xffU;
    TEST_CHECK(can_decoder_process(&decoder, &vcu_status, &state));
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_THROTTLE_PERCENT) == 0U);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_BRAKE_PERCENT) != 0U);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_MODE) != 0U);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_VALID_DRIVE_STATE) != 0U);

    can_decoder_expire_stale(
        &decoder, start_us + CAN_DECODER_FRESHNESS_TIMEOUT_US + 501U, &state);
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
