#include "webd_json.h"

#include "test_check.h"

#include <stdio.h>
#include <string.h>

static unsigned int count_text(const char *text, const char *needle)
{
    unsigned int count = 0U;
    size_t length = strlen(needle);

    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += length;
    }
    return count;
}

int main(void)
{
    BmsStateSnapshot snapshot;
    char json[65536];
    int length;
    unsigned int board;
    unsigned int cell;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.available = true;
    snapshot.fresh = true;
    snapshot.sequence = 42U;
    snapshot.age_us = 20000U;
    snapshot.state.timestamp_mono_us = 1000000U;
    snapshot.state.valid = true;
    snapshot.state.meter_ui_valid = true;
    snapshot.state.meter_ap_valid = true;
    snapshot.state.meter_valid = true;
    snapshot.state.hv1_v = 386.4f;
    snapshot.state.hv2_v = 386.1f;
    snapshot.state.hv3_v = 386.2f;
    snapshot.state.hv4_v = 387.2f;
    snapshot.state.hv5_v = 387.0f;
    snapshot.state.current_a = -21.62f;
    for (board = 0U; board < BMS_BOARD_COUNT; ++board) {
        snapshot.state.board[board].valid = true;
        snapshot.state.board[board].fault_valid = true;
        for (cell = 0U; cell < BMS_CELLS_PER_BOARD; ++cell) {
            snapshot.state.board[board].cell_valid[cell] = true;
            snapshot.state.board[board].cell_voltage_v[cell] = 3.9f;
        }
        for (cell = 0U; cell < BMS_TEMPS_PER_BOARD; ++cell) {
            snapshot.state.board[board].temperature_valid[cell] = true;
            snapshot.state.board[board].temperature_c[cell] = 32.0f;
        }
    }
    snapshot.state.fault_event_count = 1U;
    snapshot.state.fault_event_head = 1U;
    snapshot.state.fault_events[0].timestamp_mono_us = 1000000U;
    snapshot.state.fault_events[0].board = 1U;
    snapshot.state.fault_events[0].channel = 12U;
    snapshot.state.fault_events[0].type = BMS_FAULT_CELL_VOLTAGE_OPEN;
    snapshot.state.fault_events[0].active = true;
    length = webd_json_bms(json, sizeof(json), &snapshot);
    TEST_CHECK(length > 0);
    TEST_CHECK(strstr(json, "\"hv1_v\":386.4000") != NULL);
    TEST_CHECK(strstr(json, "\"hv4_v\":387.2000") != NULL);
    TEST_CHECK(count_text(json, "\"id\":") == BMS_BOARD_COUNT);
    TEST_CHECK(count_text(json, "\"cells_v\":") == BMS_BOARD_COUNT);
    TEST_CHECK(count_text(json, "\"temps_c\":") == BMS_BOARD_COUNT);
    TEST_CHECK(strstr(json, "\"global_cell\":37") != NULL);
    TEST_CHECK(strstr(json, "cell_voltage_open") != NULL);

    /* A stale source keeps diagnostic arrays/history but never exposes a
     * stale measurement as numeric zero or as a live value. */
    snapshot.fresh = false;
    snapshot.state.hv1_v = 0.0f;
    snapshot.state.board[0].cell_voltage_v[0] = 0.0f;
    length = webd_json_bms(json, sizeof(json), &snapshot);
    TEST_CHECK(length > 0);
    TEST_CHECK(strstr(json, "\"status\":\"STALE\"") != NULL);
    TEST_CHECK(strstr(json, "\"hv1_v\":null") != NULL);
    TEST_CHECK(strstr(json, "\"cells_v\":[null") != NULL);
    return 0;
}
