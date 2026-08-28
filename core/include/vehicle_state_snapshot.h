#ifndef T113_VEHICLE_STATE_SNAPSHOT_H
#define T113_VEHICLE_STATE_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <pthread.h>

#include <race/vehicle_state.h>

#define VEHICLE_STATE_SNAPSHOT_HZ 10U
#define VEHICLE_STATE_SNAPSHOT_PERIOD_US 100000ULL

typedef bool (*VehicleStateSnapshotProvider)(uint64_t timestamp_us,
                                              VehicleState *state,
                                              void *context);
typedef void (*VehicleStateSnapshotSink)(uint64_t timestamp_us,
                                         const VehicleState *state,
                                         void *context);

typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    VehicleStateSnapshotProvider provider;
    VehicleStateSnapshotSink sink;
    void *context;
    bool initialized;
    bool started;
    bool stop_requested;
} VehicleStateSnapshotter;

/* Runs the live snapshot clock using absolute CLOCK_MONOTONIC deadlines. */
int vehicle_state_snapshotter_init(VehicleStateSnapshotter *snapshotter,
                                    VehicleStateSnapshotProvider provider,
                                    VehicleStateSnapshotSink sink,
                                    void *context);
int vehicle_state_snapshotter_start(VehicleStateSnapshotter *snapshotter);
void vehicle_state_snapshotter_stop(VehicleStateSnapshotter *snapshotter);

/* Replay uses the same period, but advances this schedule from source time. */
typedef struct {
    uint64_t next_timestamp_us;
    bool initialized;
} VehicleStateSnapshotSchedule;

void vehicle_state_snapshot_schedule_init(VehicleStateSnapshotSchedule *schedule,
                                           uint64_t first_timestamp_us);
bool vehicle_state_snapshot_schedule_next_due(
    VehicleStateSnapshotSchedule *schedule, uint64_t up_to_timestamp_us,
    bool include_current, uint64_t *snapshot_timestamp_us);

#endif
