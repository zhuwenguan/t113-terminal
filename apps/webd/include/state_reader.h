#ifndef T113_WEBD_STATE_READER_H
#define T113_WEBD_STATE_READER_H

#include <stdbool.h>
#include <stdint.h>

#include <race/shared_state.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef struct {
    VehicleState state;
    uint64_t published_mono_us;
    uint32_t sequence;
    uint64_t age_us;
    bool available;
    bool fresh;
} StateSnapshot;

typedef struct {
    RaceSharedStateHandle shared;
    char shm_name[NAME_MAX];
    uint64_t last_open_attempt_us;
    uint64_t last_published_mono_us;
    bool opened;
} StateReader;

void state_reader_init(StateReader *reader, const char *shm_name);

int state_reader_read(StateReader *reader, uint64_t now_mono_us,
                      StateSnapshot *snapshot);

void state_reader_close(StateReader *reader);

#endif
