#define _POSIX_C_SOURCE 200809L

#include "hud_renderer.h"

#include "hud_blend.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const HudTextColor hud_png_white = { 245U, 249U, 252U, 255U };
static const HudTextColor hud_png_muted = { 178U, 190U, 196U, 255U };
static const HudTextColor hud_png_cyan = { 44U, 210U, 232U, 255U };
static const HudTextColor hud_png_red = { 255U, 66U, 72U, 255U };
static const HudTextColor hud_png_green = { 64U, 230U, 122U, 255U };
static const HudTextColor hud_png_yellow = { 255U, 210U, 64U, 255U };

/* Layout anchors for dynamic text.  These are top anchors; the renderer turns
 * them into FreeType baselines using the actual face metrics. */
#define HUD_STATUS_TEXT_X 278
#define HUD_STATUS_TEXT_Y 16
#define HUD_TOP_PANEL_SOC_Y 22
#define HUD_TOP_PANEL_HV_Y 46
#define HUD_TOP_PANEL_TEMP_Y 100
#define HUD_RIGHT_PANEL_LAP_Y 18
#define HUD_RIGHT_PANEL_DELTA_Y 48
#define HUD_RIGHT_PANEL_TIMER_Y 78
#define HUD_TEXT_REFRESH_PERIOD_US 100000ULL

typedef enum {
    /* V3 sprite order: temperature, battery, flag, REC, GPS, connection. */
    HUD_ICON_TEMPERATURE = 0,
    HUD_ICON_BATTERY = 1,
    HUD_ICON_FLAG = 2,
    HUD_ICON_RECORDING = 3,
    HUD_ICON_GPS = 4,
    HUD_ICON_CONNECTION = 5
} HudIconIndex;

static uint64_t hud_png_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static const VehicleState *hud_png_latched_text_state(
    HudRenderer *renderer, const VehicleState *state)
{
    uint64_t now_us;

    if (renderer == NULL) return NULL;
    now_us = hud_png_now_us();
    if (!renderer->text_state_initialized ||
        (now_us >= renderer->text_state_last_refresh_us &&
         now_us - renderer->text_state_last_refresh_us >=
             HUD_TEXT_REFRESH_PERIOD_US)) {
        if (state != NULL) {
            renderer->text_state = *state;
            renderer->text_state_valid = true;
        } else {
            memset(&renderer->text_state, 0, sizeof(renderer->text_state));
            renderer->text_state_valid = false;
        }
        renderer->text_state_last_refresh_us = now_us;
        renderer->text_state_initialized = true;
    }
    return renderer->text_state_valid ? &renderer->text_state : NULL;
}

static int hud_png_scaled_x(const HudRenderer *renderer, int value)
{
    return renderer->offset_x + (int)lround((double)value * renderer->scale_x);
}

static int hud_png_scaled_y(const HudRenderer *renderer, int value)
{
    return renderer->offset_y + (int)lround((double)value * renderer->scale_y);
}

static uint32_t hud_png_scaled_size(const HudRenderer *renderer, int value)
{
    int result = (int)lround((double)value * renderer->scale_x);

    return result > 0 ? (uint32_t)result : 1U;
}

static HudRect hud_png_scaled_rect(const HudRenderer *renderer, HudRect rect)
{
    HudRect scaled;

    scaled.x = hud_png_scaled_x(renderer, rect.x);
    scaled.y = hud_png_scaled_y(renderer, rect.y);
    scaled.width = (uint32_t)(hud_png_scaled_x(
        renderer, rect.x + (int)rect.width) - scaled.x);
    scaled.height = (uint32_t)(hud_png_scaled_y(
        renderer, rect.y + (int)rect.height) - scaled.y);
    return scaled;
}

static void hud_png_set_regions(const HudRenderer *renderer, HudLayer *layer,
                                const HudRect *rects, size_t rect_count)
{
    size_t index;

    layer->region_count = rect_count > HUD_MAX_REGIONS ? HUD_MAX_REGIONS :
                          rect_count;
    for (index = 0U; index < layer->region_count; ++index) {
        HudRect scaled = hud_png_scaled_rect(renderer, rects[index]);

        if (scaled.x < 0 || scaled.y < 0 ||
            (uint32_t)scaled.x > layer->width ||
            (uint32_t)scaled.y > layer->height) {
            layer->regions[index] = (HudRegion){ 0U, 0U, 0U, 0U };
            continue;
        }
        layer->regions[index].x = (uint32_t)scaled.x;
        layer->regions[index].y = (uint32_t)scaled.y;
        layer->regions[index].width = scaled.width >
            layer->width - layer->regions[index].x ?
            layer->width - layer->regions[index].x : scaled.width;
        layer->regions[index].height = scaled.height >
            layer->height - layer->regions[index].y ?
            layer->height - layer->regions[index].y : scaled.height;
    }
}

static void hud_png_set_static_regions(const HudRenderer *renderer,
                                       HudLayer *layer)
{
    HudRect regions[] = {
        renderer->layout.static_frame,
        renderer->layout.left_top_panel,
        renderer->layout.right_top_panel,
        renderer->layout.rpm_arc,
        renderer->layout.speed_plate,
        renderer->layout.center_gear,
        renderer->layout.g_meter,
        renderer->layout.pedal_bar
    };

    hud_png_set_regions(renderer, layer, regions,
                        sizeof(regions) / sizeof(regions[0]));
}

