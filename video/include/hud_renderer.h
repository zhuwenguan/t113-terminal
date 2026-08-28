#ifndef T113_HUD_RENDERER_H
#define T113_HUD_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <race/vehicle_state.h>
#include "hud_assets.h"
#include "hud_layout.h"
#include "hud_resources.h"
#include "hud_text.h"
#include "video_frame.h"

/* The production camera and recorder are native 800x600.  Keep the HUD
 * design in that same 4:3 coordinate space so the production surface has no
 * letterbox band or runtime layout translation. */
#define HUD_BASE_WIDTH 800U
#define HUD_BASE_HEIGHT 600U
#define HUD_MAX_REGIONS 12U

typedef enum {
    HUD_PIXEL_FORMAT_UNKNOWN = 0,
    /* Existing CPU/compositor layers use byte order R, G, B, A. */
    HUD_PIXEL_FORMAT_RGBA8888 = 1,
    /* G2D/Cedar ARGB8888 on this board uses byte order B, G, R, A. */
    HUD_PIXEL_FORMAT_ARGB8888 = 2
} HudPixelFormat;

typedef enum {
    /* PNG is the production default; legacy remains available for A/B. */
    HUD_STYLE_PNG = 0,
    HUD_STYLE_LEGACY = 1,
    HUD_STYLE_NONE = 2
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
    uint64_t rpm_time_us;
    uint64_t pedal_time_us;
    uint64_t g_time_us;
    uint64_t text_time_us;
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
    double scale_x;
    double scale_y;
    int offset_x;
    int offset_y;
    HudLayer static_layer;
    HudLayer dynamic_layer;
    HudLayer external_dynamic_layer;
    uint8_t *external_surface_pixels;
    bool external_surface_initialized;
    bool dynamic_surface_only;
    HudStyle style;
    HudAssets assets;
    HudTextAtlas text_atlas;
    HudLayout layout;
    /* Numeric/label text is held for the human-readable HUD refresh rate;
     * graphical instruments still use the current video-frame state. */
    VehicleState text_state;
    uint64_t text_state_last_refresh_us;
    bool text_state_initialized;
    bool text_state_valid;
    double filtered_lateral_g;
    double filtered_longitudinal_g;
    bool filtered_g_initialized;
    bool initialized;
} HudRenderer;

int hud_renderer_init(HudRenderer *renderer, uint32_t width, uint32_t height);
/* Initializes a renderer without allocating a dynamic CPU framebuffer.  A
 * static ARGB template is retained so production can restore only dynamic HUD
 * regions without a full-frame upload. */
int hud_renderer_init_external(HudRenderer *renderer, uint32_t width,
                               uint32_t height);
int hud_renderer_init_style(HudRenderer *renderer, uint32_t width,
                            uint32_t height, HudStyle style,
                            const char *asset_dir,
                            const char *font_path,
                            HudPixelFormat surface_format);
const char *hud_style_name(HudStyle style);
bool hud_style_parse(const char *value, HudStyle *style);
void hud_renderer_close(HudRenderer *renderer);

size_t hud_renderer_asset_memory(const HudRenderer *renderer);
size_t hud_renderer_resident_memory(const HudRenderer *renderer);
bool hud_renderer_png_assets_ready(const HudRenderer *renderer);

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

/* PNG/none backends are kept separate from the legacy renderer so the old
 * A/B path remains source-compatible. */
const HudLayer *hud_png_render_dynamic(HudRenderer *renderer,
                                       const VehicleState *state,
                                       bool recording);
const HudLayer *hud_png_render_dynamic_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing);
const HudLayer *hud_png_render_frame_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing);
const HudLayer *hud_none_render_dynamic(HudRenderer *renderer,
                                        const VehicleState *state,
                                        bool recording);
const HudLayer *hud_none_render_frame_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing);

#endif
