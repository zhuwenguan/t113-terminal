#ifndef RACE_HEALTH_H
#define RACE_HEALTH_H
#include <stdint.h>
typedef enum { RACE_HEALTH_UNKNOWN = 0, RACE_HEALTH_STARTING,
    RACE_HEALTH_READY, RACE_HEALTH_DEGRADED, RACE_HEALTH_STOPPING } RaceHealthState;
typedef struct { uint32_t version; RaceHealthState state;
    uint64_t heartbeat_mono_us; uint32_t error_flags; } RaceHealth;
#endif