static void hud_png_set_dynamic_regions(const HudRenderer *renderer,
                                        HudLayer *layer)
{
    HudRect regions[] = {
        /* Only restore pixels that can contain a changing glyph or fill.
         * The panel and instrument backgrounds remain in static_layer. */
        /* Cover the complete top-panel value band.  LAP now starts higher at
         * y=15 and TEMP extends lower at y=100; keep FreeType bearings inside
         * the restore region so neither change can leave a stale glyph. */
        { 145, 10, 90U, 120U },
        { 700, 10, 90U, 120U },
        { renderer->layout.rpm_arc.x,
          renderer->layout.rpm_arc.y + 55,
          renderer->layout.rpm_arc.width, 50U },
        { renderer->layout.speed_plate.x, renderer->layout.speed_plate.y,
          renderer->layout.speed_plate.width, renderer->layout.speed_plate.height },
        { renderer->layout.g_meter.x + 12,
          renderer->layout.g_meter.y + 12,
          renderer->layout.g_meter.width - 24U,
          renderer->layout.g_meter.height - 24U },
        { renderer->layout.center_gear.x + 16,
          renderer->layout.center_gear.y + 10,
          renderer->layout.center_gear.width - 10U, 64U },
        /* The RPM value is drawn below the gear value at y + 76.  It was
         * outside the old restore rectangle, so shorter/new glyphs were
         * composited over the previous frame and produced text ghosts. */
        { renderer->layout.center_gear.x + 16,
          renderer->layout.center_gear.y + 70,
          renderer->layout.center_gear.width - 10U, 36U },
        /* The two pedal fills have alpha bounds x=16..35/50..69 and
         * y=36..157 inside the 120x180 canvas at (668,408).  Include the
         * full bounds so a previous full bar cannot leave its top/left edge
         * behind when the next state is zero. */
        { 682, 440, 60U, 130U },
        { 246, 12, 100U, 28U }
    };

    hud_png_set_regions(renderer, layer, regions,
                        sizeof(regions) / sizeof(regions[0]));
}

static const HudImage *hud_png_asset(const HudRenderer *renderer,
                                     HudAssetId id)
{
    return hud_assets_get(&renderer->assets, id);
}

static bool hud_png_blit(const HudRenderer *renderer, HudLayer *layer,
                         HudAssetId id, HudRect destination)
{
    const HudImage *image = hud_png_asset(renderer, id);
    HudRect scaled;

    if (image == NULL) return true;
    scaled = hud_png_scaled_rect(renderer, destination);
    if (scaled.width == 0U || scaled.height == 0U) return false;
    return hud_blend_image(image, layer, scaled.x, scaled.y, scaled.width,
                           scaled.height);
}

static bool hud_png_blit_region(const HudRenderer *renderer, HudLayer *layer,
                                HudAssetId id, HudRect source,
                                HudRect destination)
{
    const HudImage *image = hud_png_asset(renderer, id);
    HudRect scaled;

    if (image == NULL || source.width == 0U || source.height == 0U) return true;
    scaled = hud_png_scaled_rect(renderer, destination);
    return hud_blend_image_region(image, layer, (uint32_t)source.x,
                                  (uint32_t)source.y, source.width,
                                  source.height, scaled.x, scaled.y,
                                  scaled.width, scaled.height);
}

static bool hud_png_surface_layer(HudLayer *layer, const HudSurface *surface)
{
    if (layer == NULL || surface == NULL || surface->pixels == NULL ||
        surface->width == 0U || surface->height == 0U ||
        surface->stride_bytes < surface->width * VIDEO_FRAME_CHANNELS) {
        return false;
    }
    memset(layer, 0, sizeof(*layer));
    layer->width = surface->width;
    layer->height = surface->height;
    layer->stride = surface->stride_bytes;
    layer->pixels = surface->pixels;
    layer->pixel_format = surface->pixel_format;
    return true;
}

static bool hud_png_render_static(HudRenderer *renderer)
{
    HudLayer *layer = &renderer->static_layer;

    if (layer->pixels == NULL) return false;
    memset(layer->pixels, 0, (size_t)layer->stride * layer->height);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_STATIC_FRAME,
                       renderer->layout.static_frame);
    /* The supplied artwork names describe their original canvas placement:
     * left_top_panel.png contains LAP/DELTA/TIMER/FLAG, while
     * right_top_panel.png contains SOC/HV/LV/TEMP.  The production semantic
     * layout is the opposite, so place the assets on the opposite sides. */
    (void)hud_png_blit(renderer, layer, HUD_ASSET_RIGHT_TOP_PANEL,
                       renderer->layout.left_top_panel);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_LEFT_TOP_PANEL,
                       renderer->layout.right_top_panel);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_RPM_ARC_BG,
                       renderer->layout.rpm_arc);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_NUMBER_HIGHLIGHT_PLATE,
                       renderer->layout.speed_plate);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_CENTER_GEAR_FRAME,
                       renderer->layout.center_gear);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_G_METER_BG,
                       renderer->layout.g_meter);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_PEDAL_BAR_BG,
                       renderer->layout.pedal_bar);
    hud_png_set_static_regions(renderer, layer);
    return true;
}

static void hud_png_apply_asset_geometry(HudRenderer *renderer)
{
    const HudImage *rpm_fill;

    if (renderer == NULL) return;
    rpm_fill = hud_png_asset(renderer, HUD_ASSET_RPM_ARC_FILL);
    if (rpm_fill != NULL && rpm_fill->alpha_max > 0U &&
        rpm_fill->alpha_x_max >= rpm_fill->alpha_x_min) {
        renderer->layout.rpm_content_x = rpm_fill->alpha_x_min;
        renderer->layout.rpm_content_width =
            rpm_fill->alpha_x_max - rpm_fill->alpha_x_min + 1U;
    }
}

static bool hud_png_valid(const VehicleState *state, uint32_t bit, double value)
{
    return state != NULL && (state->valid_mask & bit) != 0U && isfinite(value);
}

