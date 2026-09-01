#ifndef RACE_BMS_STATE_H
#define RACE_BMS_STATE_H

#include <stdbool.h>
#include <stdint.h>

#define BMS_BOARD_COUNT 4U
#define BMS_CELLS_PER_BOARD 24U
#define BMS_TEMPS_PER_BOARD 8U
#define BMS_FAULT_TEMP_CHANNELS 24U
#define BMS_FAULT_EVENT_CAPACITY 128U
#define BMS_STALE_TIMEOUT_US UINT64_C(1000000)

typedef enum {
    BMS_FAULT_CELL_VOLTAGE_OPEN = 0,
    BMS_FAULT_TEMP_OPEN,
    BMS_FAULT_TEMP_SHORT,
    BMS_FAULT_BALANCE_STUCK_OPEN,
    BMS_FAULT_BALANCE_STUCK_CLOSED,
    BMS_FAULT_LOW_NEGATIVE_VOLTAGE_OPEN,
    BMS_FAULT_LOW_POSITIVE_VOLTAGE,
    BMS_FAULT_HIGH_NEGATIVE_VOLTAGE_OPEN,
    BMS_FAULT_HIGH_POSITIVE_VOLTAGE,
    BMS_FAULT_LTC6804_LOW_COMM,
    BMS_FAULT_LTC6804_HIGH_COMM,
    BMS_FAULT_TYPE_COUNT
} BmsFaultType;

typedef struct {
    uint64_t timestamp_mono_us;
    uint32_t sequence;
    uint8_t board;
    uint8_t channel;
    BmsFaultType type;
    bool active;
} BmsFaultEvent;

typedef struct {
    float cell_voltage_v[BMS_CELLS_PER_BOARD];
    float temperature_c[BMS_TEMPS_PER_BOARD];
    bool cell_valid[BMS_CELLS_PER_BOARD];
    bool temperature_valid[BMS_TEMPS_PER_BOARD];

    bool voltage_open[BMS_CELLS_PER_BOARD];
    bool temp_open[BMS_FAULT_TEMP_CHANNELS];
    bool temp_short[BMS_FAULT_TEMP_CHANNELS];
    bool balance_stuck_open[BMS_CELLS_PER_BOARD];
    bool balance_stuck_closed[BMS_CELLS_PER_BOARD];

    bool low_negative_voltage_open;
    bool low_positive_voltage_fault;
    bool high_negative_voltage_open;
    bool high_positive_voltage_fault;
    bool ltc6804_low_comm_fault;
    bool ltc6804_high_comm_fault;

    uint64_t fault_30_raw;
    uint64_t fault_31_raw;
    uint64_t fault_30_update_mono_us;
    uint64_t fault_31_update_mono_us;
    uint64_t last_update_mono_us;
    uint64_t last_fault_update_mono_us;
    uint64_t cell_update_mono_us[BMS_CELLS_PER_BOARD];
    uint64_t temperature_update_mono_us[BMS_TEMPS_PER_BOARD];
    bool fault_valid;
    bool valid;
} BmsBoardState;

typedef struct {
    BmsBoardState board[BMS_BOARD_COUNT];

    float hv1_v;
    float hv2_v;
    float hv3_v;
    float hv4_v;
    float hv5_v;
    float current_a;
    uint64_t meter_last_update_mono_us;
    uint64_t meter_ui_update_mono_us;
    uint64_t meter_ap_update_mono_us;
    bool meter_ui_valid;
    bool meter_ap_valid;
    bool meter_valid;

    uint64_t timestamp_mono_us;
    bool valid;
    BmsFaultEvent fault_events[BMS_FAULT_EVENT_CAPACITY];
    uint32_t fault_event_head;
    uint32_t fault_event_count;
    uint32_t fault_event_sequence;
} BmsState;

static inline const char *bms_fault_type_name(BmsFaultType type)
{
    switch (type) {
    case BMS_FAULT_CELL_VOLTAGE_OPEN:
        return "cell_voltage_open";
    case BMS_FAULT_TEMP_OPEN:
        return "temperature_open";
    case BMS_FAULT_TEMP_SHORT:
        return "temperature_short";
    case BMS_FAULT_BALANCE_STUCK_OPEN:
        return "balance_stuck_open";
    case BMS_FAULT_BALANCE_STUCK_CLOSED:
        return "balance_stuck_closed";
    case BMS_FAULT_LOW_NEGATIVE_VOLTAGE_OPEN:
        return "low_negative_voltage_open";
    case BMS_FAULT_LOW_POSITIVE_VOLTAGE:
        return "low_positive_voltage_fault";
    case BMS_FAULT_HIGH_NEGATIVE_VOLTAGE_OPEN:
        return "high_negative_voltage_open";
    case BMS_FAULT_HIGH_POSITIVE_VOLTAGE:
        return "high_positive_voltage_fault";
    case BMS_FAULT_LTC6804_LOW_COMM:
        return "ltc6804_low_communication";
    case BMS_FAULT_LTC6804_HIGH_COMM:
        return "ltc6804_high_communication";
    default:
        return "unknown";
    }
}

#endif
