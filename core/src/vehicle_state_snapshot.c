#define _POSIX_C_SOURCE 200809L

#include "vehicle_state_snapshot.h"
#include "console_log.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static bool snapshot_monotonic_us(uint64_t *timestamp_us)
{
    struct timespec now;

    if (timestamp_us == NULL || clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return false;
    }
    *timestamp_us = (uint64_t)now.tv_sec * 1000000ULL +
                    (uint64_t)now.tv_nsec / 1000ULL;
    return true;
}

static void snapshot_add_period(struct timespec *timestamp)
{
    timestamp->tv_nsec +=
        (long)(VEHICLE_STATE_SNAPSHOT_PERIOD_US % 1000000ULL) * 1000L;
    timestamp->tv_sec +=
        (time_t)(VEHICLE_STATE_SNAPSHOT_PERIOD_US / 1000000ULL);
    if (timestamp->tv_nsec >= 1000000000L) {
        timestamp->tv_sec++;
        timestamp->tv_nsec -= 1000000000L;
    }
}

static bool snapshot_deadline_is_due(const struct timespec *deadline,
                                     const struct timespec *now)
{
    return deadline->tv_sec < now->tv_sec ||
           (deadline->tv_sec == now->tv_sec &&
            deadline->tv_nsec <= now->tv_nsec);
}

static bool snapshot_stop_requested(VehicleStateSnapshotter *snapshotter)
{
    bool stop_requested;

    if (pthread_mutex_lock(&snapshotter->mutex) != 0) {
        return true;
    }
    stop_requested = snapshotter->stop_requested;
    pthread_mutex_unlock(&snapshotter->mutex);
    return stop_requested;
}

static void *vehicle_state_snapshot_thread(void *context)
{
    VehicleStateSnapshotter *snapshotter = context;
    struct timespec deadline;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) {
        LOG_ERROR("VehicleState snapshot clock_gettime failed: %s",
                  strerror(errno));
        return NULL;
    }
    snapshot_add_period(&deadline);

    while (!snapshot_stop_requested(snapshotter)) {
        int sleep_result;
        uint64_t timestamp_us;
        VehicleState state;

        do {
            sleep_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                           &deadline, NULL);
        } while (sleep_result == EINTR &&
                 !snapshot_stop_requested(snapshotter));
        if (sleep_result != 0) {
            if (sleep_result != EINTR) {
                LOG_ERROR("VehicleState snapshot clock_nanosleep failed: %s",
                          strerror(sleep_result));
            }
            break;
        }
        if (snapshot_stop_requested(snapshotter)) {
            break;
        }
        if (!snapshot_monotonic_us(&timestamp_us)) {
            LOG_ERROR("VehicleState snapshot timestamp failed: %s",
                      strerror(errno));
            break;
        }

        memset(&state, 0, sizeof(state));
        if (snapshotter->provider(timestamp_us, &state,
                                  snapshotter->context)) {
            snapshotter->sink(timestamp_us, &state, snapshotter->context);
        }

        /* Keep the clock anchored to the original monotonic timeline. If a
         * wake-up was late, skip missed periods instead of emitting a burst. */
        {
            struct timespec now;

            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
                LOG_ERROR("VehicleState snapshot clock_gettime failed: %s",
                          strerror(errno));
                break;
            }
            do {
                snapshot_add_period(&deadline);
            } while (snapshot_deadline_is_due(&deadline, &now));
        }
    }

    return NULL;
}

int vehicle_state_snapshotter_init(VehicleStateSnapshotter *snapshotter,
                                    VehicleStateSnapshotProvider provider,
                                    VehicleStateSnapshotSink sink,
                                    void *context)
{
    int result;

    if (snapshotter == NULL || provider == NULL || sink == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(snapshotter, 0, sizeof(*snapshotter));
    snapshotter->provider = provider;
    snapshotter->sink = sink;
    snapshotter->context = context;
    result = pthread_mutex_init(&snapshotter->mutex, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    snapshotter->initialized = true;
    return 0;
}

int vehicle_state_snapshotter_start(VehicleStateSnapshotter *snapshotter)
{
    int result;

    if (snapshotter == NULL || !snapshotter->initialized ||
        snapshotter->started) {
        errno = EINVAL;
        return -1;
    }
    result = pthread_create(&snapshotter->thread, NULL,
                            vehicle_state_snapshot_thread, snapshotter);
    if (result != 0) {
        errno = result;
        return -1;
    }
    snapshotter->started = true;
    return 0;
}

void vehicle_state_snapshotter_stop(VehicleStateSnapshotter *snapshotter)
{
    if (snapshotter == NULL || !snapshotter->initialized) {
        return;
    }
    if (snapshotter->started) {
        if (pthread_mutex_lock(&snapshotter->mutex) == 0) {
            snapshotter->stop_requested = true;
            pthread_mutex_unlock(&snapshotter->mutex);
        }
        pthread_join(snapshotter->thread, NULL);
        snapshotter->started = false;
    }
    pthread_mutex_destroy(&snapshotter->mutex);
    snapshotter->initialized = false;
}

static bool snapshot_add_period_us(uint64_t timestamp_us,
                                   uint64_t *next_timestamp_us)
{
    if (timestamp_us > UINT64_MAX - VEHICLE_STATE_SNAPSHOT_PERIOD_US) {
        return false;
    }
    *next_timestamp_us = timestamp_us +
                         VEHICLE_STATE_SNAPSHOT_PERIOD_US;
    return true;
}

void vehicle_state_snapshot_schedule_init(VehicleStateSnapshotSchedule *schedule,
                                           uint64_t first_timestamp_us)
{
    if (schedule == NULL) {
        return;
    }
    memset(schedule, 0, sizeof(*schedule));
    schedule->initialized = snapshot_add_period_us(
        first_timestamp_us, &schedule->next_timestamp_us);
}

bool vehicle_state_snapshot_schedule_next_due(
    VehicleStateSnapshotSchedule *schedule, uint64_t up_to_timestamp_us,
    bool include_current, uint64_t *snapshot_timestamp_us)
{
    bool due;

    if (schedule == NULL || snapshot_timestamp_us == NULL ||
        !schedule->initialized) {
        return false;
    }
    due = schedule->next_timestamp_us < up_to_timestamp_us ||
          (include_current && schedule->next_timestamp_us ==
                                  up_to_timestamp_us);
    if (!due) {
        return false;
    }
    *snapshot_timestamp_us = schedule->next_timestamp_us;
    schedule->initialized = snapshot_add_period_us(
        schedule->next_timestamp_us, &schedule->next_timestamp_us);
    return true;
}
