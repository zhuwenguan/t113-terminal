#ifndef T113_WEBD_BMS_STATE_READER_H
#define T113_WEBD_BMS_STATE_READER_H

#include <stdbool.h>
#include <stdint.h>

#include <race/bms_shared_state.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef struct {
    BmsState state;
    uint64_t published_mono_us;
    uint32_t sequence;
    uint64_t age_us;
    bool available;
    bool fresh;
} BmsStateSnapshot;

typedef struct {
    RaceSharedBmsStateHandle shared;
    char shm_name[NAME_MAX];
    uint64_t last_open_attempt_us;
    bool opened;
} BmsStateReader;

void bms_state_reader_init(BmsStateReader *reader, const char *shm_name);

int bms_state_reader_read(BmsStateReader *reader, uint64_t now_mono_us,
                          BmsStateSnapshot *snapshot);

void bms_state_reader_close(BmsStateReader *reader);

#endif
