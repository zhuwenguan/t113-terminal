#ifndef T113_HUD_RENDERER_H
#define T113_HUD_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hud_render.h"

/* The production camera, HUD and recorder use the fixed 800x600 design. */
#define HUD_BASE_WIDTH 800U
#define HUD_BASE_HEIGHT 600U
#define HUD_MAX_REGIONS 12U

typedef enum {
    HUD_PIXEL_FORMAT_UNKNOWN = 0,
    /* CPU video frames use R,G,B,A bytes. */
    HUD_PIXEL_FORMAT_RGBA8888 = 1,
    /* T113 G2D/Cedar ARGB8888 uses B,G,R,A bytes in CPU memory. */
    HUD_PIXEL_FORMAT_ARGB8888 = 2
} HudPixelFormat;

typedef enum {
    HUD_STYLE_UI = 0,
    HUD_STYLE_NONE = 1
} HudStyle;

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

typedef struct HudLayer {
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
    HudLayer static_layer;
    HudLayer dynamic_layer;
    HudLayer external_dynamic_layer;
    bool dynamic_surface_only;
    HudStyle style;
    HudContext *ui_context;
    bool initialized;
} HudRenderer;

/* Initializes the selected backend. UI requires a TTF font; none does not. */
int hud_renderer_init_style(HudRenderer *renderer, uint32_t width,
                            uint32_t height, HudStyle style,
                            const char *font_path,
                            HudPixelFormat surface_format);
const char *hud_style_name(HudStyle style);
bool hud_style_parse(const char *value, HudStyle *style);
void hud_renderer_close(HudRenderer *renderer);

/* Owned layer memory only; caller-owned external surfaces are not counted. */
size_t hud_renderer_resident_memory(const HudRenderer *renderer);

/* Transparent static layer used by the CPU compositor. */
const HudLayer *hud_renderer_static_layer(const HudRenderer *renderer);

/* Renders one complete UI frame into the owned CPU layer. */
const HudLayer *hud_renderer_render_dynamic(HudRenderer *renderer,
                                             const VehicleState *state,
                                             bool recording);

/* Renders directly into a caller-owned HudSurface. */
const HudLayer *hud_renderer_render_dynamic_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing);
const HudLayer *hud_renderer_render_frame_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing);

#endif
