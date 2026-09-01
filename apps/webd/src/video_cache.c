#define _POSIX_C_SOURCE 200809L

#include "video_cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    WEBD_VIDEO_RETRY_INTERVAL_US = 250000U,
    WEBD_VIDEO_POLL_INTERVAL_US = 20000U,
    WEBD_VIDEO_REOPEN_AFTER_US = 2000000U,
    WEBD_VIDEO_RATE_WINDOW_US = 1000000U
};

static uint64_t video_cache_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static void video_cache_sleep(void)
{
    struct timespec delay = {0, WEBD_VIDEO_POLL_INTERVAL_US * 1000L};

    (void)nanosleep(&delay, NULL);
}

static int video_cache_try_open(WebdVideoCache *cache, uint64_t now_us)
{
    if (cache->opened) {
        return 0;
    }
    if (cache->last_open_attempt_us != 0U &&
        now_us >= cache->last_open_attempt_us &&
        now_us - cache->last_open_attempt_us <
            WEBD_VIDEO_RETRY_INTERVAL_US) {
        errno = ENOENT;
        return -1;
    }
    cache->last_open_attempt_us = now_us;
    if (race_video_ipc_reader_open(&cache->reader, cache->shm_name) < 0) {
        return -1;
    }
    cache->opened = true;
    return 0;
}

