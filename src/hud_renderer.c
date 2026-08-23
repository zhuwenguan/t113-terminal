#define _POSIX_C_SOURCE 200809L

#include "hud_renderer.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} HudColor;

static const HudColor hud_cyan = { 44U, 210U, 232U, 235U };
static const HudColor hud_white = { 245U, 249U, 252U, 245U };
static const HudColor hud_muted = { 143U, 180U, 190U, 235U };
static const HudColor hud_green = { 64U, 230U, 122U, 245U };
static const HudColor hud_red = { 255U, 66U, 72U, 245U };
static const HudColor hud_yellow = { 255U, 210U, 64U, 245U };
static const HudColor hud_black_panel = { 0U, 0U, 0U, 166U };

/* Compact 5x7 font. It intentionally uses a fixed-width ASCII alphabet so
 * changing telemetry values never moves the surrounding layout. */
static const uint8_t hud_font[36][7] = {
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, /* 0 */
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, /* 1 */
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, /* 2 */
    {0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e}, /* 3 */
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, /* 4 */
    {0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e}, /* 5 */
    {0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e}, /* 6 */
    {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, /* 8 */
    {0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e}, /* 9 */
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, /* A */
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, /* B */
    {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, /* C */
    {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}, /* D */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, /* E */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, /* F */
    {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}, /* G */
    {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, /* H */
    {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}, /* I */
    {0x07,0x02,0x02,0x02,0x12,0x12,0x0c}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, /* L */
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, /* O */
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}, /* P */
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, /* Q */
    {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, /* R */
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, /* S */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0a,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, /* W */
    {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, /* X */
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, /* Y */
    {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}  /* Z */
};

static const uint8_t hud_dot[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x04};
static const uint8_t hud_colon[7] = {0x00,0x04,0x00,0x00,0x00,0x04,0x00};
static const uint8_t hud_minus[7] = {0x00,0x00,0x00,0x1f,0x00,0x00,0x00};
static const uint8_t hud_plus[7] = {0x00,0x04,0x04,0x1f,0x04,0x04,0x00};
static const uint8_t hud_percent[7] = {0x19,0x19,0x02,0x04,0x08,0x13,0x13};
static const uint8_t hud_slash[7] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00};

static int hud_scaled_x(const HudRenderer *renderer, int value)
{
    return (int)lround((double)value * renderer->scale_x);
}

static int hud_scaled_y(const HudRenderer *renderer, int value)
{
    return (int)lround((double)value * renderer->scale_y);
}

static int hud_font_scale(const HudRenderer *renderer, int scale)
{
    int result = (int)lround((double)scale * renderer->scale_x);
    return result < 1 ? 1 : result;
}

static uint64_t hud_renderer_now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0U;
    }
    return (uint64_t)timestamp.tv_sec * 1000000U +
           (uint64_t)timestamp.tv_nsec / 1000U;
}

static void hud_layer_clear(HudLayer *layer)
{
    if (layer != NULL && layer->pixels != NULL) {
        memset(layer->pixels, 0, (size_t)layer->stride * layer->height);
        layer->region_count = 0U;
    }
}

static void hud_layer_set_regions_from_base(const HudRenderer *renderer,
                                            HudLayer *layer,
                                            const HudRegion *base_regions,
                                            size_t base_count)
{
    size_t index;

    layer->region_count = base_count > HUD_MAX_REGIONS ? HUD_MAX_REGIONS :
                          base_count;
    for (index = 0U; index < layer->region_count; ++index) {
        layer->regions[index].x = (uint32_t)hud_scaled_x(
            renderer, (int)base_regions[index].x);
        layer->regions[index].y = (uint32_t)hud_scaled_y(
            renderer, (int)base_regions[index].y);
        layer->regions[index].width = (uint32_t)hud_scaled_x(
            renderer, (int)(base_regions[index].x + base_regions[index].width)) -
            layer->regions[index].x;
        layer->regions[index].height = (uint32_t)hud_scaled_y(
            renderer, (int)(base_regions[index].y + base_regions[index].height)) -
            layer->regions[index].y;
    }
}

static void hud_layer_set_static_regions(const HudRenderer *renderer,
                                         HudLayer *layer)
{
    static const HudRegion regions[] = {
        { 20U, 10U, 240U, 40U },
        { 24U, 64U, 252U, 354U },
        { 988U, 30U, 268U, 180U },
        { 988U, 237U, 268U, 269U },
        { 24U, 535U, 350U, 157U },
        { 460U, 535U, 360U, 157U },
        { 850U, 535U, 406U, 157U }
    };

    hud_layer_set_regions_from_base(renderer, layer, regions,
                                     sizeof(regions) / sizeof(regions[0]));
}

static void hud_layer_set_dynamic_regions(const HudRenderer *renderer,
                                          HudLayer *layer)
{
    static const HudRegion regions[] = {
        { 20U, 10U, 240U, 40U },
        { 35U, 95U, 225U, 275U },
        { 1105U, 55U, 140U, 100U },
        { 1100U, 250U, 125U, 210U },
        { 1100U, 465U, 110U, 30U },
        { 120U, 548U, 220U, 95U },
        { 520U, 550U, 230U, 90U },
        { 860U, 550U, 350U, 70U }
    };

    hud_layer_set_regions_from_base(renderer, layer, regions,
                                     sizeof(regions) / sizeof(regions[0]));
}

static void hud_pixel(HudLayer *layer, int x, int y, HudColor color)
{
    uint8_t *pixel;

    if (layer == NULL || layer->pixels == NULL || x < 0 || y < 0 ||
        (uint32_t)x >= layer->width || (uint32_t)y >= layer->height ||
        color.a == 0U) {
        return;
    }
    pixel = layer->pixels + (size_t)y * layer->stride +
            (size_t)x * VIDEO_FRAME_CHANNELS;
    if (layer->pixel_format == HUD_PIXEL_FORMAT_ARGB8888) {
        /* The T113 G2D ARGB8888 dma-buf is little-endian B,G,R,A. */
        pixel[0] = color.b;
        pixel[1] = color.g;
        pixel[2] = color.r;
    } else {
        pixel[0] = color.r;
        pixel[1] = color.g;
        pixel[2] = color.b;
    }
    pixel[3] = color.a;
}

static void hud_fill_rect(const HudRenderer *renderer, HudLayer *layer,
                          int x, int y, int width, int height,
                          HudColor color)
{
    int left = hud_scaled_x(renderer, x);
    int top = hud_scaled_y(renderer, y);
    int right = hud_scaled_x(renderer, x + width);
    int bottom = hud_scaled_y(renderer, y + height);
    int row;

    for (row = top; row < bottom; ++row) {
        int column;
        for (column = left; column < right; ++column) {
            hud_pixel(layer, column, row, color);
        }
    }
}

static void hud_outline_rect(const HudRenderer *renderer, HudLayer *layer,
                             int x, int y, int width, int height,
                             HudColor color)
{
    hud_fill_rect(renderer, layer, x, y, width, 1, color);
    hud_fill_rect(renderer, layer, x, y + height - 1, width, 1, color);
    hud_fill_rect(renderer, layer, x, y, 1, height, color);
    hud_fill_rect(renderer, layer, x + width - 1, y, 1, height, color);
}

static const uint8_t *hud_glyph(char character)
{
    unsigned int index;

    if (character >= '0' && character <= '9') {
        index = (unsigned int)(character - '0');
    } else if (character >= 'A' && character <= 'Z') {
        index = 10U + (unsigned int)(character - 'A');
    } else {
        switch (character) {
        case '.': return hud_dot;
        case ':': return hud_colon;
        case '-': return hud_minus;
        case '+': return hud_plus;
        case '%': return hud_percent;
        case '/': return hud_slash;
        default: return NULL;
        }
    }
    return hud_font[index];
}

static int hud_draw_text(const HudRenderer *renderer, HudLayer *layer,
                         int x, int y, const char *text, int scale,
                         HudColor color)
{
    const int actual_scale = hud_font_scale(renderer, scale);
    const int advance = 6 * actual_scale;
    int cursor = hud_scaled_x(renderer, x);
    int top = hud_scaled_y(renderer, y);

    if (text == NULL) {
        return cursor;
    }
    while (*text != '\0') {
        const uint8_t *glyph = hud_glyph(*text);
        int glyph_row;

        if (glyph != NULL) {
            for (glyph_row = 0; glyph_row < 7; ++glyph_row) {
                int glyph_column;
                for (glyph_column = 0; glyph_column < 5; ++glyph_column) {
                    if ((glyph[glyph_row] & (1U << (4 - glyph_column))) != 0U) {
                        int yy;
                        int xx;
                        for (yy = 0; yy < actual_scale; ++yy) {
                            for (xx = 0; xx < actual_scale; ++xx) {
                                hud_pixel(layer, cursor + glyph_column * actual_scale +
                                          xx, top + glyph_row * actual_scale + yy,
                                          color);
                            }
                        }
                    }
                }
            }
        }
        cursor += (*text == ' ' ? advance : advance);
        ++text;
    }
    return cursor;
}

static void hud_draw_circle(const HudRenderer *renderer, HudLayer *layer,
                            int center_x, int center_y, int radius,
                            HudColor color)
{
    int y;

    for (y = -radius; y <= radius; ++y) {
        int x;
        int limit = (int)sqrt((double)radius * radius - (double)y * y);
        for (x = -limit; x <= limit; ++x) {
            int distance = x * x + y * y;
            int radius_squared = radius * radius;
            if (distance >= radius_squared - radius * 2 &&
                distance <= radius_squared + radius * 2) {
                hud_pixel(layer, hud_scaled_x(renderer, center_x + x),
                          hud_scaled_y(renderer, center_y + y), color);
            }
        }
    }
}

static void hud_draw_line(const HudRenderer *renderer, HudLayer *layer,
                          int x1, int y1, int x2, int y2, HudColor color)
{
    int dx = abs(x2 - x1);
    int sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1);
    int sy = y1 < y2 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        hud_pixel(layer, hud_scaled_x(renderer, x1), hud_scaled_y(renderer, y1),
                  color);
        if (x1 == x2 && y1 == y2) {
            break;
        }
        if (2 * error >= dy) {
            error += dy;
            x1 += sx;
        }
        if (2 * error <= dx) {
            error += dx;
            y1 += sy;
        }
    }
}