static double hud_png_clamp(double value, double minimum, double maximum)
{
    if (!isfinite(value) || value < minimum) return minimum;
    return value > maximum ? maximum : value;
}

static void hud_png_format_time(char *buffer, size_t size, bool valid,
                                double seconds)
{
    unsigned int minutes;
    unsigned int whole_seconds;
    unsigned int milliseconds;

    if (!valid || !isfinite(seconds) || seconds < 0.0) {
        (void)snprintf(buffer, size, "--:--.---");
        return;
    }
    seconds = hud_png_clamp(seconds, 0.0, 5999.999);
    minutes = (unsigned int)(seconds / 60.0);
    whole_seconds = (unsigned int)seconds % 60U;
    milliseconds = (unsigned int)lround((seconds - floor(seconds)) * 1000.0);
    if (milliseconds >= 1000U) {
        milliseconds = 0U;
        whole_seconds++;
    }
    (void)snprintf(buffer, size, "%02u:%02u.%03u", minutes,
                   whole_seconds, milliseconds);
}

static void hud_png_format_value(char *buffer, size_t size, bool valid,
                                 const char *format, double value,
                                 const char *suffix)
{
    if (!valid || !isfinite(value)) {
        (void)snprintf(buffer, size, "--");
        return;
    }
    (void)snprintf(buffer, size, format, value);
    if (suffix != NULL && suffix[0] != '\0') {
        size_t length = strlen(buffer);
        if (length < size) {
            (void)snprintf(buffer + length, size - length, "%s", suffix);
        }
    }
}

static int hud_png_text_baseline(HudRenderer *renderer, int y, const char *text,
                                 uint32_t pixel_size)
{
    HudTextMetrics metrics;

    if (text == NULL || !hud_text_measure(&renderer->text_atlas, text,
                                          pixel_size, &metrics)) {
        return hud_png_scaled_y(renderer, y);
    }
    /* Layout coordinates remain top anchors, while the font API receives a
     * baseline.  This is where FreeType's ascender is intentionally applied;
     * all glyphs in a row therefore share one baseline. */
    return hud_png_scaled_y(renderer, y) + metrics.ascent;
}

static void hud_png_draw_text(HudRenderer *renderer, HudLayer *layer,
                              int x, int y, const char *text, int size,
                              HudTextColor color)
{
    uint32_t pixel_size;

    if (size <= 0) return;
    pixel_size = hud_png_scaled_size(renderer, size);
    (void)hud_text_draw(&renderer->text_atlas, layer,
                        hud_png_scaled_x(renderer, x),
                        hud_png_text_baseline(renderer, y, text, pixel_size),
                        pixel_size, color, HUD_ALIGN_LEFT, text);
}

static void hud_png_draw_right_text(HudRenderer *renderer, HudLayer *layer,
                                    int right, int y, const char *text,
                                    int size, HudTextColor color)
{
    uint32_t pixel_size;

    if (size <= 0) return;
    pixel_size = hud_png_scaled_size(renderer, size);
    (void)hud_text_draw(&renderer->text_atlas, layer,
                        hud_png_scaled_x(renderer, right),
                        hud_png_text_baseline(renderer, y, text, pixel_size),
                        pixel_size, color, HUD_ALIGN_RIGHT, text);
}

static void hud_png_draw_center_text(HudRenderer *renderer, HudLayer *layer,
                                     int center, int y, const char *text,
                                     int size, HudTextColor color)
{
    uint32_t pixel_size;

    if (size <= 0) return;
    pixel_size = hud_png_scaled_size(renderer, size);
    (void)hud_text_draw(&renderer->text_atlas, layer,
                        hud_png_scaled_x(renderer, center),
                        hud_png_text_baseline(renderer, y, text, pixel_size),
                        pixel_size, color, HUD_ALIGN_CENTER, text);
}

static void hud_png_draw_icon(const HudRenderer *renderer, HudLayer *layer,
                              HudIconIndex icon, int x, int y, int size)
{
    HudRect source = { (int)icon * 64, 0, 64U, 64U };
    HudRect destination = { x, y, (uint32_t)size, (uint32_t)size };

    (void)hud_png_blit_region(renderer, layer, HUD_ASSET_WARNING_ICON_SET,
                              source, destination);
}

static const char *hud_png_drive_state(VehicleDriveState state)
{
    switch (state) {
    case VEHICLE_DRIVE_STATE_OFF: return "OFF";
    case VEHICLE_DRIVE_STATE_NEUTRAL: return "N";
    case VEHICLE_DRIVE_STATE_DRIVE: return "D";
    case VEHICLE_DRIVE_STATE_REVERSE: return "R";
    case VEHICLE_DRIVE_STATE_READY: return "RTD";
    case VEHICLE_DRIVE_STATE_UNKNOWN:
    default: return "--";
    }
}

