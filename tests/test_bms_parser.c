#include "bms_parser.h"

#include "test_check.h"

#include <linux/can.h>

#include <math.h>
#include <stddef.h>
#include <string.h>

static CanMessage make_message(uint64_t timestamp_us, uint32_t id)
{
    CanMessage message;

    memset(&message, 0, sizeof(message));
    message.timestamp_us = timestamp_us;
    message.bus = BMS_PARSER_BUS;
    message.id = id;
    message.flags = CAN_MESSAGE_FLAG_STANDARD;
    message.dlc = 8U;
    return message;
}

static void put_u16_le(CanMessage *message, size_t offset, uint16_t value)
{
    message->data[offset] = (uint8_t)value;
    message->data[offset + 1U] = (uint8_t)(value >> 8U);
}

static void set_bit(CanMessage *message, unsigned int bit)
{
    message->data[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
}

static void test_all_cell_voltages(void)
{
    BmsParser parser;
    unsigned int board;
    unsigned int group;
    unsigned int cell;
    uint64_t timestamp = 1000000U;

    bms_parser_init(&parser);
    for (board = 0U; board < BMS_BOARD_COUNT; ++board) {
        for (group = 0U; group < 6U; ++group) {
            CanMessage message = make_message(
                timestamp++, BMS_PARSER_CELL_VOLTAGE_BASE + group * 0x10U +
                board);

            for (cell = 0U; cell < 4U; ++cell) {
                unsigned int index = group * 4U + cell;
                put_u16_le(&message, cell * 2U,
                            (uint16_t)(30000U + board * 1000U + index));
            }
            TEST_CHECK(bms_parser_process(&parser, &message));
        }
    }
    for (board = 0U; board < BMS_BOARD_COUNT; ++board) {
        for (cell = 0U; cell < BMS_CELLS_PER_BOARD; ++cell) {
            float expected = (float)(30000U + board * 1000U + cell) *
                             0.0001f;
            TEST_CHECK(parser.state.board[board].cell_valid[cell]);
            TEST_CHECK(fabsf(parser.state.board[board].cell_voltage_v[cell] -
                             expected) < 0.00001f);
        }
    }
    TEST_CHECK(parser.state.board[0].cell_voltage_v[23] !=
               parser.state.board[1].cell_voltage_v[0]);
    TEST_CHECK(parser.state.board[1].cell_voltage_v[23] !=
               parser.state.board[2].cell_voltage_v[0]);
    TEST_CHECK(parser.state.board[2].cell_voltage_v[23] !=
               parser.state.board[3].cell_voltage_v[0]);
}

static void test_temperatures_and_meter(void)
{
    BmsParser parser;
    CanMessage message;

    bms_parser_init(&parser);
    message = make_message(2000000U, 0x200U);
    message.data[0] = 82U;
    message.data[7] = 107U;
    TEST_CHECK(bms_parser_process(&parser, &message));
    TEST_CHECK(parser.state.board[0].temperature_valid[0]);
    TEST_CHECK(parser.state.board[0].temperature_valid[7]);
    TEST_CHECK(parser.state.board[0].temperature_c[0] == 32.0f);
    TEST_CHECK(parser.state.board[0].temperature_c[7] == 57.0f);

    message = make_message(2000020U, BMS_PARSER_ID_KPE_UI);
    put_u16_le(&message, 0U, 1000U); /* HV2 */
    put_u16_le(&message, 2U, 1100U); /* HV1 */
    put_u16_le(&message, 4U, 1200U); /* HV3 */
    put_u16_le(&message, 6U, 32500U); /* -0 A after offset */
    TEST_CHECK(bms_parser_process(&parser, &message));
    TEST_CHECK(fabsf(parser.state.hv1_v - 165.0f) < 0.001f);
    TEST_CHECK(fabsf(parser.state.hv2_v - 150.0f) < 0.001f);
    TEST_CHECK(fabsf(parser.state.hv3_v - 180.0f) < 0.001f);
    TEST_CHECK(fabsf(parser.state.current_a) < 0.001f);

    message = make_message(2000040U, BMS_PARSER_ID_KPE_AP);
    put_u16_le(&message, 0U, 0x1111U); /* AH */
    put_u16_le(&message, 2U, 0x2222U); /* PH */
    put_u16_le(&message, 4U, 2500U); /* HV4 */
    put_u16_le(&message, 6U, 2600U); /* HV5 */
    TEST_CHECK(bms_parser_process(&parser, &message));
    TEST_CHECK(fabsf(parser.state.hv4_v - 375.0f) < 0.001f);
    TEST_CHECK(fabsf(parser.state.hv5_v - 390.0f) < 0.001f);
}

static void test_fault_mapping_and_edges(void)
{
    BmsParser parser;
    CanMessage fault30;
    CanMessage fault31;
    CanMessage baseline30;
    CanMessage baseline31;
    unsigned int events_after_fault;

    bms_parser_init(&parser);
    fault30 = make_message(3000000U, 0x301U);
    fault31 = make_message(3000010U, 0x311U);
    /* Voltage V01/V12/V13/V24 and low/high-side/LTC faults. */
    set_bit(&fault30, 0U);
    set_bit(&fault30, 1U);
    set_bit(&fault30, 12U);
    set_bit(&fault30, 13U);
    set_bit(&fault30, 14U);
    set_bit(&fault30, 15U);
    set_bit(&fault30, 16U);
    set_bit(&fault30, 17U);
    set_bit(&fault30, 28U);
    set_bit(&fault30, 29U);
    /* T01/T24 OPEN and T01/T08 SHORT. */
    set_bit(&fault30, 32U);
    set_bit(&fault30, 55U);
    set_bit(&fault30, 56U);
    set_bit(&fault30, 63U);
    /* T09/T24 SHORT and Balance 01/24 OPEN/CLOSED. */
    set_bit(&fault31, 0U);
    set_bit(&fault31, 15U);
    set_bit(&fault31, 16U);
    set_bit(&fault31, 39U);
    set_bit(&fault31, 40U);
    set_bit(&fault31, 63U);

    /* A normal frame establishes the zero baseline without an event. */
    baseline30 = make_message(2999990U, 0x301U);
    baseline31 = make_message(2999991U, 0x311U);
    TEST_CHECK(bms_parser_process(&parser, &baseline30));
    TEST_CHECK(bms_parser_process(&parser, &baseline31));
    TEST_CHECK(parser.state.fault_event_count == 0U);
    TEST_CHECK(bms_parser_process(&parser, &fault30));
    TEST_CHECK(bms_parser_process(&parser, &fault31));
    events_after_fault = parser.state.fault_event_count;
    TEST_CHECK(events_after_fault == 20U);
    TEST_CHECK(parser.state.board[1].voltage_open[0]);
    TEST_CHECK(parser.state.board[1].voltage_open[11]);
    TEST_CHECK(parser.state.board[1].voltage_open[12]);
    TEST_CHECK(parser.state.board[1].voltage_open[23]);
    TEST_CHECK(parser.state.board[1].temp_open[0]);
    TEST_CHECK(parser.state.board[1].temp_open[23]);
    TEST_CHECK(parser.state.board[1].temp_short[0]);
    TEST_CHECK(parser.state.board[1].temp_short[7]);
    TEST_CHECK(parser.state.board[1].temp_short[8]);
    TEST_CHECK(parser.state.board[1].temp_short[23]);
    TEST_CHECK(parser.state.board[1].balance_stuck_open[0]);
    TEST_CHECK(parser.state.board[1].balance_stuck_open[23]);
    TEST_CHECK(parser.state.board[1].balance_stuck_closed[0]);
    TEST_CHECK(parser.state.board[1].balance_stuck_closed[23]);
    TEST_CHECK(parser.state.board[1].fault_30_raw != 0U);
    TEST_CHECK(parser.state.board[1].fault_31_raw != 0U);

    /* Repeating a fault frame does not spam the ring. */
    fault30.timestamp_us++;
    fault31.timestamp_us++;
    TEST_CHECK(bms_parser_process(&parser, &fault30));
    TEST_CHECK(bms_parser_process(&parser, &fault31));
    TEST_CHECK(parser.state.fault_event_count == events_after_fault);

    /* Zero frames create exactly one RECOVERED edge for every active fault. */
    memset(fault30.data, 0, sizeof(fault30.data));
    memset(fault31.data, 0, sizeof(fault31.data));
    fault30.timestamp_us++;
    fault31.timestamp_us++;
    TEST_CHECK(bms_parser_process(&parser, &fault30));
    TEST_CHECK(bms_parser_process(&parser, &fault31));
    TEST_CHECK(parser.state.fault_event_count == events_after_fault * 2U);
    TEST_CHECK(!parser.state.board[1].voltage_open[23]);
    TEST_CHECK(!parser.state.board[1].temp_short[23]);
}

static void test_fault_ring_capacity(void)
{
    BmsParser parser;
    unsigned int index;

    bms_parser_init(&parser);
    for (index = 0U; index < BMS_FAULT_EVENT_CAPACITY + 2U; ++index) {
        CanMessage message = make_message(5000000U + index, 0x300U);

        if ((index & 1U) != 0U) {
            set_bit(&message, 1U);
        }
        TEST_CHECK(bms_parser_process(&parser, &message));
    }
    TEST_CHECK(parser.state.fault_event_count == BMS_FAULT_EVENT_CAPACITY);
    TEST_CHECK(parser.state.fault_event_sequence ==
               BMS_FAULT_EVENT_CAPACITY + 1U);
    TEST_CHECK(parser.state.fault_events[(parser.state.fault_event_head +
                                          BMS_FAULT_EVENT_CAPACITY - 1U) %
                                         BMS_FAULT_EVENT_CAPACITY].active);
}

static void test_out_of_order_and_future_frames(void)
{
    BmsParser parser;
    CanMessage newer;
    CanMessage older;

    bms_parser_init(&parser);
    newer = make_message(6000000U, 0x100U);
    put_u16_le(&newer, 0U, 40000U);
    older = make_message(5999999U, 0x100U);
    put_u16_le(&older, 0U, 10000U);
    TEST_CHECK(bms_parser_process(&parser, &newer));
    TEST_CHECK(bms_parser_process(&parser, &older));
    TEST_CHECK(parser.state.board[0].cell_voltage_v[0] == 4.0f);
    TEST_CHECK(parser.state.timestamp_mono_us == 6000000U);

    /* A timestamp from the future is not considered live until wall-clock
     * monotonic time reaches it. */
    bms_parser_expire_stale(&parser, 5999999U);
    TEST_CHECK(!parser.state.board[0].valid);
    bms_parser_expire_stale(&parser, 6000000U);
    TEST_CHECK(parser.state.board[0].valid);
}

static void test_reject_and_stale(void)
{
    BmsParser parser;
    CanMessage message = make_message(4000000U, 0x100U);
    BmsState snapshot;

    bms_parser_init(&parser);
    message.dlc = 7U;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.dlc = 8U;
    message.flags = CAN_MESSAGE_FLAG_EXTENDED;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.flags = CAN_MESSAGE_FLAG_REMOTE;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.flags = CAN_MESSAGE_FLAG_STANDARD | CAN_MESSAGE_FLAG_ERROR;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.flags = 0U;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.bus = 0U;
    message.flags = CAN_MESSAGE_FLAG_STANDARD;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.bus = BMS_PARSER_BUS;
    message.flags = CAN_MESSAGE_FLAG_STANDARD;
    message.id = 0x104U;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.id = 0x777U;
    TEST_CHECK(!bms_parser_process(&parser, &message));
    message.id = 0x100U;
    TEST_CHECK(bms_parser_process(&parser, &message));
    bms_parser_expire_stale(&parser, 4000000U + BMS_STALE_TIMEOUT_US + 1U);
    TEST_CHECK(!parser.state.board[0].valid);
    TEST_CHECK(!parser.state.board[0].cell_valid[0]);
    TEST_CHECK(!parser.state.valid);
    bms_parser_snapshot(&parser, 5000002U, &snapshot);
    TEST_CHECK(!snapshot.valid);
}

int main(void)
{
    test_all_cell_voltages();
    test_temperatures_and_meter();
    test_fault_mapping_and_edges();
    test_fault_ring_capacity();
    test_out_of_order_and_future_frames();
    test_reject_and_stale();
    return 0;
}
