#ifndef RACE_EVENT_PROTOCOL_H
#define RACE_EVENT_PROTOCOL_H
#include <stdint.h>
#define RACE_EVENT_PROTOCOL_VERSION 1U
typedef enum { RACE_EVENT_SYSTEM_START = 1, RACE_EVENT_SYSTEM_STOP,
    RACE_EVENT_CORE_STALE, RACE_EVENT_STORAGE_ERROR } RaceEventType;
typedef struct { uint16_t version; uint16_t type; uint32_t payload_size;
    uint64_t monotonic_us; } RaceEventHeader;
#endif
