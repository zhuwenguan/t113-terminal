#define _POSIX_C_SOURCE 200809L

#include <race/bms_shared_state.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *bms_object_name(const char *name)
{
    return name != NULL ? name : RACE_BMS_SHARED_STATE_NAME;
}

static void bms_reset_handle(RaceSharedBmsStateHandle *handle)
{
    memset(handle, 0, sizeof(*handle));
    handle->fd = -1;
}

static int bms_lock_writer_fd(int fd)
{
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            errno = EBUSY;
        }
        return -1;
    }
    return 0;
}

static void bms_initialize_mapping(RaceSharedBmsState *mapping)
{
    uint32_t sequence = atomic_load_explicit(&mapping->seq,
                                             memory_order_relaxed);

    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    atomic_store_explicit(&mapping->seq, sequence + 1U,
                          memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    mapping->magic = RACE_BMS_SHM_MAGIC;
    mapping->version = RACE_BMS_SHM_VERSION;
    mapping->header_size = (uint16_t)offsetof(RaceSharedBmsState, state);
    mapping->total_size = sizeof(RaceSharedBmsState);
    mapping->state_size = sizeof(BmsState);
    mapping->flags = 0U;
    mapping->publish_mono_us = 0U;
    memset(&mapping->state, 0, sizeof(mapping->state));
    atomic_store_explicit(&mapping->seq, sequence + 2U, memory_order_release);
}

int race_shared_bms_state_validate(const RaceSharedBmsState *mapping)
{
    if (mapping == NULL || mapping->magic != RACE_BMS_SHM_MAGIC ||
        mapping->version != RACE_BMS_SHM_VERSION ||
        mapping->header_size != offsetof(RaceSharedBmsState, state) ||
        mapping->total_size != sizeof(RaceSharedBmsState) ||
        mapping->state_size != sizeof(BmsState)) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int race_shared_bms_state_writer_open(RaceSharedBmsStateHandle *handle,
                                      const char *name)
{
    _Atomic uint32_t local_sequence;

    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    bms_reset_handle(handle);
    if (!atomic_is_lock_free(&local_sequence)) {
        errno = ENOTSUP;
        return -1;
    }
    handle->fd = shm_open(bms_object_name(name), O_RDWR | O_CREAT, 0644);
    if (handle->fd < 0 || bms_lock_writer_fd(handle->fd) < 0 ||
        ftruncate(handle->fd, sizeof(RaceSharedBmsState)) < 0) {
        goto fail;
    }
    handle->mapping = mmap(NULL, sizeof(RaceSharedBmsState),
                           PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if (handle->mapping == MAP_FAILED) {
        handle->mapping = NULL;
        goto fail;
    }
    if (!atomic_is_lock_free(&handle->mapping->seq)) {
        errno = ENOTSUP;
        goto fail;
    }
    bms_initialize_mapping(handle->mapping);
    handle->writer = true;
    return 0;
fail:
    race_shared_bms_state_close(handle);
    return -1;
}

int race_shared_bms_state_reader_open(RaceSharedBmsStateHandle *handle,
                                      const char *name)
{
    struct stat status;

    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    bms_reset_handle(handle);
    memset(&status, 0, sizeof(status));
    handle->fd = shm_open(bms_object_name(name), O_RDONLY, 0);
    if (handle->fd < 0 || fstat(handle->fd, &status) < 0) {
        goto fail;
    }
    if (status.st_size != (off_t)sizeof(RaceSharedBmsState)) {
        errno = EPROTO;
        goto fail;
    }
    handle->mapping = mmap(NULL, sizeof(RaceSharedBmsState), PROT_READ,
                           MAP_SHARED, handle->fd, 0);
    if (handle->mapping == MAP_FAILED) {
        handle->mapping = NULL;
        goto fail;
    }
    if (race_shared_bms_state_validate(handle->mapping) < 0) {
        goto fail;
    }
    return 0;
fail:
    race_shared_bms_state_close(handle);
    return -1;
}

int race_shared_bms_state_publish(RaceSharedBmsStateHandle *handle,
                                  const BmsState *state,
                                  uint64_t published_mono_us)
{
    uint32_t sequence;

    if (handle == NULL || !handle->writer || handle->mapping == NULL ||
        state == NULL) {
        errno = EINVAL;
        return -1;
    }
    sequence = atomic_load_explicit(&handle->mapping->seq,
                                    memory_order_relaxed);
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    atomic_store_explicit(&handle->mapping->seq, sequence + 1U,
                          memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    memcpy(&handle->mapping->state, state, sizeof(*state));
    handle->mapping->publish_mono_us = published_mono_us;
    atomic_store_explicit(&handle->mapping->seq, sequence + 2U,
                          memory_order_release);
    return 0;
}

int race_shared_bms_state_read(const RaceSharedBmsStateHandle *handle,
                               BmsState *state, uint64_t *published_mono_us,
                               uint32_t *sequence)
{
    unsigned int retry;

    if (handle == NULL || handle->mapping == NULL || state == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (retry = 0U; retry < RACE_BMS_SHARED_READ_RETRIES; ++retry) {
        uint32_t before = atomic_load_explicit(&handle->mapping->seq,
                                               memory_order_acquire);
        uint64_t timestamp;

        if ((before & 1U) != 0U) {
            continue;
        }
        memcpy(state, &handle->mapping->state, sizeof(*state));
        timestamp = handle->mapping->publish_mono_us;
        atomic_thread_fence(memory_order_seq_cst);
        if (before == atomic_load_explicit(&handle->mapping->seq,
                                           memory_order_acquire)) {
            if (published_mono_us != NULL) {
                *published_mono_us = timestamp;
            }
            if (sequence != NULL) {
                *sequence = before;
            }
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

bool race_shared_bms_state_is_stale(uint64_t now_mono_us,
                                    uint64_t published_mono_us,
                                    uint64_t threshold_us)
{
    return published_mono_us == 0U || now_mono_us < published_mono_us ||
           now_mono_us - published_mono_us > threshold_us;
}

void race_shared_bms_state_close(RaceSharedBmsStateHandle *handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->mapping != NULL) {
        (void)munmap(handle->mapping, sizeof(RaceSharedBmsState));
    }
    if (handle->fd >= 0) {
        (void)close(handle->fd);
    }
    bms_reset_handle(handle);
}
