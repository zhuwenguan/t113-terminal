#ifndef RACE_SHARED_STATE_H
#define RACE_SHARED_STATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <race/vehicle_state.h>
#define RACE_SHARED_STATE_NAME "/race_vehicle_state"
#define RACE_SHM_MAGIC UINT32_C(0x52414345)
#define RACE_SHM_VERSION 3U
#define RACE_STATE_STALE_US UINT64_C(1000000)
#define RACE_SHARED_READ_RETRIES 1000U
typedef struct {
    uint32_t magic; uint16_t version; uint16_t header_size;
    uint32_t total_size; uint32_t state_size; _Atomic uint32_t seq;
    uint32_t flags; uint64_t publish_mono_us; VehicleState state;
} RaceSharedState;
typedef struct { int fd; RaceSharedState *mapping; bool writer; } RaceSharedStateHandle;
int race_shared_state_writer_open(RaceSharedStateHandle *, const char *);
int race_shared_state_reader_open(RaceSharedStateHandle *, const char *);
int race_shared_state_validate(const RaceSharedState *);
int race_shared_state_publish(RaceSharedStateHandle *, const VehicleState *, uint64_t);
int race_shared_state_read(const RaceSharedStateHandle *, VehicleState *, uint64_t *, uint32_t *);
bool race_shared_state_is_stale(uint64_t, uint64_t, uint64_t);
void race_shared_state_close(RaceSharedStateHandle *);
#endif