static void hud_panel(const HudRenderer *renderer, HudLayer *layer,
                      int x, int y, int width, int height)
{
    hud_fill_rect(renderer, layer, x, y, width, height, hud_black_panel);
    hud_outline_rect(renderer, layer, x, y, width, height, hud_cyan);
}

static void hud_static_text(const HudRenderer *renderer, HudLayer *layer,
                            int x, int y, const char *text)
{
    (void)hud_draw_text(renderer, layer, x, y, text, 1, hud_cyan);
}

static void hud_draw_segment_outline(const HudRenderer *renderer, HudLayer *layer,
                                     int x, int y, int count, int width,
                                     int height)
{
    int index;

    for (index = 0; index < count; ++index) {
        int left = x + index * (width + 3);
        hud_outline_rect(renderer, layer, left, y, width, height, hud_muted);
    }
}

static bool hud_has(const VehicleState *state, uint32_t bit)
{
    return state != NULL && (state->valid_mask & bit) == bit;
}

static bool hud_finite(const VehicleState *state, uint32_t bit, double value)
{
    return hud_has(state, bit) && isfinite(value);
}

static void hud_format_number(char *buffer, size_t buffer_size,
                              bool valid, double value, const char *format,
                              const char *unit)
{
    if (!valid || !isfinite(value)) {
        (void)snprintf(buffer, buffer_size, "--");
    } else {
        (void)snprintf(buffer, buffer_size, format, value);
        if (unit != NULL) {
            size_t length = strlen(buffer);
            (void)snprintf(buffer + length, buffer_size > length ?
                           buffer_size - length : 0U, " %s", unit);
        }
    }
}

