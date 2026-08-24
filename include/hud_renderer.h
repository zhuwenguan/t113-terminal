#ifndef T113_HUD_RENDERER_H
#define T113_HUD_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "vehicle_state.h"
#include "video_frame.h"

#define HUD_BASE_WIDTH 1280U
#define HUD_BASE_HEIGHT 720U
#define HUD_MAX_REGIONS 12U

typedef enum {
    HUD_PIXEL_FORMAT_UNKNOWN = 0,
    /* Existing CPU/compositor layers use byte order R, G, B, A. */
    HUD_PIXEL_FORMAT_RGBA8888 = 1,
    /* G2D/Cedar ARGB8888 on this board uses byte order B, G, R, A. */
    HUD_PIXEL_FORMAT_ARGB8888 = 2
} HudPixelFormat;

typedef struct {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    HudPixelFormat pixel_format;
} HudSurface;

typedef struct {
    uint64_t clear_time_us;
    uint64_t draw_time_us;
    uint64_t total_time_us;
} HudRenderTiming;

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
    HudPixelFormat pixel_format;
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
    HudLayer external_dynamic_layer;
    bool dynamic_surface_only;
    bool initialized;
} HudRenderer;

int hud_renderer_init(HudRenderer *renderer, uint32_t width, uint32_t height);
/* Initializes a renderer without allocating a dynamic CPU framebuffer. */
int hud_renderer_init_external(HudRenderer *renderer, uint32_t width,
                               uint32_t height);
void hud_renderer_close(HudRenderer *renderer);

/* Static geometry/text is rendered once during initialization. */
const HudLayer *hud_renderer_static_layer(const HudRenderer *renderer);

/* The dynamic layer is cleared and rendered from exactly one state snapshot. */
const HudLayer *hud_renderer_render_dynamic(HudRenderer *renderer,
                                             const VehicleState *state,
                                             bool recording);

/* Renders the dynamic layer directly into a caller-owned surface. */
const HudLayer *hud_renderer_render_dynamic_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing);

/* Clears and renders the complete static + dynamic HUD directly into a
 * caller-owned surface.  This is the production G2D overlay path: no heap
 * framebuffer upload is performed. */
const HudLayer *hud_renderer_render_frame_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing);

#endif
