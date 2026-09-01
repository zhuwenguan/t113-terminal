#ifndef RACE_VIDEO_IPC_H
#define RACE_VIDEO_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define RACE_VIDEO_IPC_NAME "/race_latest_jpeg"
#define RACE_VIDEO_IPC_MAGIC UINT32_C(0x524A5047)
#define RACE_VIDEO_IPC_VERSION 1U
#define RACE_VIDEO_IPC_SLOT_COUNT 2U
#define RACE_VIDEO_IPC_MAX_JPEG_SIZE (4U * 1024U * 1024U)
#define RACE_VIDEO_IPC_STALE_US UINT64_C(1000000)
#define RACE_VIDEO_IPC_READ_RETRIES 1000U

typedef struct {
    uint64_t sequence;
    uint64_t pts_us;
    uint64_t source_frame_count;
    uint64_t dropped_by_rate_limit;
    uint32_t source_sequence;
    uint32_t width;
    uint32_t height;
    uint32_t jpeg_size;
} RaceVideoFrameInfo;

typedef struct {
    _Atomic uint32_t sequence;
    RaceVideoFrameInfo info;
    uint8_t jpeg_data[RACE_VIDEO_IPC_MAX_JPEG_SIZE];
} RaceVideoJpegSlot;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t slot_size;
    uint32_t slot_count;
    _Atomic uint32_t active_slot;
    RaceVideoJpegSlot slots[RACE_VIDEO_IPC_SLOT_COUNT];
} RaceVideoJpegShared;

typedef struct {
    int fd;
    RaceVideoJpegShared *mapping;
    bool writer;
    uint64_t next_sequence;
} RaceVideoIpcHandle;

int race_video_ipc_writer_open(RaceVideoIpcHandle *handle, const char *name);
int race_video_ipc_reader_open(RaceVideoIpcHandle *handle, const char *name);
int race_video_ipc_validate(const RaceVideoJpegShared *mapping);

/* Publishes one self-contained JPEG. The function never waits for a reader. */
int race_video_ipc_publish(RaceVideoIpcHandle *handle,
                           const uint8_t *jpeg_data, size_t jpeg_size,
                           const RaceVideoFrameInfo *info);

/* Reads only metadata from the current published slot. */
int race_video_ipc_peek_latest(const RaceVideoIpcHandle *handle,
                               RaceVideoFrameInfo *info);

/* Copies one complete latest JPEG into caller-owned storage. */
int race_video_ipc_read_latest(const RaceVideoIpcHandle *handle,
                               uint8_t *jpeg_data, size_t jpeg_capacity,
                               RaceVideoFrameInfo *info);

bool race_video_ipc_is_stale(uint64_t now_us, uint64_t pts_us,
                             uint64_t threshold_us);
void race_video_ipc_close(RaceVideoIpcHandle *handle);

#endif
