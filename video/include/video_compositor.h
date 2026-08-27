#ifndef T113_VIDEO_COMPOSITOR_H
#define T113_VIDEO_COMPOSITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hud_renderer.h"
#include "video_frame.h"

typedef enum {
    VIDEO_COMPOSITOR_MODE_AUTO = 0,
    VIDEO_COMPOSITOR_MODE_G2D,
    VIDEO_COMPOSITOR_MODE_CPU
} VideoCompositorMode;

typedef struct {
    uint64_t yuv_copy_time_us;
    uint64_t yuv_cache_sync_time_us;
    uint64_t yuv_to_argb_time_us;
    uint64_t hud_cache_sync_time_us;
    uint64_t blend_time_us;
} VideoCompositorTiming;

typedef int (*VideoIonOpenFn)(void);
typedef void (*VideoIonCloseFn)(void);
typedef void *(*VideoIonAllocFn)(int size);
typedef void (*VideoIonFreeFn)(void *buffer);
typedef int (*VideoIonGetFdFn)(void *buffer);
typedef void (*VideoIonFlushFn)(void *buffer, int size);

typedef struct {
    void *memory;
    int fd;
    size_t size;
} VideoIonBuffer;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t argb_stride;
    VideoCompositorMode requested_mode;
    VideoCompositorMode active_mode;
    int g2d_fd;
    void *ion_library;
    VideoIonOpenFn ion_open;
    VideoIonCloseFn ion_close;
    VideoIonAllocFn ion_alloc;
    VideoIonFreeFn ion_free;
    VideoIonGetFdFn ion_get_fd;
    VideoIonFlushFn ion_flush;
    VideoIonBuffer yuv_input;
    VideoIonBuffer argb_camera;
    VideoIonBuffer argb_hud;
    VideoIonBuffer argb_final;
    HudSurface hud_surface;
    VideoFrame final_frame;
    uint32_t fatal_failures;
    bool ion_opened;
    bool g2d_healthy;
    bool initialized;
} VideoCompositor;

const char *video_compositor_mode_name(VideoCompositorMode mode);
bool video_compositor_parse_mode(const char *value, VideoCompositorMode *mode);
int video_compositor_init(VideoCompositor *compositor, uint32_t width,
                          uint32_t height);
int video_compositor_init_mode(VideoCompositor *compositor, uint32_t width,
                               uint32_t height, VideoCompositorMode mode);
void video_compositor_close(VideoCompositor *compositor);
VideoCompositorMode video_compositor_active_mode(
    const VideoCompositor *compositor);
bool video_compositor_is_g2d_healthy(const VideoCompositor *compositor);
const HudSurface *video_compositor_hud_surface(
    const VideoCompositor *compositor);
bool video_compositor_sync_hud(VideoCompositor *compositor,
                               VideoCompositorTiming *timing);
const VideoFrame *video_compositor_compose_g2d(
    VideoCompositor *compositor, const VideoYuvFrame *camera,
    uint64_t timestamp_us, VideoCompositorTiming *timing);
bool video_compositor_compose(const VideoCompositor *compositor,
                              const VideoFrame *camera,
                              const HudLayer *static_layer,
                              const HudLayer *dynamic_layer,
                              VideoFrame *output);
bool video_compositor_compose_yuv(const VideoCompositor *compositor,
                                  VideoYuvFrame *camera,
                                  const HudLayer *static_layer,
                                  const HudLayer *dynamic_layer);

#endif
