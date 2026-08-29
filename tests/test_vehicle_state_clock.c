#include "test_check.h"
#include <string.h>

#include "vehicle_state_clock.h"

static void make_valid_lappro_state(VehicleState *state)
{
    memset(state, 0, sizeof(*state));
    state->valid_mask = VEHICLE_STATE_LAPPRO_REQUIRED_MASK;
}

int main(void)
{
    VehicleStateClock clock;
    VehicleState state;

    vehicle_state_clock_init(&clock, false);
    TEST_CHECK(vehicle_state_clock_snapshot_timestamp(&clock, 9000000ULL) ==
           9000000ULL);
    vehicle_state_clock_note_event(&clock, 1000000ULL);
    TEST_CHECK(vehicle_state_clock_snapshot_timestamp(&clock, 9000000ULL) ==
           9000000ULL);

    make_valid_lappro_state(&state);
    vehicle_state_clock_note_lappro(&clock, 1000000ULL, true);
    vehicle_state_clock_expire_lappro(&clock, 2000000ULL, &state);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_LAPPRO_REQUIRED_MASK) ==
           VEHICLE_STATE_LAPPRO_REQUIRED_MASK);
    vehicle_state_clock_expire_lappro(&clock, 2000001ULL, &state);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_LAPPRO_REQUIRED_MASK) == 0U);

    make_valid_lappro_state(&state);
    vehicle_state_clock_note_lappro(&clock, 3000000ULL, true);
    vehicle_state_clock_expire_lappro(&clock, 2999999ULL, &state);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_LAPPRO_REQUIRED_MASK) == 0U);

    make_valid_lappro_state(&state);
    vehicle_state_clock_note_lappro(&clock, 4000000ULL, false);
    vehicle_state_clock_expire_lappro(&clock, 4000000ULL, &state);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_LAPPRO_REQUIRED_MASK) == 0U);

    vehicle_state_clock_init(&clock, true);
    TEST_CHECK(vehicle_state_clock_snapshot_timestamp(&clock, 9000000ULL) ==
           9000000ULL);
    vehicle_state_clock_note_event(&clock, 1234567ULL);
    TEST_CHECK(vehicle_state_clock_snapshot_timestamp(&clock, 9000000ULL) ==
           1234567ULL);
    vehicle_state_clock_note_lappro(&clock, 2345678ULL, true);
    TEST_CHECK(vehicle_state_clock_snapshot_timestamp(&clock, 9000000ULL) ==
           2345678ULL);
    make_valid_lappro_state(&state);
    vehicle_state_clock_expire_lappro(&clock, 3345678ULL, &state);
    TEST_CHECK((state.valid_mask & VEHICLE_STATE_LAPPRO_REQUIRED_MASK) ==
           VEHICLE_STATE_LAPPRO_REQUIRED_MASK);
    return 0;
}
