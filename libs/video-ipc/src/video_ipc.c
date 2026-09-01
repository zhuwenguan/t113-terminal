#define _POSIX_C_SOURCE 200809L

#include <race/video_ipc.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *video_ipc_object_name(const char *name)
{
    return name != NULL && name[0] != '\0' ? name : RACE_VIDEO_IPC_NAME;
}

static void video_ipc_reset_handle(RaceVideoIpcHandle *handle)
{
    if (handle == NULL) {
        return;
    }
    memset(handle, 0, sizeof(*handle));
    handle->fd = -1;
}

static int video_ipc_lock_writer(int fd)
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

static void video_ipc_initialize(RaceVideoJpegShared *mapping)
{
    unsigned int index;

    memset(mapping, 0, sizeof(*mapping));
    mapping->magic = RACE_VIDEO_IPC_MAGIC;
    mapping->version = RACE_VIDEO_IPC_VERSION;
    mapping->header_size = (uint16_t)offsetof(RaceVideoJpegShared, slots);
    mapping->total_size = sizeof(*mapping);
    mapping->slot_size = sizeof(RaceVideoJpegSlot);
    mapping->slot_count = RACE_VIDEO_IPC_SLOT_COUNT;
    atomic_init(&mapping->active_slot, 0U);
    for (index = 0U; index < RACE_VIDEO_IPC_SLOT_COUNT; ++index) {
        atomic_init(&mapping->slots[index].sequence, 0U);
    }
    atomic_thread_fence(memory_order_seq_cst);
}

