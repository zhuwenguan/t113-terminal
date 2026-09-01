#ifndef T113_VEHICLE_STATE_CLOCK_H
#define T113_VEHICLE_STATE_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include <race/vehicle_state.h>

#define VEHICLE_STATE_LAPPRO_FRESHNESS_TIMEOUT_US 1000000ULL

typedef struct {
    bool replay_mode;
    bool replay_timestamp_valid;
    bool lappro_timestamp_valid;
    uint64_t replay_timestamp_us;
    uint64_t lappro_timestamp_us;
} VehicleStateClock;

/*
 * This type does not provide internal synchronization. After initialization,
 * callers that share it across threads must hold the owning VehicleState lock
 * while reading or updating it.
 */

void vehicle_state_clock_init(VehicleStateClock *clock, bool replay_mode);

void vehicle_state_clock_note_event(VehicleStateClock *clock,
                                    uint64_t timestamp_us);

void vehicle_state_clock_note_lappro(VehicleStateClock *clock,
                                    uint64_t timestamp_us, bool valid_fix);

uint64_t vehicle_state_clock_snapshot_timestamp(const VehicleStateClock *clock,
                                                uint64_t live_timestamp_us);

void vehicle_state_clock_expire_lappro(const VehicleStateClock *clock,
                                      uint64_t timestamp_us,
                                      VehicleState *state);

#endif
