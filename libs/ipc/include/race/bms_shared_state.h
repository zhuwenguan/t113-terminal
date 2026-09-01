#ifndef RACE_BMS_SHARED_STATE_H
#define RACE_BMS_SHARED_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include <race/bms_state.h>

#define RACE_BMS_SHARED_STATE_NAME "/race_bms_state"
#define RACE_BMS_SHM_MAGIC UINT32_C(0x424d5353)
#define RACE_BMS_SHM_VERSION 1U
#define RACE_BMS_SHARED_READ_RETRIES 1000U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t state_size;
    _Atomic uint32_t seq;
    uint32_t flags;
    uint64_t publish_mono_us;
    BmsState state;
} RaceSharedBmsState;

typedef struct {
    int fd;
    RaceSharedBmsState *mapping;
    bool writer;
} RaceSharedBmsStateHandle;

int race_shared_bms_state_writer_open(RaceSharedBmsStateHandle *handle,
                                      const char *name);
int race_shared_bms_state_reader_open(RaceSharedBmsStateHandle *handle,
                                      const char *name);
int race_shared_bms_state_validate(const RaceSharedBmsState *mapping);
int race_shared_bms_state_publish(RaceSharedBmsStateHandle *handle,
                                  const BmsState *state,
                                  uint64_t published_mono_us);
int race_shared_bms_state_read(const RaceSharedBmsStateHandle *handle,
                               BmsState *state, uint64_t *published_mono_us,
                               uint32_t *sequence);
bool race_shared_bms_state_is_stale(uint64_t now_mono_us,
                                    uint64_t published_mono_us,
                                    uint64_t threshold_us);
void race_shared_bms_state_close(RaceSharedBmsStateHandle *handle);

#endif
