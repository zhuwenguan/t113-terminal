#ifndef T113_REPLAY_H
#define T113_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "can_receiver.h"
#include "lappro.h"
#include "vehicle_state.h"

typedef enum {
    REPLAY_CAN0 = 0,
    REPLAY_CAN1,
    REPLAY_LAPPRO
} ReplaySource;

typedef struct {
    uint64_t timestamp_us;
    ReplaySource source;
    union {
        CanMessage can;
        LapProState lappro;
    } payload;
} ReplayEvent;

typedef void (*ReplayCanSink)(const CanMessage *message, void *context);
typedef void (*ReplayLapProSink)(uint64_t timestamp_us,
                                 const LapProState *state, void *context);
typedef void (*ReplayVehicleStateSnapshotSink)(uint64_t timestamp_us,
                                               void *context);

typedef struct {
    const char *directory;
    double speed;
    ReplayCanSink can_sink;
    ReplayLapProSink lappro_sink;
    void *sink_context;
    const VehicleState *vehicle_state;
    ReplayVehicleStateSnapshotSink vehicle_state_snapshot_sink;
} ReplayConfig;

/* Runs a merged, timestamp-ordered replay until the input is exhausted. */
int replay_run(const ReplayConfig *config);

#endif
