#ifndef RACE_VEHICLE_STATE_H
#define RACE_VEHICLE_STATE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    VEHICLE_STATE_VALID_SPEED_KMH = 1U << 0, VEHICLE_STATE_VALID_HEADING_DEG = 1U << 1,
    VEHICLE_STATE_VALID_LONGITUDE_DEG = 1U << 4, VEHICLE_STATE_VALID_LATITUDE_DEG = 1U << 5,
    VEHICLE_STATE_VALID_ACC_X_MPS2 = 1U << 6, VEHICLE_STATE_VALID_ACC_Y_MPS2 = 1U << 7,
    VEHICLE_STATE_VALID_ACC_Z_MPS2 = 1U << 8, VEHICLE_STATE_VALID_MOTOR_RPM = 1U << 9,
    VEHICLE_STATE_VALID_TORQUE_NM = 1U << 10, VEHICLE_STATE_VALID_HV_VOLTAGE_V = 1U << 11,
    VEHICLE_STATE_VALID_HV_CURRENT_A = 1U << 12, VEHICLE_STATE_VALID_HV_POWER_W = 1U << 13,
    VEHICLE_STATE_VALID_HV_SOC_PERCENT = 1U << 14, VEHICLE_STATE_VALID_LV_VOLTAGE_V = 1U << 15,
    VEHICLE_STATE_VALID_GPS_FIX_TYPE = 1U << 16, VEHICLE_STATE_VALID_GPS_NUM_SV = 1U << 17,
    VEHICLE_STATE_VALID_THROTTLE_PERCENT = 1U << 18, VEHICLE_STATE_VALID_BRAKE_PERCENT = 1U << 19,
    VEHICLE_STATE_VALID_DRIVE_STATE = 1U << 20,
    VEHICLE_STATE_VALID_MOTOR_TEMPERATURE_C = 1U << 21,
    VEHICLE_STATE_VALID_CONTROLLER_TEMPERATURE_C = 1U << 22,
    VEHICLE_STATE_VALID_BATTERY_TEMPERATURE_C = 1U << 23,
    VEHICLE_STATE_VALID_MODE = 1U << 24,
    VEHICLE_STATE_VALID_HV_DISCHARGE_AH = 1U << 25,
    VEHICLE_STATE_VALID_HV_SOFTWARE_DISCHARGE_AH = 1U << 26
};
typedef enum { VEHICLE_DRIVE_STATE_IDLE = 0, VEHICLE_DRIVE_STATE_READY = 1 }
    VehicleDriveState;
static inline const char *vehicle_drive_state_name(VehicleDriveState state)
{
    return state == VEHICLE_DRIVE_STATE_READY ? "READY" :
           state == VEHICLE_DRIVE_STATE_IDLE ? "IDLE" : "UNKNOWN";
}
typedef enum { RACE_EVENT_UNKNOWN = 0, RACE_EVENT_CIRCUIT_MODE,
    RACE_EVENT_MODE_COUNT } RaceEventMode;
static inline const char *vehicle_state_event_mode_name(RaceEventMode mode)
{ return mode == RACE_EVENT_CIRCUIT_MODE ? "CIRCUIT_MODE" : "UNKNOWN"; }
enum {
    VEHICLE_STATE_CAN_REQUIRED_MASK = VEHICLE_STATE_VALID_MOTOR_RPM |
        VEHICLE_STATE_VALID_TORQUE_NM | VEHICLE_STATE_VALID_HV_VOLTAGE_V |
        VEHICLE_STATE_VALID_HV_CURRENT_A | VEHICLE_STATE_VALID_HV_POWER_W |
        VEHICLE_STATE_VALID_MOTOR_TEMPERATURE_C |
        VEHICLE_STATE_VALID_CONTROLLER_TEMPERATURE_C |
        VEHICLE_STATE_VALID_BATTERY_TEMPERATURE_C |
        VEHICLE_STATE_VALID_THROTTLE_PERCENT |
        VEHICLE_STATE_VALID_BRAKE_PERCENT |
        VEHICLE_STATE_VALID_DRIVE_STATE |
        VEHICLE_STATE_VALID_MODE,
    VEHICLE_STATE_LAPPRO_REQUIRED_MASK = VEHICLE_STATE_VALID_SPEED_KMH |
        VEHICLE_STATE_VALID_HEADING_DEG | VEHICLE_STATE_VALID_LONGITUDE_DEG |
        VEHICLE_STATE_VALID_LATITUDE_DEG | VEHICLE_STATE_VALID_ACC_X_MPS2 |
        VEHICLE_STATE_VALID_ACC_Y_MPS2 | VEHICLE_STATE_VALID_ACC_Z_MPS2 |
        VEHICLE_STATE_VALID_GPS_FIX_TYPE | VEHICLE_STATE_VALID_GPS_NUM_SV
};
typedef struct {
    uint64_t timestamp_us; uint32_t valid_mask; RaceEventMode event_mode;
    uint32_t lap_number; bool lap_valid; double lap_time_s;
    double last_lap_time_s; double best_lap_time_s; double delta_time_s;
    double track_distance_m; uint32_t sector_number; bool can_valid;
    bool gps_valid; uint8_t gps_fix_type; uint8_t gps_num_sv;
    double speed_kmh; double heading_deg; double longitude; double latitude;
    double acc_x; double acc_y; double acc_z; double motor_rpm;
    double torque_nm; double motor_temperature_c;
    double controller_temperature_c; double battery_temperature_c;
    double hv_voltage_v; double hv_current_a;
    double hv_power_w; double hv_soc_percent; double hv_discharge_ah;
    double hv_software_discharge_ah; double lv_voltage_v;
    double accel_distance_m; double accel_50m_time_s; double accel_time_s;
    double max_acceleration; double throttle_percent; double brake_percent;
    VehicleDriveState drive_state; uint32_t mode; bool hv_soc_valid;
    uint8_t reserved[32];
} VehicleState;
#endif