static void hud_format_time(char *buffer, size_t buffer_size, bool valid,
                            double seconds)
{
    unsigned int minutes;
    unsigned int whole_seconds;
    unsigned int milliseconds;

    if (!valid || !isfinite(seconds) || seconds < 0.0) {
        (void)snprintf(buffer, buffer_size, "--");
        return;
    }
    if (seconds > 5999.999) {
        seconds = 5999.999;
    }
    minutes = (unsigned int)(seconds / 60.0);
    whole_seconds = (unsigned int)seconds % 60U;
    milliseconds = (unsigned int)lround((seconds - floor(seconds)) * 1000.0);
    if (milliseconds >= 1000U) {
        milliseconds = 0U;
        whole_seconds++;
    }
    (void)snprintf(buffer, buffer_size, "%02u:%02u.%03u", minutes,
                   whole_seconds, milliseconds);
}

static double hud_clamp_percent(double value)
{
    if (!isfinite(value) || value < 0.0) {
        return 0.0;
    }
    return value > 100.0 ? 100.0 : value;
}

static void hud_draw_progress(const HudRenderer *renderer, HudLayer *layer,
                              int x, int y, int count, int width, int height,
                              double percent, HudColor color, bool valid)
{
    int index;
    double clamped = hud_clamp_percent(percent);

    for (index = 0; index < count; ++index) {
        int left = x + index * (width + 3);
        double threshold = ((double)(index + 1) / (double)count) * 100.0;
        if (valid && clamped >= threshold - 0.001) {
            hud_fill_rect(renderer, layer, left + 1, y + 1, width - 2,
                          height - 2, color);
        }
    }
}

