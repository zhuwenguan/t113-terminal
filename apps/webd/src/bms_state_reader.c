#define _POSIX_C_SOURCE 200809L

#include "bms_state_reader.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum {
    BMS_STATE_READER_RETRY_INTERVAL_US = 250000U,
    BMS_STATE_READER_REOPEN_AFTER_US = 2000000U
};

static int bms_state_reader_try_open(BmsStateReader *reader,
                                     uint64_t now_mono_us)
{
    if (reader == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (reader->opened) {
        return 0;
    }
    if (reader->last_open_attempt_us != 0U &&
        now_mono_us >= reader->last_open_attempt_us &&
        now_mono_us - reader->last_open_attempt_us <
            BMS_STATE_READER_RETRY_INTERVAL_US) {
        errno = ENOENT;
        return -1;
    }
    reader->last_open_attempt_us = now_mono_us;
    if (race_shared_bms_state_reader_open(&reader->shared,
                                          reader->shm_name) < 0) {
        return -1;
    }
    reader->opened = true;
    return 0;
}

void bms_state_reader_init(BmsStateReader *reader, const char *shm_name)
{
    if (reader == NULL) {
        return;
    }
    memset(reader, 0, sizeof(*reader));
    reader->shared.fd = -1;
    if (shm_name == NULL || shm_name[0] == '\0') {
        shm_name = RACE_BMS_SHARED_STATE_NAME;
    }
    (void)snprintf(reader->shm_name, sizeof(reader->shm_name), "%s", shm_name);
}

int bms_state_reader_read(BmsStateReader *reader, uint64_t now_mono_us,
                          BmsStateSnapshot *snapshot)
{
    uint64_t published_mono_us = 0U;
    uint32_t sequence = 0U;

    if (reader == NULL || snapshot == NULL || now_mono_us == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!reader->opened &&
        bms_state_reader_try_open(reader, now_mono_us) < 0) {
        return -1;
    }
    if (race_shared_bms_state_read(&reader->shared, &snapshot->state,
                                   &published_mono_us, &sequence) < 0) {
        int saved_errno = errno;

        if (saved_errno == EPROTO || saved_errno == EINVAL) {
            bms_state_reader_close(reader);
        }
        errno = saved_errno;
        return -1;
    }
    snapshot->published_mono_us = published_mono_us;
    snapshot->sequence = sequence;
    snapshot->available = true;
    snapshot->fresh = !race_shared_bms_state_is_stale(
        now_mono_us, published_mono_us, BMS_STALE_TIMEOUT_US);
    if (published_mono_us != 0U && now_mono_us >= published_mono_us) {
        snapshot->age_us = now_mono_us - published_mono_us;
    }
    if (!snapshot->fresh && published_mono_us != 0U &&
        now_mono_us >= published_mono_us &&
        now_mono_us - published_mono_us >= BMS_STATE_READER_REOPEN_AFTER_US) {
        bms_state_reader_close(reader);
        (void)bms_state_reader_try_open(reader, now_mono_us);
    }
    return 0;
}

void bms_state_reader_close(BmsStateReader *reader)
{
    if (reader == NULL) {
        return;
    }
    race_shared_bms_state_close(&reader->shared);
    reader->opened = false;
    reader->last_open_attempt_us = 0U;
}
