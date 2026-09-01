#ifndef T113_WEBD_VIDEO_CACHE_H
#define T113_WEBD_VIDEO_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#include <race/video_ipc.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef struct {
    bool available;
    bool fresh;
    uint64_t sequence;
    uint64_t pts_us;
    uint64_t frame_age_us;
    uint64_t source_frame_count;
    uint64_t dropped_by_rate_limit;
    uint32_t width;
    uint32_t height;
    uint32_t jpeg_size;
    double source_fps;
    double web_publish_fps;
} WebdVideoSnapshot;

typedef struct {
    pthread_mutex_t mutex;
    RaceVideoIpcHandle reader;
    char shm_name[NAME_MAX];
    pthread_t thread;
    uint8_t *jpeg_data;
    size_t jpeg_capacity;
    size_t jpeg_size;
    RaceVideoFrameInfo info;
    uint64_t last_open_attempt_us;
    uint64_t last_seen_sequence;
    uint64_t rate_window_pts_us;
    uint64_t rate_window_source_frames;
    uint64_t rate_window_sequence;
    double source_fps;
    double web_publish_fps;
    bool opened;
    bool have_latest;
    bool thread_started;
    _Atomic bool stopping;
} WebdVideoCache;

void webd_video_cache_init(WebdVideoCache *cache, const char *shm_name);
int webd_video_cache_start(WebdVideoCache *cache);
void webd_video_cache_stop(WebdVideoCache *cache);

void webd_video_cache_snapshot(WebdVideoCache *cache, uint64_t now_us,
                               WebdVideoSnapshot *snapshot);

/* 0: copied a new frame, 1: no newer frame, -1: unavailable/error. */
int webd_video_cache_copy_latest(WebdVideoCache *cache, uint64_t last_sequence,
                                 uint8_t *destination, size_t capacity,
                                 RaceVideoFrameInfo *info);

#endif