static void hud_png_draw_rpm(const HudRenderer *renderer, HudLayer *layer,
                             const VehicleState *state)
{
    double ratio;
    uint32_t segment_count;
    uint32_t visible_width;
    HudRect destination = renderer->layout.rpm_arc;
    const HudRect *gear = &renderer->layout.center_gear;
    uint32_t gear_left;
    uint32_t gear_right;
    int gear_start;
    int gear_end;
    int rpm_start;
    int rpm_end;
    bool valid = hud_png_valid(state, VEHICLE_STATE_VALID_MOTOR_RPM,
                               state != NULL ? state->motor_rpm : 0.0);

    if (!valid || renderer->layout.rpm_max <= 0.0) return;
    ratio = hud_png_clamp(state->motor_rpm / renderer->layout.rpm_max, 0.0, 1.0);
    segment_count = (uint32_t)floor(ratio * renderer->layout.rpm_segments +
                                    0.000001);
    if (ratio > 0.0 && segment_count == 0U) segment_count = 1U;
    if (segment_count > renderer->layout.rpm_segments) {
        segment_count = renderer->layout.rpm_segments;
    }
    if (segment_count == 0U) return;
    visible_width = renderer->layout.rpm_content_x + (uint32_t)lround(
        (double)segment_count * renderer->layout.rpm_content_width /
        renderer->layout.rpm_segments);
    if (visible_width > destination.width) visible_width = destination.width;
    /* The supplied RPM fill shares the arc canvas with the gear frame. Keep
     * the center frame as a mask so a full RPM bar cannot paint over the
     * large gear value. */
    /* Mask only the intersection with the gear frame.  The gear frame is
     * normally inside the RPM canvas, but the production layout may place it
     * to the left with only a small overlap at the canvas boundary. */
    gear_start = gear->x;
    gear_end = gear_start + (int)gear->width;
    rpm_start = renderer->layout.rpm_arc.x;
    rpm_end = rpm_start + (int)renderer->layout.rpm_arc.width;
    if (gear_start < rpm_end && gear_end > rpm_start) {
        gear_left = gear_start > rpm_start ?
                    (uint32_t)(gear_start - rpm_start) : 0U;
        gear_right = gear_end < rpm_end ?
                     (uint32_t)(gear_end - rpm_start) :
                     (uint32_t)(rpm_end - rpm_start);
    } else {
        gear_left = 0U;
        gear_right = 0U;
    }
    if (gear_right == 0U || visible_width <= gear_left) {
        (void)hud_png_blit_region(renderer, layer, HUD_ASSET_RPM_ARC_FILL,
                                  (HudRect){ 0, 0, visible_width,
                                             renderer->layout.rpm_arc.height },
                                  (HudRect){ renderer->layout.rpm_arc.x,
                                             renderer->layout.rpm_arc.y,
                                             visible_width,
                                             renderer->layout.rpm_arc.height });
    } else {
        if (gear_left > 0U) {
            (void)hud_png_blit_region(renderer, layer, HUD_ASSET_RPM_ARC_FILL,
                                      (HudRect){ 0, 0, gear_left,
                                                 renderer->layout.rpm_arc.height },
                                      (HudRect){ renderer->layout.rpm_arc.x,
                                                 renderer->layout.rpm_arc.y,
                                                 gear_left,
                                                 renderer->layout.rpm_arc.height });
        }
        if (visible_width > gear_right) {
            uint32_t right_width = visible_width - gear_right;
            (void)hud_png_blit_region(renderer, layer, HUD_ASSET_RPM_ARC_FILL,
                                      (HudRect){ (int)gear_right, 0, right_width,
                                                 renderer->layout.rpm_arc.height },
                                      (HudRect){ renderer->layout.rpm_arc.x +
                                                     (int)gear_right,
                                                 renderer->layout.rpm_arc.y,
                                                 right_width,
                                                 renderer->layout.rpm_arc.height });
        }
    }
}

static void hud_png_draw_pedal(const HudRenderer *renderer, HudLayer *layer,
                               const VehicleState *state)
{
    double throttle = hud_png_valid(state,
        VEHICLE_STATE_VALID_THROTTLE_PERCENT,
        state != NULL ? state->throttle_percent : 0.0) ?
        state->throttle_percent / 100.0 : 0.0;
    double brake_value = state != NULL ? state->brake_percent : 0.0;
    bool brake_valid = hud_png_valid(state, VEHICLE_STATE_VALID_BRAKE_PERCENT,
                                     brake_value);
    double brake = brake_valid ?
        (renderer->layout.brake_input_is_pressure ?
         brake_value / renderer->layout.brake_pressure_max : brake_value / 100.0)
        : 0.0;
    {
        const HudImage *blue = hud_png_asset(renderer,
                                              HUD_ASSET_PEDAL_BAR_FILL_BLUE);
        const HudImage *red = hud_png_asset(renderer,
                                             HUD_ASSET_PEDAL_BAR_FILL_RED);
        const uint32_t image_height = renderer->layout.pedal_bar.height;
        const uint32_t segment_count = renderer->layout.pedal_segments;

        /* The V3 bar has transparent margins below/above the ten complete
         * cells.  Find those bounds from the loaded PNG and choose a cell
         * boundary, rather than cutting a cell or using the old V2 slot. */
        const HudImage *fills[] = { blue, red };
        const double values[] = { throttle, brake };
        const HudAssetId ids[] = { HUD_ASSET_PEDAL_BAR_FILL_BLUE,
                                   HUD_ASSET_PEDAL_BAR_FILL_RED };
        size_t fill_index;

        if (segment_count == 0U) return;
        for (fill_index = 0U; fill_index < 2U; ++fill_index) {
            const HudImage *fill = fills[fill_index];
            double normalized = hud_png_clamp(values[fill_index], 0.0, 1.0);
            uint32_t visible_segments;
            uint32_t active_top;
            uint32_t active_bottom;
            uint32_t source_y;
            uint32_t source_height;
            HudRect source;
            HudRect destination;

            if (fill == NULL || fill->alpha_max == 0U ||
                fill->alpha_y_max < fill->alpha_y_min) {
                continue;
            }
            visible_segments = (uint32_t)lround(normalized * segment_count);
            if (visible_segments == 0U) continue;
            if (visible_segments > segment_count) {
                visible_segments = segment_count;
            }
            active_top = fill->alpha_y_min < image_height ?
                         fill->alpha_y_min : image_height;
            active_bottom = fill->alpha_y_max + 1U < image_height ?
                            fill->alpha_y_max + 1U : image_height;
            if (active_bottom <= active_top) continue;
            source_y = active_top + (uint32_t)lround(
                (double)(segment_count - visible_segments) *
                (double)(active_bottom - active_top) / segment_count);
            if (source_y >= image_height) continue;
            source_height = image_height - source_y;
            if (fill->alpha_x_max < fill->alpha_x_min) continue;
            source = (HudRect){ (int)fill->alpha_x_min, (int)source_y,
                                fill->alpha_x_max - fill->alpha_x_min + 1U,
                                source_height };
            destination = (HudRect){
                renderer->layout.pedal_bar.x + (int)fill->alpha_x_min,
                renderer->layout.pedal_bar.y + (int)source_y,
                source.width, source.height
            };
            (void)hud_png_blit_region(renderer, layer, ids[fill_index], source,
                                      destination);
        }
    }
}

