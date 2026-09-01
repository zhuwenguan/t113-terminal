#include "vehicle_state_clock.h"

#include <string.h>

void vehicle_state_clock_init(VehicleStateClock *clock, bool replay_mode)
{
    if (clock == NULL) {
        return;
    }
    memset(clock, 0, sizeof(*clock));
    clock->replay_mode = replay_mode;
}

void vehicle_state_clock_note_event(VehicleStateClock *clock,
                                    uint64_t timestamp_us)
{
    if (clock == NULL || !clock->replay_mode) {
        return;
    }
    clock->replay_timestamp_us = timestamp_us;
    clock->replay_timestamp_valid = true;
}

void vehicle_state_clock_note_lappro(VehicleStateClock *clock,
                                    uint64_t timestamp_us, bool valid_fix)
{
    if (clock == NULL) {
        return;
    }
    vehicle_state_clock_note_event(clock, timestamp_us);
    clock->lappro_timestamp_valid = valid_fix;
    if (valid_fix) {
        clock->lappro_timestamp_us = timestamp_us;
    }
}

uint64_t vehicle_state_clock_snapshot_timestamp(const VehicleStateClock *clock,
                                                uint64_t live_timestamp_us)
{
    if (clock != NULL && clock->replay_mode &&
        clock->replay_timestamp_valid) {
        return clock->replay_timestamp_us;
    }
    return live_timestamp_us;
}

void vehicle_state_clock_expire_lappro(const VehicleStateClock *clock,
                                      uint64_t timestamp_us,
                                      VehicleState *state)
{
    bool fresh;

    if (state == NULL) {
        return;
    }
    fresh = clock != NULL && clock->lappro_timestamp_valid &&
            timestamp_us >= clock->lappro_timestamp_us &&
            timestamp_us - clock->lappro_timestamp_us <=
                VEHICLE_STATE_LAPPRO_FRESHNESS_TIMEOUT_US;
    if (!fresh) {
        state->valid_mask &= ~VEHICLE_STATE_LAPPRO_REQUIRED_MASK;
    }
}
