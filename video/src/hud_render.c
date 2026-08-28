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
    static const HudRect regions[] = {
        { 0, 0, 800U, 600U },
        { 12, 12, 220U, 120U },
        { 568, 12, 220U, 120U },
        { 140, 430, 520U, 170U },
        { 320, 480, 160U, 110U },
        { 12, 428, 160U, 160U },
        { 668, 408, 120U, 180U }
    };

    hud_png_set_regions(renderer, layer, regions,
                        sizeof(regions) / sizeof(regions[0]));
}

static void hud_png_set_dynamic_regions(const HudRenderer *renderer,
                                        HudLayer *layer)
{
    static const HudRect regions[] = {
        /* Only restore pixels that can contain a changing glyph or fill.
         * The panel and instrument backgrounds remain in static_layer. */
        { 155, 24, 77U, 88U },
        { 708, 24, 72U, 90U },
        { 164, 495, 473U, 27U },
        { 24, 440, 136U, 136U },
        { 336, 490, 150U, 64U },
        { 686, 448, 58U, 140U },
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
    (void)hud_png_blit(renderer, layer, HUD_ASSET_LEFT_TOP_PANEL,
                       renderer->layout.left_top_panel);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_RIGHT_TOP_PANEL,
                       renderer->layout.right_top_panel);
    (void)hud_png_blit(renderer, layer, HUD_ASSET_RPM_ARC_BG,
                       renderer->layout.rpm_arc);
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

static void hud_png_draw_text(const HudRenderer *renderer, HudLayer *layer,
                              int x, int y, const char *text, int size,
                              HudTextColor color)
{
    if (size > 0) {
        (void)hud_text_draw(&renderer->text_atlas, layer,
                            hud_png_scaled_x(renderer, x),
                            hud_png_scaled_y(renderer, y), text,
                            hud_png_scaled_size(renderer, size), color);
    }
}

static void hud_png_draw_right_text(const HudRenderer *renderer, HudLayer *layer,
                                    int right, int y, const char *text,
                                    int size, HudTextColor color)
{
    uint32_t width = hud_text_width(&renderer->text_atlas, text,
                                    hud_png_scaled_size(renderer, size));
    int x = hud_png_scaled_x(renderer, right) - (int)width;

    (void)hud_text_draw(&renderer->text_atlas, layer, x,
                        hud_png_scaled_y(renderer, y), text,
                        hud_png_scaled_size(renderer, size), color);
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
    gear_left = gear->x > renderer->layout.rpm_arc.x ?
                (uint32_t)(gear->x - renderer->layout.rpm_arc.x) : 0U;
    gear_right = gear_left + gear->width;
    if (visible_width <= gear_left) {
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
    center_x = (double)renderer->layout.g_meter.x + 80.0;
    center_y = (double)renderer->layout.g_meter.y + 80.0;
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

static void hud_png_draw_status(const HudRenderer *renderer, HudLayer *layer,
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
        hud_png_draw_text(renderer, layer, 278, 22, "REC",
                          renderer->layout.text_size_small, hud_png_red);
    } else if (!gps_ok && ((now_us / 500000ULL) & 1ULL) == 0ULL) {
        (void)hud_png_blit(renderer, layer, HUD_ASSET_STATUS_PILL,
                           renderer->layout.status_pill);
        hud_png_draw_icon(renderer, layer, HUD_ICON_GPS, 251, 14, 24);
        hud_png_draw_text(renderer, layer, 278, 22, "GPS",
                          renderer->layout.text_size_small, hud_png_yellow);
    } else if (!can_ok) {
        (void)hud_png_blit(renderer, layer, HUD_ASSET_STATUS_PILL,
                           renderer->layout.status_pill);
        hud_png_draw_icon(renderer, layer, HUD_ICON_CONNECTION, 251, 14, 24);
        hud_png_draw_text(renderer, layer, 278, 22, "BMS",
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
    (void)snprintf(value, sizeof(value), "%02u / --",
                   state != NULL && state->lap_valid ? state->lap_number : 0U);
    if (state == NULL || !state->lap_valid) (void)snprintf(value, sizeof(value), "-- / --");
    hud_png_draw_right_text(renderer, layer, 219, 29, value,
                            renderer->layout.text_size_small, hud_png_white);
    valid = state != NULL && state->lap_valid && isfinite(state->delta_time_s);
    hud_png_format_value(value, sizeof(value), valid, "%+.2f",
                         valid ? state->delta_time_s : 0.0, "");
    hud_png_draw_right_text(renderer, layer, 219, 52, value,
                            renderer->layout.text_size_small,
                            !valid ? hud_png_muted :
                            (state->delta_time_s < 0.0 ? hud_png_green :
                             (state->delta_time_s > 0.0 ? hud_png_red :
                              hud_png_white)));
    hud_png_format_time(value, sizeof(value), state != NULL && state->lap_valid,
                        state != NULL ? state->lap_time_s : 0.0);
    hud_png_draw_right_text(renderer, layer, 219, 75, value,
                            renderer->layout.text_size_small, hud_png_white);
    hud_png_draw_right_text(renderer, layer, 219, 98, flag_state,
                            renderer->layout.text_size_small, hud_png_cyan);

    valid = hud_png_valid(state, VEHICLE_STATE_VALID_HV_SOC_PERCENT,
                          state != NULL ? state->hv_soc_percent : 0.0);
    hud_png_format_value(value, sizeof(value), valid, "%.0f",
                         valid ? state->hv_soc_percent : 0.0, "%");
    hud_png_draw_right_text(renderer, layer, 775, 28, value,
                            renderer->layout.text_size_value,
                            valid && state->hv_soc_percent < 20.0 ?
                            hud_png_red : hud_png_white);
    valid = hud_png_valid(state, VEHICLE_STATE_VALID_HV_VOLTAGE_V,
                          state != NULL ? state->hv_voltage_v : 0.0);
    hud_png_format_value(value, sizeof(value), valid, "%.0f",
                         valid ? state->hv_voltage_v : 0.0, "V");
    hud_png_draw_right_text(renderer, layer, 775, 51, value,
                            renderer->layout.text_size_value, hud_png_white);
    valid = hud_png_valid(state, VEHICLE_STATE_VALID_LV_VOLTAGE_V,
                          state != NULL ? state->lv_voltage_v : 0.0);
    hud_png_format_value(value, sizeof(value), valid, "%.1f",
                         valid ? state->lv_voltage_v : 0.0, "V");
    hud_png_draw_right_text(renderer, layer, 775, 74, value,
                            renderer->layout.text_size_value, hud_png_white);
    /* VehicleState currently has no motor-temperature field.  Keep the
     * supplied TEMP row honest instead of inventing a value. */
    hud_png_draw_right_text(renderer, layer, 775, 97, "-- C",
                            renderer->layout.text_size_value, hud_png_muted);

    {
        const char *gear = state != NULL &&
            (state->valid_mask & VEHICLE_STATE_VALID_DRIVE_STATE) ?
            hud_png_drive_state(state->drive_state) : "--";
        size_t gear_length = strlen(gear);
        int gear_x = gear_length >= 3U ? 340 :
                     (gear_length == 2U ? 360 : 380);
        hud_png_draw_text(renderer, layer, gear_x, 494, gear,
                          renderer->layout.text_size_gear, hud_png_white);
    }
    hud_png_draw_status(renderer, layer, state, recording, now_us);
}

static bool hud_png_render_dynamic_layer(HudRenderer *renderer, HudLayer *layer,
                                         const VehicleState *state,
                                         bool recording, bool clear,
                                         HudRenderTiming *timing)
{
    uint64_t stage_start;

    if (layer == NULL || layer->pixels == NULL) return false;
    if (clear) memset(layer->pixels, 0, (size_t)layer->stride * layer->height);
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
    hud_png_render_values(renderer, layer, state, recording);
    if (timing != NULL) timing->text_time_us = hud_png_now_us() - stage_start;
    hud_png_set_dynamic_regions(renderer, layer);
    return true;
}

int hud_renderer_init_style(HudRenderer *renderer, uint32_t width,
                            uint32_t height, HudStyle style,
                            const char *asset_dir,
                            HudPixelFormat surface_format)
{
    int result;
    const char *directory = asset_dir != NULL && asset_dir[0] != '\0' ?
                            asset_dir : "assets/hud";

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
    if (hud_text_atlas_init(&renderer->text_atlas) < 0) {
        fprintf(stderr, "HUD PNG: glyph atlas initialization failed; text disabled\n");
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