static void hud_png_draw_g(HudRenderer *renderer, HudLayer *layer,
                           const VehicleState *state)
{
    bool lateral_valid = hud_png_valid(state, VEHICLE_STATE_VALID_ACC_Y_MPS2,
                                       state != NULL ? state->acc_y : 0.0);
    bool longitudinal_valid = hud_png_valid(state,
        VEHICLE_STATE_VALID_ACC_X_MPS2, state != NULL ? state->acc_x : 0.0);
    double lateral = lateral_valid ? state->acc_y / 9.80665 : 0.0;
    double longitudinal = longitudinal_valid ? state->acc_x / 9.80665 : 0.0;
    double center_x;
    double center_y;
    int dot_x;
    int dot_y;
    const HudImage *dot_image;

    if (!lateral_valid && !longitudinal_valid) {
        renderer->filtered_g_initialized = false;
        return;
    }
    lateral = hud_png_clamp(lateral, -renderer->layout.g_max,
                            renderer->layout.g_max);
    longitudinal = hud_png_clamp(longitudinal, -renderer->layout.g_max,
                                 renderer->layout.g_max);
    if (!renderer->filtered_g_initialized) {
        renderer->filtered_lateral_g = lateral;
        renderer->filtered_longitudinal_g = longitudinal;
        renderer->filtered_g_initialized = true;
    } else {
        double alpha = hud_png_clamp(renderer->layout.g_filter_alpha, 0.0, 1.0);
        renderer->filtered_lateral_g += alpha *
            (lateral - renderer->filtered_lateral_g);
        renderer->filtered_longitudinal_g += alpha *
            (longitudinal - renderer->filtered_longitudinal_g);
    }
    center_x = (double)renderer->layout.g_meter.x +
               (double)renderer->layout.g_meter.width / 2.0;
    center_y = (double)renderer->layout.g_meter.y +
               (double)renderer->layout.g_meter.height / 2.0;
    dot_x = hud_png_scaled_x(renderer, (int)lround(center_x +
        hud_png_clamp(renderer->filtered_lateral_g / renderer->layout.g_max,
                      -1.0, 1.0) * renderer->layout.g_radius));
    dot_y = hud_png_scaled_y(renderer, (int)lround(center_y -
        hud_png_clamp(renderer->filtered_longitudinal_g / renderer->layout.g_max,
                      -1.0, 1.0) * renderer->layout.g_radius));
    dot_image = hud_png_asset(renderer, HUD_ASSET_G_METER_DOT);
    if (dot_image != NULL) {
        uint32_t dot_width = hud_png_scaled_size(renderer, dot_image->width);
        uint32_t dot_height = (uint32_t)lround(
            (double)dot_image->height * renderer->scale_y);
        if (dot_height == 0U) dot_height = 1U;
        (void)hud_blend_image(dot_image, layer,
                              dot_x - (int)dot_width / 2,
                              dot_y - (int)dot_height / 2,
                              dot_width, dot_height);
    }
}

static void hud_png_draw_status(HudRenderer *renderer, HudLayer *layer,
                                const VehicleState *state, bool recording,
                                uint64_t now_us)
{
    bool gps_ok = state != NULL && state->gps_valid &&
                  state->gps_fix_type >= 3U;
    bool can_ok = state != NULL && state->can_valid;

    if (recording) {
        (void)hud_png_blit(renderer, layer, HUD_ASSET_STATUS_PILL,
                           renderer->layout.status_pill);
        hud_png_draw_icon(renderer, layer, HUD_ICON_RECORDING, 251, 14, 24);
        hud_png_draw_text(renderer, layer, HUD_STATUS_TEXT_X,
                          HUD_STATUS_TEXT_Y, "REC",
                          renderer->layout.text_size_small, hud_png_red);
    } else if (!gps_ok && ((now_us / 500000ULL) & 1ULL) == 0ULL) {
        (void)hud_png_blit(renderer, layer, HUD_ASSET_STATUS_PILL,
                           renderer->layout.status_pill);
        hud_png_draw_icon(renderer, layer, HUD_ICON_GPS, 251, 14, 24);
        hud_png_draw_text(renderer, layer, HUD_STATUS_TEXT_X,
                          HUD_STATUS_TEXT_Y, "GPS",
                          renderer->layout.text_size_small, hud_png_yellow);
    } else if (!can_ok) {
        (void)hud_png_blit(renderer, layer, HUD_ASSET_STATUS_PILL,
                           renderer->layout.status_pill);
        hud_png_draw_icon(renderer, layer, HUD_ICON_CONNECTION, 251, 14, 24);
        hud_png_draw_text(renderer, layer, HUD_STATUS_TEXT_X,
                          HUD_STATUS_TEXT_Y, "BMS",
                          renderer->layout.text_size_small, hud_png_yellow);
    }
}