static const char *hud_drive_state_name(VehicleDriveState state)
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

static void hud_render_static(HudRenderer *renderer)
{
    HudLayer *layer = &renderer->static_layer;
    int index;

    hud_layer_clear(layer);
    /* Top status line intentionally has no panel to keep the road visible. */
    hud_panel(renderer, layer, 24, 64, 252, 354);
    hud_static_text(renderer, layer, 40, 76, "ELECTRIC SYSTEM");
    hud_static_text(renderer, layer, 40, 100, "MOTOR RPM");
    hud_static_text(renderer, layer, 40, 127, "TORQUE");
    hud_fill_rect(renderer, layer, 40, 150, 220, 1, hud_cyan);
    hud_static_text(renderer, layer, 40, 164, "HV VOLT");
    hud_static_text(renderer, layer, 40, 191, "HV CURR");
    hud_static_text(renderer, layer, 40, 218, "HV POWER");
    hud_fill_rect(renderer, layer, 40, 245, 220, 1, hud_cyan);
    hud_static_text(renderer, layer, 40, 258, "SOC");
    hud_draw_segment_outline(renderer, layer, 40, 310, 16, 12, 12);
    hud_static_text(renderer, layer, 40, 340, "LV VOLT");

    hud_panel(renderer, layer, 988, 30, 268, 180);
    hud_static_text(renderer, layer, 1004, 45, "LAP TIME");
    hud_static_text(renderer, layer, 1004, 82, "BEST");
    hud_static_text(renderer, layer, 1004, 119, "DELTA");

    hud_panel(renderer, layer, 988, 237, 268, 269);
    hud_static_text(renderer, layer, 1004, 252, "LATERAL G");
    hud_static_text(renderer, layer, 1004, 468, "LONG G");
    hud_draw_circle(renderer, layer, 1122, 365, 92, hud_cyan);
    hud_draw_circle(renderer, layer, 1122, 365, 46, hud_muted);
    hud_draw_line(renderer, layer, 1028, 365, 1216, 365, hud_muted);
    hud_draw_line(renderer, layer, 1122, 271, 1122, 459, hud_muted);

    hud_panel(renderer, layer, 24, 535, 350, 157);
    hud_static_text(renderer, layer, 40, 550, "THROTTLE");
    hud_static_text(renderer, layer, 40, 615, "BRAKE");
    hud_draw_segment_outline(renderer, layer, 126, 553, 12, 13, 16);
    hud_draw_segment_outline(renderer, layer, 126, 618, 12, 13, 16);

    hud_panel(renderer, layer, 460, 535, 360, 157);
    hud_static_text(renderer, layer, 525, 650, "KM/H");
    hud_static_text(renderer, layer, 720, 575, "STATE");

    hud_panel(renderer, layer, 850, 535, 406, 157);
    hud_static_text(renderer, layer, 866, 550, "MOTOR RPM");
    hud_draw_segment_outline(renderer, layer, 866, 598, 20, 14, 18);
    for (index = 0; index <= 5; ++index) {
        char tick[8];
        (void)snprintf(tick, sizeof(tick), "%uK", index * 2U);
        (void)hud_draw_text(renderer, layer, 866 + index * 72, 654, tick, 1,
                            hud_muted);
    }
    hud_fill_rect(renderer, layer, 32, 30, 130, 2, hud_cyan);
    hud_fill_rect(renderer, layer, 1115, 22, 125, 2, hud_cyan);
    hud_layer_set_static_regions(renderer, layer);
}

static void hud_value(const HudRenderer *renderer, HudLayer *layer,
                      int x, int y, const char *value, HudColor color)
{
    (void)hud_draw_text(renderer, layer, x, y, value, 1, color);
}