int race_video_ipc_validate(const RaceVideoJpegShared *mapping)
{
    if (mapping == NULL || mapping->magic != RACE_VIDEO_IPC_MAGIC ||
        mapping->version != RACE_VIDEO_IPC_VERSION ||
        mapping->header_size != offsetof(RaceVideoJpegShared, slots) ||
        mapping->total_size != sizeof(RaceVideoJpegShared) ||
        mapping->slot_size != sizeof(RaceVideoJpegSlot) ||
        mapping->slot_count != RACE_VIDEO_IPC_SLOT_COUNT) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int race_video_ipc_writer_open(RaceVideoIpcHandle *handle, const char *name)
{
    _Atomic uint32_t local_sequence;
    struct stat status;
    bool initialize_mapping;

    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    video_ipc_reset_handle(handle);
    if (!atomic_is_lock_free(&local_sequence)) {
        errno = ENOTSUP;
        return -1;
    }
    handle->fd = shm_open(video_ipc_object_name(name), O_RDWR | O_CREAT, 0644);
    if (handle->fd < 0 || video_ipc_lock_writer(handle->fd) < 0 ||
        fstat(handle->fd, &status) < 0) {
        goto fail;
    }
    initialize_mapping = status.st_size != (off_t)sizeof(RaceVideoJpegShared);
    if (initialize_mapping &&
        ftruncate(handle->fd, sizeof(RaceVideoJpegShared)) < 0) {
        goto fail;
    }
    handle->mapping = mmap(NULL, sizeof(RaceVideoJpegShared),
                           PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if (handle->mapping == MAP_FAILED) {
        handle->mapping = NULL;
        goto fail;
    }
    if (!atomic_is_lock_free(&handle->mapping->active_slot)) {
        errno = ENOTSUP;
        goto fail;
    }
    if (initialize_mapping || race_video_ipc_validate(handle->mapping) < 0) {
        video_ipc_initialize(handle->mapping);
    } else {
        uint32_t active_slot = atomic_load_explicit(
            &handle->mapping->active_slot, memory_order_acquire);

        if (active_slot < RACE_VIDEO_IPC_SLOT_COUNT) {
            uint32_t slot_sequence = atomic_load_explicit(
                &handle->mapping->slots[active_slot].sequence,
                memory_order_acquire);
            if ((slot_sequence & 1U) == 0U) {
                handle->next_sequence =
                    handle->mapping->slots[active_slot].info.sequence;
            }
        }
    }
    handle->writer = true;
    return 0;

fail:
    race_video_ipc_close(handle);
    return -1;
}

int race_video_ipc_reader_open(RaceVideoIpcHandle *handle, const char *name)
{
    struct stat status;

    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    video_ipc_reset_handle(handle);
    memset(&status, 0, sizeof(status));
    handle->fd = shm_open(video_ipc_object_name(name), O_RDONLY, 0);
    if (handle->fd < 0 || fstat(handle->fd, &status) < 0) {
        goto fail;
    }
    if (status.st_size != (off_t)sizeof(RaceVideoJpegShared)) {
        errno = EPROTO;
        goto fail;
    }
    handle->mapping = mmap(NULL, sizeof(RaceVideoJpegShared), PROT_READ,
                           MAP_SHARED, handle->fd, 0);
    if (handle->mapping == MAP_FAILED) {
        handle->mapping = NULL;
        goto fail;
    }
    if (race_video_ipc_validate(handle->mapping) < 0) {
        goto fail;
    }
    return 0;

fail:
    race_video_ipc_close(handle);
    return -1;
}

static int video_ipc_read_slot(const RaceVideoJpegShared *mapping,
                               uint8_t *jpeg_data, size_t jpeg_capacity,
                               RaceVideoFrameInfo *info, bool copy_data)
{
    unsigned int retry;

    if (mapping == NULL || (copy_data && jpeg_data == NULL) || info == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (retry = 0U; retry < RACE_VIDEO_IPC_READ_RETRIES; ++retry) {
        uint32_t active_slot;
        const RaceVideoJpegSlot *slot;
        uint32_t before;
        uint32_t after;
        RaceVideoFrameInfo local_info;

        active_slot = atomic_load_explicit(&mapping->active_slot,
                                           memory_order_acquire);
        if (active_slot >= RACE_VIDEO_IPC_SLOT_COUNT) {
            continue;
        }
        slot = &mapping->slots[active_slot];
        before = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        local_info = slot->info;
        if (local_info.sequence == 0U || local_info.jpeg_size == 0U ||
            local_info.jpeg_size > RACE_VIDEO_IPC_MAX_JPEG_SIZE ||
            local_info.width == 0U || local_info.height == 0U) {
            errno = ENODATA;
            return -1;
        }
        if (copy_data) {
            if ((size_t)local_info.jpeg_size > jpeg_capacity) {
                errno = EMSGSIZE;
                return -1;
            }
            memcpy(jpeg_data, slot->jpeg_data, local_info.jpeg_size);
        }
        atomic_thread_fence(memory_order_seq_cst);
        after = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (before == after) {
            *info = local_info;
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

int race_video_ipc_peek_latest(const RaceVideoIpcHandle *handle,
                               RaceVideoFrameInfo *info)
{
    if (handle == NULL || handle->mapping == NULL || info == NULL) {
        errno = EINVAL;
        return -1;
    }
    return video_ipc_read_slot(handle->mapping, NULL, 0U, info, false);
}

int race_video_ipc_read_latest(const RaceVideoIpcHandle *handle,
                               uint8_t *jpeg_data, size_t jpeg_capacity,
                               RaceVideoFrameInfo *info)
{
    if (handle == NULL || handle->mapping == NULL || jpeg_data == NULL ||
        info == NULL) {
        errno = EINVAL;
        return -1;
    }
    return video_ipc_read_slot(handle->mapping, jpeg_data, jpeg_capacity,
                               info, true);
}

int race_video_ipc_publish(RaceVideoIpcHandle *handle,
                           const uint8_t *jpeg_data, size_t jpeg_size,
                           const RaceVideoFrameInfo *info)
{
    unsigned int active_slot;
    unsigned int inactive_slot;
    RaceVideoJpegSlot *slot;
    uint32_t slot_sequence;
    RaceVideoFrameInfo published_info;

    if (handle == NULL || !handle->writer || handle->mapping == NULL ||
        jpeg_data == NULL || jpeg_size == 0U || info == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (jpeg_size > RACE_VIDEO_IPC_MAX_JPEG_SIZE ||
        info->width == 0U || info->height == 0U) {
        errno = EMSGSIZE;
        return -1;
    }
    active_slot = atomic_load_explicit(&handle->mapping->active_slot,
                                       memory_order_acquire);
    if (active_slot >= RACE_VIDEO_IPC_SLOT_COUNT) {
        active_slot = 0U;
    }
    inactive_slot = active_slot ^ 1U;
    slot = &handle->mapping->slots[inactive_slot];
    slot_sequence = atomic_load_explicit(&slot->sequence,
                                         memory_order_relaxed);
    if (slot_sequence & 1U) {
        ++slot_sequence;
    }
    atomic_store_explicit(&slot->sequence, slot_sequence + 1U,
                          memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    published_info = *info;
    if (++handle->next_sequence == 0U) {
        ++handle->next_sequence;
    }
    published_info.sequence = handle->next_sequence;
    published_info.jpeg_size = (uint32_t)jpeg_size;
    memcpy(slot->jpeg_data, jpeg_data, jpeg_size);
    slot->info = published_info;
    atomic_store_explicit(&slot->sequence, slot_sequence + 2U,
                          memory_order_release);
    atomic_store_explicit(&handle->mapping->active_slot, inactive_slot,
                          memory_order_release);
    return 0;
}

bool race_video_ipc_is_stale(uint64_t now_us, uint64_t pts_us,
                             uint64_t threshold_us)
{
    return pts_us == 0U || now_us < pts_us || now_us - pts_us > threshold_us;
}

void race_video_ipc_close(RaceVideoIpcHandle *handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->mapping != NULL) {
        (void)munmap(handle->mapping, sizeof(RaceVideoJpegShared));
    }
    if (handle->fd >= 0) {
        (void)close(handle->fd);
    }
    video_ipc_reset_handle(handle);
}