static void hud_png_render_values(HudRenderer *renderer, HudLayer *layer,
                                  const VehicleState *state, bool recording)
{
    char value[32];
    bool valid;
    const char *flag_state = "--";
    uint64_t now_us = hud_png_now_us();

    if (state != NULL && state->drive_state == VEHICLE_DRIVE_STATE_READY) {
        flag_state = "READY";
    }
    /* The left panel is the vehicle-status block. */
    valid = hud_png_valid(state, VEHICLE_STATE_VALID_HV_SOC_PERCENT,
                          state != NULL ? state->hv_soc_percent : 0.0);
    hud_png_format_value(value, sizeof(value), valid, "%.0f",
                         valid ? state->hv_soc_percent : 0.0, "%");
    hud_png_draw_right_text(renderer, layer, 219, HUD_TOP_PANEL_SOC_Y, value,
                            renderer->layout.text_size_value,
                            valid && state->hv_soc_percent < 20.0 ?
                            hud_png_red : hud_png_white);
    valid = hud_png_valid(state, VEHICLE_STATE_VALID_HV_VOLTAGE_V,
                          state != NULL ? state->hv_voltage_v : 0.0);
    hud_png_format_value(value, sizeof(value), valid, "%.0f",
                         valid ? state->hv_voltage_v : 0.0, "V");
    hud_png_draw_right_text(renderer, layer, 219, HUD_TOP_PANEL_HV_Y, value,
                            renderer->layout.text_size_value, hud_png_white);
    valid = hud_png_valid(state, VEHICLE_STATE_VALID_LV_VOLTAGE_V,
                          state != NULL ? state->lv_voltage_v : 0.0);
    hud_png_format_value(value, sizeof(value), valid, "%.1f",
                         valid ? state->lv_voltage_v : 0.0, "V");
    hud_png_draw_right_text(renderer, layer, 219, 74, value,
                            renderer->layout.text_size_value, hud_png_white);
    /* VehicleState currently has no motor-temperature field.  Keep the
     * supplied TEMP row honest instead of inventing a value. */
    hud_png_draw_right_text(renderer, layer, 219, HUD_TOP_PANEL_TEMP_Y, "-- C",
                            renderer->layout.text_size_value, hud_png_muted);

    /* The right panel is a direct view of VehicleState.  The HUD does not
     * decide whether lap data is trustworthy or complete; that policy belongs
     * to the state producer.  VehicleState currently exposes one lap number,
     * so do not invent a total-laps suffix here. */
    if (state != NULL) {
        (void)snprintf(value, sizeof(value), "%02u", state->lap_number);
    } else {
        (void)snprintf(value, sizeof(value), "--");
    }
    hud_png_draw_right_text(renderer, layer, 775, HUD_RIGHT_PANEL_LAP_Y, value,
                            renderer->layout.text_size_small, hud_png_white);
    valid = state != NULL && isfinite(state->delta_time_s);
    hud_png_format_value(value, sizeof(value), valid, "%+.2f",
                         valid ? state->delta_time_s : 0.0, "");
    hud_png_draw_right_text(renderer, layer, 775, HUD_RIGHT_PANEL_DELTA_Y, value,
                            renderer->layout.text_size_small,
                            !valid ? hud_png_muted :
                            (state->delta_time_s < 0.0 ? hud_png_green :
                             (state->delta_time_s > 0.0 ? hud_png_red :
                              hud_png_white)));
    hud_png_format_time(value, sizeof(value), state != NULL,
                        state != NULL ? state->lap_time_s : 0.0);
    hud_png_draw_right_text(renderer, layer, 775, HUD_RIGHT_PANEL_TIMER_Y, value,
                            renderer->layout.text_size_small, hud_png_white);
    hud_png_draw_right_text(renderer, layer, 775, 98, flag_state,
                            renderer->layout.text_size_small, hud_png_cyan);

    /* The number plate is deliberately text-only artwork.  Both the primary
     * speed and its unit are rendered from hud.ttf so no numeric label is
     * baked into the PNG layer. */
    valid = hud_png_valid(state, VEHICLE_STATE_VALID_SPEED_KMH,
                          state != NULL ? state->speed_kmh : 0.0);
    hud_png_draw_center_text(renderer, layer,
                             renderer->layout.speed_plate.x +
                                 (int)renderer->layout.speed_plate.width / 2,
                             renderer->layout.speed_plate.y + 1, "SPEED",
                             renderer->layout.text_size_small, hud_png_cyan);
    hud_png_format_value(value, sizeof(value), valid, "%.0f",
                         valid ? state->speed_kmh : 0.0, "");
    hud_png_draw_center_text(renderer, layer,
                             renderer->layout.speed_plate.x +
                                 (int)renderer->layout.speed_plate.width / 2,
                             renderer->layout.speed_plate.y + 10, value,
                             renderer->layout.text_size_speed,
                             valid ? hud_png_white : hud_png_muted);
    hud_png_draw_center_text(renderer, layer,
                             renderer->layout.speed_plate.x +
                                 (int)renderer->layout.speed_plate.width / 2,
                             renderer->layout.speed_plate.y + 45, "km/h",
                             renderer->layout.text_size_small, hud_png_muted);

    {
        const char *gear = state != NULL &&
            (state->valid_mask & VEHICLE_STATE_VALID_DRIVE_STATE) ?
            hud_png_drive_state(state->drive_state) : "--";
        hud_png_draw_center_text(renderer, layer,
                                 renderer->layout.center_gear.x +
                                     (int)renderer->layout.center_gear.width / 2,
                                 renderer->layout.center_gear.y + 2, gear,
                                 renderer->layout.text_size_gear, hud_png_white);
        valid = hud_png_valid(state, VEHICLE_STATE_VALID_MOTOR_RPM,
                              state != NULL ? state->motor_rpm : 0.0);
        hud_png_format_value(value, sizeof(value), valid, "%.0f",
                             valid ? state->motor_rpm : 0.0, " rpm");
        hud_png_draw_center_text(renderer, layer,
                                 renderer->layout.center_gear.x +
                                     (int)renderer->layout.center_gear.width / 2,
                                 renderer->layout.center_gear.y + 76, value,
                                 renderer->layout.text_size_rpm,
                                 valid ? hud_png_cyan : hud_png_muted);
    }
    hud_png_draw_status(renderer, layer, state, recording, now_us);
}

