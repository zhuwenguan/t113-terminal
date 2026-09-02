#define _POSIX_C_SOURCE 200809L

#include <race/core_status.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *core_status_object_name(const char *name)
{
    return name != NULL ? name : RACE_CORE_STATUS_NAME;
}

static void core_status_reset_handle(RaceCoreStatusHandle *handle)
{
    memset(handle, 0, sizeof(*handle));
    handle->fd = -1;
}

static int core_status_lock_writer_fd(int fd)
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

static void core_status_initialize_mapping(RaceCoreStatus *mapping)
{
    atomic_store_explicit(&mapping->seq, 1U, memory_order_release);
    mapping->magic = RACE_CORE_STATUS_MAGIC;
    mapping->version = RACE_CORE_STATUS_VERSION;
    mapping->header_size = (uint16_t)offsetof(RaceCoreStatus, data);
    mapping->total_size = sizeof(*mapping);
    memset(&mapping->data, 0, sizeof(mapping->data));
    mapping->data.state = RACE_CORE_LIFECYCLE_PRECHECK;
    atomic_store_explicit(&mapping->seq, 2U, memory_order_release);
}

int race_core_status_validate(const RaceCoreStatus *status)
{
    if (status == NULL || status->magic != RACE_CORE_STATUS_MAGIC ||
        status->version != RACE_CORE_STATUS_VERSION ||
        status->header_size != offsetof(RaceCoreStatus, data) ||
        status->total_size != sizeof(*status)) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int race_core_status_writer_open(RaceCoreStatusHandle *handle,
                                 const char *name)
{
    _Atomic uint32_t local_seq;

    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    core_status_reset_handle(handle);
    if (!atomic_is_lock_free(&local_seq)) {
        errno = ENOTSUP;
        return -1;
    }
    handle->fd = shm_open(core_status_object_name(name), O_RDWR | O_CREAT,
                          0644);
    if (handle->fd < 0 || core_status_lock_writer_fd(handle->fd) < 0 ||
        ftruncate(handle->fd, sizeof(RaceCoreStatus)) < 0) {
        goto fail;
    }
    handle->mapping = mmap(NULL, sizeof(RaceCoreStatus),
                           PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if (handle->mapping == MAP_FAILED) {
        handle->mapping = NULL;
        goto fail;
    }
    if (!atomic_is_lock_free(&handle->mapping->seq)) {
        errno = ENOTSUP;
        goto fail;
    }
    core_status_initialize_mapping(handle->mapping);
    handle->writer = true;
    return 0;

fail:
    race_core_status_close(handle);
    return -1;
}

int race_core_status_reader_open(RaceCoreStatusHandle *handle,
                                 const char *name)
{
    struct stat status;

    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    core_status_reset_handle(handle);
    memset(&status, 0, sizeof(status));
    handle->fd = shm_open(core_status_object_name(name), O_RDONLY, 0);
    if (handle->fd < 0 || fstat(handle->fd, &status) < 0 ||
        status.st_size != (off_t)sizeof(RaceCoreStatus)) {
        if (status.st_size != (off_t)sizeof(RaceCoreStatus)) {
            errno = EPROTO;
        }
        goto fail;
    }
    handle->mapping = mmap(NULL, sizeof(RaceCoreStatus), PROT_READ, MAP_SHARED,
                           handle->fd, 0);
    if (handle->mapping == MAP_FAILED) {
        handle->mapping = NULL;
        goto fail;
    }
    if (race_core_status_validate(handle->mapping) < 0) {
        goto fail;
    }
    return 0;

fail:
    race_core_status_close(handle);
    return -1;
}

int race_core_status_publish(RaceCoreStatusHandle *handle,
                             const RaceCoreStatusData *data)
{
    uint32_t seq;

    if (handle == NULL || !handle->writer || handle->mapping == NULL ||
        data == NULL) {
        errno = EINVAL;
        return -1;
    }
    seq = atomic_load_explicit(&handle->mapping->seq, memory_order_relaxed);
    if (seq & 1U) {
        seq++;
    }
    atomic_store_explicit(&handle->mapping->seq, seq + 1U,
                          memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    memcpy(&handle->mapping->data, data, sizeof(*data));
    atomic_store_explicit(&handle->mapping->seq, seq + 2U,
                          memory_order_release);
    return 0;
}

int race_core_status_read(const RaceCoreStatusHandle *handle,
                          RaceCoreStatusData *data)
{
    unsigned int retry;

    if (handle == NULL || handle->mapping == NULL || data == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (retry = 0U; retry < 1000U; ++retry) {
        uint32_t before = atomic_load_explicit(&handle->mapping->seq,
                                               memory_order_acquire);
        if (before & 1U) {
            continue;
        }
        memcpy(data, &handle->mapping->data, sizeof(*data));
        atomic_thread_fence(memory_order_seq_cst);
        if (before == atomic_load_explicit(&handle->mapping->seq,
                                           memory_order_acquire)) {
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

bool race_core_status_is_stale(uint64_t now_us, uint64_t heartbeat_us,
                               uint64_t threshold_us)
{
    return heartbeat_us == 0U || now_us < heartbeat_us ||
           now_us - heartbeat_us > threshold_us;
}

const char *race_core_lifecycle_name(RaceCoreLifecycleState state)
{
    switch (state) {
    case RACE_CORE_LIFECYCLE_PRECHECK:
        return "PRECHECK";
    case RACE_CORE_LIFECYCLE_RUNNING:
        return "RUNNING";
    case RACE_CORE_LIFECYCLE_DEGRADED:
        return "DEGRADED";
    case RACE_CORE_LIFECYCLE_STOPPING:
        return "STOPPING";
    case RACE_CORE_LIFECYCLE_STOPPED:
    default:
        return "STOPPED";
    }
}

void race_core_status_close(RaceCoreStatusHandle *handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->mapping != NULL) {
        (void)munmap(handle->mapping, sizeof(RaceCoreStatus));
    }
    if (handle->fd >= 0) {
        (void)close(handle->fd);
    }
    core_status_reset_handle(handle);
}
