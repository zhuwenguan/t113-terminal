#ifndef T113_RACE_EVENT_H
#define T113_RACE_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include "accel_test.h"
#include "gps_track.h"
#include "lap_timer.h"
#include "lappro.h"
#include "vehicle_state.h"

typedef struct {
    RaceEventMode mode;
    GpsTrack gps_track;
    LapTimer lap_timer;
    AccelTest accel_test;
    size_t loop_history_index;
    size_t loop_current_index;
    uint32_t loop_match_count;
    uint64_t last_report_timestamp_us;
} RaceEvent;

void race_event_init(RaceEvent *event, const char *trace_directory);

void race_event_update(RaceEvent *event, uint64_t timestamp_us,
                       const LapProState *sample, VehicleState *state);

void race_event_close(RaceEvent *event);

#endif