static bool hud_png_render_dynamic_layer(HudRenderer *renderer, HudLayer *layer,
                                         const VehicleState *state,
                                         bool recording, bool clear,
                                         HudRenderTiming *timing)
{
    const VehicleState *text_state;
    uint64_t stage_start;

    if (layer == NULL || layer->pixels == NULL) return false;
    if (clear) memset(layer->pixels, 0, (size_t)layer->stride * layer->height);
    text_state = hud_png_latched_text_state(renderer, state);
    stage_start = hud_png_now_us();
    hud_png_draw_rpm(renderer, layer, state);
    if (timing != NULL) timing->rpm_time_us = hud_png_now_us() - stage_start;
    stage_start = hud_png_now_us();
    hud_png_draw_pedal(renderer, layer, state);
    if (timing != NULL) timing->pedal_time_us = hud_png_now_us() - stage_start;
    stage_start = hud_png_now_us();
    hud_png_draw_g(renderer, layer, state);
    if (timing != NULL) timing->g_time_us = hud_png_now_us() - stage_start;
    stage_start = hud_png_now_us();
    hud_png_render_values(renderer, layer, text_state, recording);
    if (timing != NULL) timing->text_time_us = hud_png_now_us() - stage_start;
    hud_png_set_dynamic_regions(renderer, layer);
    return true;
}

int hud_renderer_init_style(HudRenderer *renderer, uint32_t width,
                            uint32_t height, HudStyle style,
                            const char *asset_dir,
                            const char *font_path,
                            HudPixelFormat surface_format)
{
    int result;
    const char *directory = asset_dir != NULL && asset_dir[0] != '\0' ?
                            asset_dir : HUD_DEFAULT_ASSET_DIR;
    const char *font = font_path != NULL && font_path[0] != '\0' ?
                       font_path : HUD_DEFAULT_FONT_PATH;

    if (style != HUD_STYLE_PNG && style != HUD_STYLE_LEGACY &&
        style != HUD_STYLE_NONE) {
        errno = EINVAL;
        return -1;
    }
    result = surface_format == HUD_PIXEL_FORMAT_ARGB8888 ?
        hud_renderer_init_external(renderer, width, height) :
        hud_renderer_init(renderer, width, height);
    if (result < 0) return result;
    renderer->style = style;
    if (style == HUD_STYLE_LEGACY) return 0;
    hud_layout_default(&renderer->layout);
    if (style == HUD_STYLE_NONE) {
        memset(renderer->static_layer.pixels, 0,
               (size_t)renderer->static_layer.stride * renderer->height);
        if (renderer->dynamic_layer.pixels != NULL) {
            memset(renderer->dynamic_layer.pixels, 0,
                   (size_t)renderer->dynamic_layer.stride * renderer->height);
        }
        return 0;
    }
    if (hud_assets_init(&renderer->assets, directory, surface_format,
                        false) < 0) {
        fprintf(stderr, "HUD PNG: resource initialization failed; using text-only fallback\n");
    }
    if (hud_text_atlas_init(&renderer->text_atlas, font) < 0) {
        fprintf(stderr, "[HUD][FONT] PNG HUD initialization aborted; text is required\n");
        hud_renderer_close(renderer);
        return -1;
    }
    {
        const uint32_t text_sizes[] = {
            (uint32_t)renderer->layout.text_size_small,
            (uint32_t)renderer->layout.text_size_value,
            (uint32_t)renderer->layout.text_size_speed,
            (uint32_t)renderer->layout.text_size_rpm,
            (uint32_t)renderer->layout.text_size_gear
        };

        if (!hud_text_atlas_warm(&renderer->text_atlas, text_sizes,
                                 sizeof(text_sizes) / sizeof(text_sizes[0]))) {
            fprintf(stderr, "[HUD][FONT] Failed to warm glyph cache for %s\n",
                    font);
            hud_renderer_close(renderer);
            return -1;
        }
    }
    hud_png_apply_asset_geometry(renderer);
    if (!hud_png_render_static(renderer)) {
        fprintf(stderr, "HUD PNG: static template initialization failed; using empty HUD\n");
        memset(renderer->static_layer.pixels, 0,
               (size_t)renderer->static_layer.stride * renderer->height);
        hud_png_set_static_regions(renderer, &renderer->static_layer);
    }
    return 0;
}

const char *hud_style_name(HudStyle style)
{
    switch (style) {
    case HUD_STYLE_PNG: return "png";
    case HUD_STYLE_LEGACY: return "legacy";
    case HUD_STYLE_NONE: return "none";
    default: return "unknown";
    }
}

bool hud_style_parse(const char *value, HudStyle *style)
{
    if (value == NULL || style == NULL) return false;
    if (strcmp(value, "png") == 0) *style = HUD_STYLE_PNG;
    else if (strcmp(value, "legacy") == 0) *style = HUD_STYLE_LEGACY;
    else if (strcmp(value, "none") == 0) *style = HUD_STYLE_NONE;
    else return false;
    return true;
}

size_t hud_renderer_asset_memory(const HudRenderer *renderer)
{
    return renderer != NULL ? hud_assets_resident_bytes(&renderer->assets) : 0U;
}

