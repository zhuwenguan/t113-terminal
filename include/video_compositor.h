#ifndef T113_VIDEO_COMPOSITOR_H
#define T113_VIDEO_COMPOSITOR_H

#include <stdbool.h>

#include "hud_renderer.h"
#include "video_frame.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    bool initialized;
} VideoCompositor;

int video_compositor_init(VideoCompositor *compositor, uint32_t width,
                          uint32_t height);
bool video_compositor_compose(const VideoCompositor *compositor,
                              const VideoFrame *camera,
                              const HudLayer *static_layer,
                              const HudLayer *dynamic_layer,
                              VideoFrame *output);

#endif
