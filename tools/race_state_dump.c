#include <race/shared_state.h>
#include <race/time_utils.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    RaceSharedStateHandle handle;
    VehicleState state;
    uint64_t published, now;
    uint32_t seq;
    if (race_shared_state_reader_open(&handle, NULL) < 0) {
        fprintf(stderr, "open %s: %s\n", RACE_SHARED_STATE_NAME, strerror(errno)); return 1;
    }
    if (race_shared_state_read(&handle, &state, &published, &seq) < 0 || race_monotonic_us(&now) < 0) {
        fprintf(stderr, "read: %s\n", strerror(errno)); race_shared_state_close(&handle); return 1;
    }
    printf("seq=%u publish_mono_us=%llu age_us=%llu stale=%s speed=%.3f rpm=%.3f torque=%.3f voltage=%.3f current=%.3f lap=%u delta=%.3f\n",
           seq, (unsigned long long)published,
           (unsigned long long)(now >= published ? now - published : 0U),
           race_shared_state_is_stale(now, published, RACE_STATE_STALE_US) ? "yes" : "no",
           state.speed_kmh, state.motor_rpm, state.torque_nm, state.hv_voltage_v,
           state.hv_current_a, state.lap_number, state.delta_time_s);
    race_shared_state_close(&handle); return 0;
}
