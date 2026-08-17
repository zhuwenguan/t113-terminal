#ifndef T113_VEHICLE_STATE_H
#define T113_VEHICLE_STATE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    VEHICLE_STATE_VALID_SPEED_KMH = 1U << 0,
    VEHICLE_STATE_VALID_HEADING_DEG = 1U << 1,
    VEHICLE_STATE_VALID_HACC_M = 1U << 2,
    VEHICLE_STATE_VALID_ALTITUDE_M = 1U << 3,
    VEHICLE_STATE_VALID_LONGITUDE_DEG = 1U << 4,
    VEHICLE_STATE_VALID_LATITUDE_DEG = 1U << 5,
    VEHICLE_STATE_VALID_ACC_X_MPS2 = 1U << 6,
    VEHICLE_STATE_VALID_ACC_Y_MPS2 = 1U << 7,
    VEHICLE_STATE_VALID_ACC_Z_MPS2 = 1U << 8,
    VEHICLE_STATE_VALID_MOTOR_RPM = 1U << 9,
    VEHICLE_STATE_VALID_TORQUE_NM = 1U << 10,
    VEHICLE_STATE_VALID_HV_VOLTAGE_V = 1U << 11,
    VEHICLE_STATE_VALID_HV_CURRENT_A = 1U << 12,
    VEHICLE_STATE_VALID_HV_POWER_W = 1U << 13,
    VEHICLE_STATE_VALID_HV_SOC_PERCENT = 1U << 14,
    VEHICLE_STATE_VALID_LV_VOLTAGE_V = 1U << 15,
    VEHICLE_STATE_VALID_GPS_FIX_TYPE = 1U << 16,
    VEHICLE_STATE_VALID_GPS_NUM_SV = 1U << 17
};

typedef enum {
    RACE_EVENT_UNKNOWN = 0,
    RACE_EVENT_CIRCUIT_MODE,
    RACE_EVENT_ACCEL_MODE,
    RACE_EVENT_MODE_COUNT
} RaceEventMode;

static inline const char *vehicle_state_event_mode_name(RaceEventMode mode)
{
    switch (mode) {
    case RACE_EVENT_CIRCUIT_MODE:
        return "CIRCUIT_MODE";
    case RACE_EVENT_ACCEL_MODE:
        return "ACCEL_MODE";
    case RACE_EVENT_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

enum {
    VEHICLE_STATE_CAN_REQUIRED_MASK =
        VEHICLE_STATE_VALID_MOTOR_RPM |
        VEHICLE_STATE_VALID_TORQUE_NM |
        VEHICLE_STATE_VALID_HV_VOLTAGE_V |
        VEHICLE_STATE_VALID_HV_CURRENT_A |
        VEHICLE_STATE_VALID_HV_POWER_W,
    VEHICLE_STATE_LAPPRO_REQUIRED_MASK =
        VEHICLE_STATE_VALID_SPEED_KMH |
        VEHICLE_STATE_VALID_HEADING_DEG |
        VEHICLE_STATE_VALID_HACC_M |
        VEHICLE_STATE_VALID_ALTITUDE_M |
        VEHICLE_STATE_VALID_LONGITUDE_DEG |
        VEHICLE_STATE_VALID_LATITUDE_DEG |
        VEHICLE_STATE_VALID_ACC_X_MPS2 |
        VEHICLE_STATE_VALID_ACC_Y_MPS2 |
        VEHICLE_STATE_VALID_ACC_Z_MPS2 |
        VEHICLE_STATE_VALID_GPS_FIX_TYPE |
        VEHICLE_STATE_VALID_GPS_NUM_SV
};

typedef struct {
    uint64_t timestamp_us;
    uint32_t valid_mask;
    RaceEventMode event_mode;
    uint32_t lap_number;
    bool lap_valid;
    double lap_time_s;
    double current_lap_time_s;
    double last_lap_time_s;
    double best_lap_time_s;
    double delta_time_s;
    double estimated_lap_time_s;
    double track_distance_m;
    uint32_t sector_number;
    bool can_valid;
    bool gps_valid;
    uint8_t gps_fix_type;
    uint8_t gps_num_sv;
    double speed_kmh;
    double heading_deg;
    double hacc_m;
    double altitude;
    double longitude;
    double latitude;
    double acc_x;
    double acc_y;
    double acc_z;
    double motor_rpm;
    double torque_nm;
    double hv_voltage_v;
    double hv_current_a;
    double hv_power_w;
    double hv_soc_percent;
    double lv_voltage_v;
    double accel_distance_m;
    double accel_time_s;
    double max_acceleration;
    uint8_t reserved[32];
} VehicleState;

#endif