static void hud_render_dynamic_values(HudRenderer *renderer, HudLayer *layer,
                                      const VehicleState *state)
{
    VehicleState empty_state;
    char value[64];
    bool valid;
    double lateral_g;
    double longitudinal_g;

    if (state == NULL) {
        memset(&empty_state, 0, sizeof(empty_state));
        state = &empty_state;
    }

    hud_value(renderer, layer, 40, 21, "LAP", hud_white);
    if (state != NULL && state->lap_valid) {
        (void)snprintf(value, sizeof(value), "%u", state->lap_number);
    } else {
        (void)snprintf(value, sizeof(value), "--");
    }
    hud_value(renderer, layer, 76, 21, value, hud_white);

    valid = hud_finite(state, VEHICLE_STATE_VALID_MOTOR_RPM, state->motor_rpm);
    hud_format_number(value, sizeof(value), valid, state->motor_rpm, "%.0f",
                      "RPM");
    hud_value(renderer, layer, 170, 99, value, hud_white);
    valid = hud_finite(state, VEHICLE_STATE_VALID_TORQUE_NM, state->torque_nm);
    hud_format_number(value, sizeof(value), valid, state->torque_nm, "%.0f",
                      "NM");
    hud_value(renderer, layer, 170, 126, value, hud_white);
    valid = hud_finite(state, VEHICLE_STATE_VALID_HV_VOLTAGE_V,
                       state->hv_voltage_v);
    hud_format_number(value, sizeof(value), valid, state->hv_voltage_v, "%.0f",
                      "V");
    hud_value(renderer, layer, 170, 163, value, hud_white);
    valid = hud_finite(state, VEHICLE_STATE_VALID_HV_CURRENT_A,
                       state->hv_current_a);
    hud_format_number(value, sizeof(value), valid, state->hv_current_a, "%.0f",
                      "A");
    hud_value(renderer, layer, 170, 190, value, hud_white);
    valid = hud_finite(state, VEHICLE_STATE_VALID_HV_POWER_W,
                       state->hv_power_w);
    hud_format_number(value, sizeof(value), valid, state->hv_power_w / 1000.0,
                      "%.1f", "KW");
    hud_value(renderer, layer, 170, 217, value, hud_white);
    valid = hud_finite(state, VEHICLE_STATE_VALID_HV_SOC_PERCENT,
                       state->hv_soc_percent);
    hud_format_number(value, sizeof(value), valid, state->hv_soc_percent,
                      "%.0f", "%");
    hud_value(renderer, layer, 170, 257, value, hud_green);
    hud_draw_progress(renderer, layer, 40, 310, 16, 12, 12,
                      state != NULL ? state->hv_soc_percent : 0.0,
                      valid && state->hv_soc_percent < 20.0 ? hud_red :
                      (valid && state->hv_soc_percent < 40.0 ? hud_yellow :
                       hud_green), valid);
    valid = hud_finite(state, VEHICLE_STATE_VALID_LV_VOLTAGE_V,
                       state->lv_voltage_v);
    hud_format_number(value, sizeof(value), valid, state->lv_voltage_v, "%.1f",
                      "V");
    hud_value(renderer, layer, 170, 339, value, hud_white);

    hud_format_time(value, sizeof(value), state != NULL && state->lap_valid,
                    state != NULL ? state->lap_time_s : 0.0);
    hud_value(renderer, layer, 1112, 59, value, hud_white);
    hud_format_time(value, sizeof(value), state != NULL && state->lap_valid &&
                    state->best_lap_time_s > 0.0,
                    state != NULL ? state->best_lap_time_s : 0.0);
    hud_value(renderer, layer, 1112, 96, value, hud_white);
    valid = state != NULL && state->lap_valid && isfinite(state->delta_time_s);
    hud_format_number(value, sizeof(value), valid, state->delta_time_s, "%+.2f",
                      "");
    hud_value(renderer, layer, 1112, 133, value, !valid ? hud_white :
              (state->delta_time_s < -0.02 ? hud_green :
               (state->delta_time_s > 0.02 ? hud_red : hud_white)));

    valid = hud_finite(state, VEHICLE_STATE_VALID_ACC_Y_MPS2, state->acc_y);
    lateral_g = valid ? state->acc_y / 9.80665 : 0.0;
    hud_format_number(value, sizeof(value), valid, lateral_g, "%.2f", "");
    hud_value(renderer, layer, 1150, 252, value, hud_white);
    valid = hud_finite(state, VEHICLE_STATE_VALID_ACC_X_MPS2, state->acc_x);
    longitudinal_g = valid ? state->acc_x / 9.80665 : 0.0;
    hud_format_number(value, sizeof(value), valid, longitudinal_g, "%+.2f", "");
    hud_value(renderer, layer, 1114, 469, value, hud_white);
    if (valid || hud_finite(state, VEHICLE_STATE_VALID_ACC_Y_MPS2, state->acc_y)) {
        int dot_x = 1122 + (int)lround(hud_clamp_percent((lateral_g + 1.0) * 50.0) /
                                      100.0 * 82.0);
        int dot_y = 365 - (int)lround(hud_clamp_percent((longitudinal_g + 1.0) *
                                      50.0) / 100.0 * 82.0);
        hud_fill_rect(renderer, layer, dot_x - 5, dot_y - 5, 10, 10, hud_cyan);
    }

    valid = hud_finite(state, VEHICLE_STATE_VALID_THROTTLE_PERCENT,
                       state->throttle_percent);
    hud_format_number(value, sizeof(value), valid, state->throttle_percent,
                      "%.0f", "%");
    hud_value(renderer, layer, 300, 554, value, hud_green);
    hud_draw_progress(renderer, layer, 126, 553, 12, 13, 16,
                      state != NULL ? state->throttle_percent : 0.0,
                      hud_green, valid);
    valid = hud_finite(state, VEHICLE_STATE_VALID_BRAKE_PERCENT,
                       state->brake_percent);
    hud_format_number(value, sizeof(value), valid, state->brake_percent,
                      "%.0f", "%");
    hud_value(renderer, layer, 300, 619, value, hud_red);
    hud_draw_progress(renderer, layer, 126, 618, 12, 13, 16,
                      state != NULL ? state->brake_percent : 0.0,
                      hud_red, valid);

    valid = hud_finite(state, VEHICLE_STATE_VALID_SPEED_KMH, state->speed_kmh);
    hud_format_number(value, sizeof(value), valid, state->speed_kmh, "%.0f", "");
    hud_value(renderer, layer, 535, 563, value, hud_white);
    if (state != NULL && hud_has(state, VEHICLE_STATE_VALID_DRIVE_STATE)) {
        hud_value(renderer, layer, 720, 599,
                  hud_drive_state_name(state->drive_state), hud_cyan);
    } else {
        hud_value(renderer, layer, 720, 599, "--", hud_cyan);
    }

    valid = hud_finite(state, VEHICLE_STATE_VALID_MOTOR_RPM, state->motor_rpm);
    hud_format_number(value, sizeof(value), valid, state->motor_rpm, "%.0f", "");
    hud_value(renderer, layer, 866, 563, value, hud_white);
    hud_draw_progress(renderer, layer, 866, 598, 20, 14, 18,
                      state != NULL ? state->motor_rpm / 100.0 : 0.0,
                      valid && state->motor_rpm >= 8000.0 ? hud_red :
                      (valid && state->motor_rpm >= 5000.0 ? hud_yellow : hud_cyan),
                      valid);
}

