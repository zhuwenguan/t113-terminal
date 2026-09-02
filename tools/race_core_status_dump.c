#include <race/core_status.h>
#include <race/time_utils.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    RaceCoreStatusHandle handle;
    RaceCoreStatusData data;
    uint64_t now_us = 0U;
    const char *state;

    if (race_core_status_reader_open(&handle, NULL) < 0 ||
        race_core_status_read(&handle, &data) < 0 ||
        race_monotonic_us(&now_us) < 0) {
        fprintf(stderr, "core status unavailable: %s\n", strerror(errno));
        race_core_status_close(&handle);
        return 1;
    }
    state = race_core_lifecycle_name((RaceCoreLifecycleState)data.state);
    printf("state=%s flags=0x%x can_mask=0x%x logger=%s hmi=%s lappro=%s "
           "stale=%s heartbeat_mono_us=%llu session=%s\n",
           state, data.flags, data.can_ready_mask,
           (data.flags & RACE_CORE_STATUS_LOGGER_READY) != 0U ? "ready" : "down",
           (data.flags & RACE_CORE_STATUS_HMI_READY) != 0U ? "ready" : "down",
           (data.flags & RACE_CORE_STATUS_LAPPRO_READY) != 0U ? "ready" : "down",
           race_core_status_is_stale(now_us, data.heartbeat_mono_us,
                                     RACE_CORE_STATUS_STALE_US) ? "yes" : "no",
           (unsigned long long)data.heartbeat_mono_us,
           data.session_directory);
    race_core_status_close(&handle);
    return 0;
}
