#ifndef T113_HUD_RENDERER_H
#define T113_HUD_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "vehicle_state.h"
#include "video_frame.h"

#define HUD_BASE_WIDTH 1280U
#define HUD_BASE_HEIGHT 720U
#define HUD_MAX_REGIONS 12U

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} HudRegion;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *pixels;
    HudRegion regions[HUD_MAX_REGIONS];
    size_t region_count;
} HudLayer;

typedef struct {
    uint32_t width;
    uint32_t height;
    double scale_x;
    double scale_y;
    HudLayer static_layer;
    HudLayer dynamic_layer;
    bool initialized;
} HudRenderer;

int hud_renderer_init(HudRenderer *renderer, uint32_t width, uint32_t height);
void hud_renderer_close(HudRenderer *renderer);

/* Static geometry/text is rendered once during initialization. */
const HudLayer *hud_renderer_static_layer(const HudRenderer *renderer);

/* The dynamic layer is cleared and rendered from exactly one state snapshot. */
const HudLayer *hud_renderer_render_dynamic(HudRenderer *renderer,
                                             const VehicleState *state,
                                             bool recording);

#endif