static bool hud_surface_valid(const HudRenderer *renderer,
                              const HudSurface *surface)
{
    return renderer != NULL && surface != NULL && surface->pixels != NULL &&
           surface->width == renderer->width &&
           surface->height == renderer->height &&
           surface->stride_bytes >= surface->width * VIDEO_FRAME_CHANNELS &&
           (surface->pixel_format == HUD_PIXEL_FORMAT_RGBA8888 ||
            surface->pixel_format == HUD_PIXEL_FORMAT_ARGB8888);
}

static void hud_layer_set_surface(HudLayer *layer, const HudSurface *surface)
{
    memset(layer, 0, sizeof(*layer));
    layer->width = surface->width;
    layer->height = surface->height;
    layer->stride = surface->stride_bytes;
    layer->pixels = surface->pixels;
    layer->pixel_format = surface->pixel_format;
}

static const HudLayer *hud_renderer_render_dynamic_layer(
    HudRenderer *renderer, HudLayer *layer, const VehicleState *state,
    bool recording, HudRenderTiming *timing)
{
    uint64_t total_start;
    uint64_t stage_start;

    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    total_start = hud_renderer_now_us();
    stage_start = hud_renderer_now_us();
    hud_layer_clear(layer);
    if (timing != NULL) {
        timing->clear_time_us = hud_renderer_now_us() - stage_start;
    }
    stage_start = hud_renderer_now_us();
    hud_render_dynamic_values(renderer, layer, state);
    if (recording) {
        hud_draw_circle(renderer, layer, 182, 28, 6, hud_red);
        hud_value(renderer, layer, 194, 21, "REC", hud_red);
    }
    hud_layer_set_dynamic_regions(renderer, layer);
    if (timing != NULL) {
        timing->draw_time_us = hud_renderer_now_us() - stage_start;
        timing->total_time_us = hud_renderer_now_us() - total_start;
    }
    return layer;
}