size_t hud_renderer_resident_memory(const HudRenderer *renderer)
{
    size_t bytes;

    if (renderer == NULL) return 0U;
    bytes = hud_renderer_asset_memory(renderer) + renderer->text_atlas.bytes;
    if (renderer->static_layer.pixels != NULL) {
        bytes += (size_t)renderer->static_layer.stride *
                 renderer->static_layer.height;
    }
    if (renderer->dynamic_layer.pixels != NULL) {
        bytes += (size_t)renderer->dynamic_layer.stride *
                 renderer->dynamic_layer.height;
    }
    return bytes;
}

bool hud_renderer_png_assets_ready(const HudRenderer *renderer)
{
    return renderer != NULL && renderer->style == HUD_STYLE_PNG &&
           hud_assets_core_ready(&renderer->assets);
}

const HudLayer *hud_png_render_dynamic(HudRenderer *renderer,
                                       const VehicleState *state,
                                       bool recording)
{
    if (renderer == NULL || renderer->dynamic_layer.pixels == NULL) return NULL;
    return hud_png_render_dynamic_layer(renderer, &renderer->dynamic_layer,
                                        state, recording, true, NULL) ?
           &renderer->dynamic_layer : NULL;
}

const HudLayer *hud_png_render_dynamic_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing)
{
    HudLayer *layer = &renderer->external_dynamic_layer;
    uint64_t start = hud_png_now_us();

    if (!hud_png_surface_layer(layer, surface)) return NULL;
    if (timing != NULL) memset(timing, 0, sizeof(*timing));
    memset(layer->pixels, 0, (size_t)layer->stride * layer->height);
    if (timing != NULL) timing->clear_time_us = hud_png_now_us() - start;
    start = hud_png_now_us();
    if (!hud_png_render_dynamic_layer(renderer, layer, state, recording, false,
                                      timing)) {
        return NULL;
    }
    layer->region_count = 1U;
    layer->regions[0] = (HudRegion){ 0U, 0U, layer->width, layer->height };
    if (timing != NULL) {
        timing->draw_time_us = hud_png_now_us() - start;
        timing->total_time_us += timing->clear_time_us + timing->draw_time_us;
    }
    return layer;
}

const HudLayer *hud_png_render_frame_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing)
{
    HudLayer *layer = &renderer->external_dynamic_layer;
    uint64_t total_start = hud_png_now_us();
    uint64_t start;
    size_t index;

    if (!hud_png_surface_layer(layer, surface)) return NULL;
    if (timing != NULL) memset(timing, 0, sizeof(*timing));
    start = hud_png_now_us();
    if (!renderer->external_surface_initialized ||
        renderer->external_surface_pixels != surface->pixels) {
        uint32_t row;

        for (row = 0U; row < renderer->height; ++row) {
            memcpy(layer->pixels + (size_t)row * layer->stride,
                   renderer->static_layer.pixels +
                       (size_t)row * renderer->static_layer.stride,
                   (size_t)renderer->width * VIDEO_FRAME_CHANNELS);
        }
        renderer->external_surface_pixels = surface->pixels;
        renderer->external_surface_initialized = true;
    } else {
        hud_png_set_dynamic_regions(renderer, layer);
        for (index = 0U; index < layer->region_count; ++index) {
            const HudRegion *region = &layer->regions[index];
            uint32_t row;

            for (row = region->y; row < region->y + region->height; ++row) {
                memcpy(layer->pixels + (size_t)row * layer->stride +
                           (size_t)region->x * VIDEO_FRAME_CHANNELS,
                       renderer->static_layer.pixels +
                           (size_t)row * renderer->static_layer.stride +
                           (size_t)region->x * VIDEO_FRAME_CHANNELS,
                       (size_t)region->width * VIDEO_FRAME_CHANNELS);
            }
        }
    }
    if (timing != NULL) timing->clear_time_us = hud_png_now_us() - start;
    start = hud_png_now_us();
    if (!hud_png_render_dynamic_layer(renderer, layer, state, recording, false,
                                      timing)) {
        return NULL;
    }
    layer->region_count = 1U;
    layer->regions[0] = (HudRegion){ 0U, 0U, layer->width, layer->height };
    if (timing != NULL) {
        timing->draw_time_us = hud_png_now_us() - start;
        timing->total_time_us = hud_png_now_us() - total_start;
    }
    return layer;
}

const HudLayer *hud_none_render_dynamic(HudRenderer *renderer,
                                        const VehicleState *state,
                                        bool recording)
{
    (void)state;
    (void)recording;
    if (renderer == NULL || renderer->dynamic_layer.pixels == NULL) return NULL;
    memset(renderer->dynamic_layer.pixels, 0,
           (size_t)renderer->dynamic_layer.stride * renderer->height);
    renderer->dynamic_layer.region_count = 0U;
    return &renderer->dynamic_layer;
}

const HudLayer *hud_none_render_frame_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing)
{
    (void)state;
    (void)recording;
    if (renderer == NULL || surface == NULL || surface->pixels == NULL ||
        surface->width != renderer->width || surface->height != renderer->height) {
        return NULL;
    }
    memset(surface->pixels, 0, (size_t)surface->stride_bytes * surface->height);
    if (timing != NULL) memset(timing, 0, sizeof(*timing));
    renderer->external_dynamic_layer = (HudLayer){
        surface->width, surface->height, surface->stride_bytes, surface->pixels,
        surface->pixel_format, { { 0U, 0U, 0U, 0U } }, 0U
    };
    renderer->external_dynamic_layer.region_count = 1U;
    renderer->external_dynamic_layer.regions[0] =
        (HudRegion){ 0U, 0U, surface->width, surface->height };
    return &renderer->external_dynamic_layer;
}
