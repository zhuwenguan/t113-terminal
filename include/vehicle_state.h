#ifndef T113_VEHICLE_STATE_H
#define T113_VEHICLE_STATE_H

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
    uint8_t reserved[32];
} VehicleState;

#endif