static int hud_renderer_init_common(HudRenderer *renderer, uint32_t width,
                                    uint32_t height, bool allocate_layers)
{
    size_t bytes;

    if (renderer == NULL || width == 0U || height == 0U ||
        ((uint64_t)width * HUD_BASE_HEIGHT) !=
        ((uint64_t)height * HUD_BASE_WIDTH)) {
        errno = EINVAL;
        return -1;
    }
    memset(renderer, 0, sizeof(*renderer));
    if ((uint64_t)width * VIDEO_FRAME_CHANNELS > SIZE_MAX ||
        (uint64_t)height * (uint64_t)width * VIDEO_FRAME_CHANNELS > SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    bytes = (size_t)width * height * VIDEO_FRAME_CHANNELS;
    renderer->width = width;
    renderer->height = height;
    renderer->scale_x = (double)width / HUD_BASE_WIDTH;
    renderer->scale_y = (double)height / HUD_BASE_HEIGHT;
    renderer->static_layer.width = width;
    renderer->static_layer.height = height;
    renderer->static_layer.stride = width * VIDEO_FRAME_CHANNELS;
    renderer->static_layer.pixel_format = HUD_PIXEL_FORMAT_RGBA8888;
    renderer->dynamic_layer.width = width;
    renderer->dynamic_layer.height = height;
    renderer->dynamic_layer.stride = width * VIDEO_FRAME_CHANNELS;
    renderer->dynamic_layer.pixel_format = HUD_PIXEL_FORMAT_RGBA8888;
    renderer->dynamic_surface_only = !allocate_layers;
    renderer->initialized = true;
    if (allocate_layers) {
        renderer->static_layer.pixels = calloc(1U, bytes);
        renderer->dynamic_layer.pixels = calloc(1U, bytes);
        if (renderer->static_layer.pixels == NULL ||
            renderer->dynamic_layer.pixels == NULL) {
            hud_renderer_close(renderer);
            return -1;
        }
        hud_render_static(renderer);
    }
    return 0;
}

int hud_renderer_init(HudRenderer *renderer, uint32_t width, uint32_t height)
{
    return hud_renderer_init_common(renderer, width, height, true);
}

int hud_renderer_init_external(HudRenderer *renderer, uint32_t width,
                               uint32_t height)
{
    return hud_renderer_init_common(renderer, width, height, false);
}

void hud_renderer_close(HudRenderer *renderer)
{
    if (renderer == NULL) {
        return;
    }
    free(renderer->static_layer.pixels);
    free(renderer->dynamic_layer.pixels);
    memset(renderer, 0, sizeof(*renderer));
}

const HudLayer *hud_renderer_static_layer(const HudRenderer *renderer)
{
    return renderer != NULL && renderer->initialized ?
           &renderer->static_layer : NULL;
}

const HudLayer *hud_renderer_render_dynamic(HudRenderer *renderer,
                                             const VehicleState *state,
                                             bool recording)
{
    if (renderer == NULL || !renderer->initialized ||
        renderer->dynamic_surface_only || renderer->dynamic_layer.pixels == NULL) {
        return NULL;
    }
    return hud_renderer_render_dynamic_layer(renderer, &renderer->dynamic_layer,
                                              state, recording, NULL);
}

const HudLayer *hud_renderer_render_dynamic_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing)
{
    if (renderer == NULL || !renderer->initialized ||
        !hud_surface_valid(renderer, surface)) {
        return NULL;
    }
    hud_layer_set_surface(&renderer->external_dynamic_layer, surface);
    return hud_renderer_render_dynamic_layer(renderer,
                                              &renderer->external_dynamic_layer,
                                              state, recording, timing);
}