static int video_cache_reserve(WebdVideoCache *cache, size_t size)
{
    uint8_t *data;
    size_t capacity;

    if (size > RACE_VIDEO_IPC_MAX_JPEG_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    if (size <= cache->jpeg_capacity) {
        return 0;
    }
    capacity = cache->jpeg_capacity > 0U ? cache->jpeg_capacity : 65536U;
    while (capacity < size && capacity < RACE_VIDEO_IPC_MAX_JPEG_SIZE) {
        if (capacity > RACE_VIDEO_IPC_MAX_JPEG_SIZE / 2U) {
            capacity = RACE_VIDEO_IPC_MAX_JPEG_SIZE;
        } else {
            capacity *= 2U;
        }
    }
    if (capacity < size) {
        errno = EMSGSIZE;
        return -1;
    }
    data = realloc(cache->jpeg_data, capacity);
    if (data == NULL) {
        return -1;
    }
    cache->jpeg_data = data;
    cache->jpeg_capacity = capacity;
    return 0;
}

static void video_cache_mark_freshness(WebdVideoCache *cache,
                                       uint64_t now_us)
{
    if (!cache->have_latest || cache->info.pts_us == 0U) {
        return;
    }
    if (race_video_ipc_is_stale(now_us, cache->info.pts_us,
                                RACE_VIDEO_IPC_STALE_US) &&
        now_us >= cache->info.pts_us &&
        now_us - cache->info.pts_us >= WEBD_VIDEO_REOPEN_AFTER_US) {
        race_video_ipc_close(&cache->reader);
        cache->opened = false;
    }
}

static void video_cache_update_rates(WebdVideoCache *cache,
                                     const RaceVideoFrameInfo *info)
{
    /* A restarted producer reinitializes the sequence and source counters.
     * Do not turn that reset into a bogus one-frame FPS spike. */
    if ((cache->last_seen_sequence != 0U &&
         info->sequence < cache->last_seen_sequence) ||
        (cache->rate_window_source_frames != 0U &&
         info->source_frame_count < cache->rate_window_source_frames)) {
        cache->rate_window_pts_us = 0U;
        cache->rate_window_source_frames = 0U;
        cache->rate_window_sequence = 0U;
        cache->last_seen_sequence = 0U;
        cache->source_fps = 0.0;
        cache->web_publish_fps = 0.0;
    }
    if (cache->rate_window_pts_us == 0U) {
        cache->rate_window_pts_us = info->pts_us;
        cache->rate_window_source_frames = info->source_frame_count;
        cache->rate_window_sequence = info->sequence;
    } else if (info->pts_us > cache->rate_window_pts_us &&
               info->pts_us - cache->rate_window_pts_us >=
                   WEBD_VIDEO_RATE_WINDOW_US) {
        uint64_t source_delta = info->source_frame_count >=
                                cache->rate_window_source_frames ?
                                info->source_frame_count -
                                cache->rate_window_source_frames : 0U;
        uint64_t publish_delta = info->sequence >=
                                 cache->rate_window_sequence ?
                                 info->sequence - cache->rate_window_sequence :
                                 0U;
        double elapsed_s = (double)(info->pts_us -
                                    cache->rate_window_pts_us) / 1000000.0;

        if (source_delta > 0U) {
            cache->source_fps = (double)source_delta / elapsed_s;
        }
        if (publish_delta > 0U) {
            cache->web_publish_fps = (double)publish_delta / elapsed_s;
        }
        cache->rate_window_pts_us = info->pts_us;
        cache->rate_window_source_frames = info->source_frame_count;
        cache->rate_window_sequence = info->sequence;
    }
    cache->last_seen_sequence = info->sequence;
}

static void *video_cache_thread_main(void *context)
{
    WebdVideoCache *cache = context;

    while (!atomic_load_explicit(&cache->stopping, memory_order_acquire)) {
        uint64_t now_us = video_cache_now_us();

        if (now_us == 0U) {
            video_cache_sleep();
            continue;
        }
        if (!cache->opened && video_cache_try_open(cache, now_us) < 0) {
            video_cache_sleep();
            continue;
        }
        if (cache->opened) {
            RaceVideoFrameInfo info;

            if (race_video_ipc_peek_latest(&cache->reader, &info) == 0) {
                if (info.sequence != cache->last_seen_sequence) {
                    pthread_mutex_lock(&cache->mutex);
                    if (video_cache_reserve(cache, info.jpeg_size) == 0 &&
                        race_video_ipc_read_latest(&cache->reader,
                                                   cache->jpeg_data,
                                                   cache->jpeg_capacity,
                                                   &info) == 0) {
                        video_cache_update_rates(cache, &info);
                        cache->info = info;
                        cache->jpeg_size = info.jpeg_size;
                        cache->have_latest = true;
                    }
                    pthread_mutex_unlock(&cache->mutex);
                }
            } else if (errno == EPROTO || errno == EINVAL) {
                race_video_ipc_close(&cache->reader);
                cache->opened = false;
            }
            pthread_mutex_lock(&cache->mutex);
            video_cache_mark_freshness(cache, now_us);
            pthread_mutex_unlock(&cache->mutex);
        }
        video_cache_sleep();
    }
    race_video_ipc_close(&cache->reader);
    cache->opened = false;
    return NULL;
}

void webd_video_cache_init(WebdVideoCache *cache, const char *shm_name)
{
    if (cache == NULL) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
    cache->reader.fd = -1;
    (void)snprintf(cache->shm_name, sizeof(cache->shm_name), "%s",
                   shm_name != NULL && shm_name[0] != '\0' ?
                   shm_name : RACE_VIDEO_IPC_NAME);
    (void)pthread_mutex_init(&cache->mutex, NULL);
    atomic_init(&cache->stopping, false);
}

int webd_video_cache_start(WebdVideoCache *cache)
{
    int result;

    if (cache == NULL) {
        errno = EINVAL;
        return -1;
    }
    result = pthread_create(&cache->thread, NULL, video_cache_thread_main,
                            cache);
    if (result != 0) {
        errno = result;
        return -1;
    }
    cache->thread_started = true;
    return 0;
}

void webd_video_cache_stop(WebdVideoCache *cache)
{
    if (cache == NULL) {
        return;
    }
    atomic_store_explicit(&cache->stopping, true, memory_order_release);
    if (cache->thread_started) {
        (void)pthread_join(cache->thread, NULL);
        cache->thread_started = false;
    }
    pthread_mutex_lock(&cache->mutex);
    free(cache->jpeg_data);
    cache->jpeg_data = NULL;
    cache->jpeg_capacity = 0U;
    cache->jpeg_size = 0U;
    pthread_mutex_unlock(&cache->mutex);
    pthread_mutex_destroy(&cache->mutex);
}

void webd_video_cache_snapshot(WebdVideoCache *cache, uint64_t now_us,
                               WebdVideoSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (cache == NULL) {
        return;
    }
    pthread_mutex_lock(&cache->mutex);
    if (cache->have_latest) {
        snapshot->sequence = cache->info.sequence;
        snapshot->pts_us = cache->info.pts_us;
        snapshot->source_frame_count = cache->info.source_frame_count;
        snapshot->dropped_by_rate_limit = cache->info.dropped_by_rate_limit;
        snapshot->width = cache->info.width;
        snapshot->height = cache->info.height;
        snapshot->jpeg_size = cache->info.jpeg_size;
        snapshot->source_fps = cache->source_fps;
        snapshot->web_publish_fps = cache->web_publish_fps;
        snapshot->fresh = !race_video_ipc_is_stale(
            now_us, cache->info.pts_us, RACE_VIDEO_IPC_STALE_US);
        snapshot->available = snapshot->fresh;
        if (now_us >= cache->info.pts_us) {
            snapshot->frame_age_us = now_us - cache->info.pts_us;
        }
    }
    pthread_mutex_unlock(&cache->mutex);
}

int webd_video_cache_copy_latest(WebdVideoCache *cache, uint64_t last_sequence,
                                 uint8_t *destination, size_t capacity,
                                 RaceVideoFrameInfo *info)
{
    uint64_t now_us;
    int result = 1;

    if (cache == NULL || destination == NULL || info == NULL) {
        errno = EINVAL;
        return -1;
    }
    now_us = video_cache_now_us();
    pthread_mutex_lock(&cache->mutex);
    if (!cache->have_latest ||
        race_video_ipc_is_stale(now_us, cache->info.pts_us,
                                RACE_VIDEO_IPC_STALE_US)) {
        errno = ENODATA;
        result = -1;
    } else if (cache->info.sequence != last_sequence) {
        if ((size_t)cache->jpeg_size > capacity) {
            errno = EMSGSIZE;
            result = -1;
        } else {
            memcpy(destination, cache->jpeg_data, cache->jpeg_size);
            *info = cache->info;
            result = 0;
        }
    }
    pthread_mutex_unlock(&cache->mutex);
    return result;
}
