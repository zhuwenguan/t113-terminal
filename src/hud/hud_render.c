#include "hud_render.h"

#include "hud_layout.h"
#include "hud_theme.h"
#include "hud_widget.h"
#include "ui_font.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HUD_FONT_SMALL 16
#define HUD_FONT_NORMAL 24
#define HUD_FONT_SPEED 64
#define HUD_G_TO_MPS2 9.80665

struct HudContext {
    UiCanvas canvas;
    UiFont *font_small;
    UiFont *font_normal;
    UiFont *font_speed;
    HudLayout layout;
    const HudTheme *theme;
};

static const HudTheme hud_default_theme = HUD_THEME_DEFAULT;

static int hud_int_from_i64(int64_t value)
{
    if (value < INT_MIN) return INT_MIN;
    if (value > INT_MAX) return INT_MAX;
    return (int)value;
}

static bool hud_text_measure(UiFont *font, const char *text, int *width,
                             int *height)
{
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    return font != NULL && text != NULL && text[0] != '\0' && width != NULL &&
           height != NULL && ui_text_measure(font, text, width, height) == 0 &&
           *width >= 0 && *height > 0;
}

static void hud_draw_text_top(UiCanvas *canvas, UiFont *font, int x, int y,
                              uint32_t color, const char *text)
{
    int width;
    int height;

    if (!hud_text_measure(font, text, &width, &height)) return;
    (void)width;
    (void)ui_text(canvas, font, x, hud_int_from_i64((int64_t)y + height),
                  color, text);
}

static void hud_draw_text_right(UiCanvas *canvas, UiFont *font, int right,
                                int y, uint32_t color, const char *text)
{
    int width;
    int height;

    if (!hud_text_measure(font, text, &width, &height)) return;
    (void)ui_text(canvas, font,
                  hud_int_from_i64((int64_t)right - width),
                  hud_int_from_i64((int64_t)y + height), color, text);
}

static bool hud_state_valid(const VehicleState *state, uint32_t valid_bit,
                            double value)
{
    return state != NULL && (state->valid_mask & valid_bit) != 0U &&
           isfinite(value);
}

static double hud_clamp(double value, double minimum, double maximum)
{
    if (!isfinite(value) || value < minimum) return minimum;
    return value > maximum ? maximum : value;
}

static void hud_format_number(char *buffer, size_t size, bool valid,
                              double value, const char *format,
                              const char *suffix)
{
    int written;

    if (buffer == NULL || size == 0U) return;
    if (!valid || !isfinite(value)) {
        (void)snprintf(buffer, size, "--");
        return;
    }
    (void)snprintf(buffer, size, format, value);
    if (suffix == NULL || suffix[0] == '\0') return;
    written = snprintf(buffer + strlen(buffer),
                       size > strlen(buffer) ? size - strlen(buffer) : 0U,
                       "%s", suffix);
    (void)written;
}

static void hud_format_time(char *buffer, size_t size, bool valid,
                            double seconds)
{
    unsigned int minutes;
    unsigned int whole_seconds;
    unsigned int milliseconds;

    if (buffer == NULL || size == 0U) return;
    if (!valid || !isfinite(seconds) || seconds < 0.0) {
        (void)snprintf(buffer, size, "--:--.---");
        return;
    }
    seconds = hud_clamp(seconds, 0.0, 5999.999);
    minutes = (unsigned int)(seconds / 60.0);
    whole_seconds = (unsigned int)seconds % 60U;
    milliseconds = (unsigned int)lround((seconds - floor(seconds)) * 1000.0);
    if (milliseconds >= 1000U) {
        milliseconds = 0U;
        whole_seconds++;
        if (whole_seconds >= 60U) {
            whole_seconds = 0U;
            minutes++;
        }
    }
    (void)snprintf(buffer, size, "%02u:%02u.%03u", minutes,
                   whole_seconds, milliseconds);
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

static HudPanelStyle hud_panel_style(const HudContext *hud)
{
    return (HudPanelStyle){ hud->theme->panel_bg, hud->theme->panel_border,
                            1, 8 };
}

static HudTextValueStyle hud_value_style(const HudContext *hud,
                                         UiFont *value_font,
                                         HudAlign align)
{
    return (HudTextValueStyle){
        hud->font_small, value_font, hud->font_small,
        hud->theme->text_secondary, hud->theme->text_primary,
        hud->theme->text_secondary, 2, 5, align
    };
}

static HudSegmentBarStyle hud_bar_style(const HudContext *hud,
                                        uint32_t active_color)
{
    return (HudSegmentBarStyle){
        active_color, hud->theme->segment_inactive, hud->theme->panel_border,
        hud->layout.segment_count, 3, 1, 3
    };
}

static HudStatusStyle hud_status_style(const HudContext *hud,
                                       uint32_t active_color)
{
    return (HudStatusStyle){ hud->font_small, active_color,
                             hud->theme->yellow, hud->theme->text_primary,
                             4, 6 };
}

static void hud_draw_lap(HudContext *hud, const VehicleState *state)
{
    char lap[24];
    char lap_time[32];
    char delta[32];
    HudRect rect = hud->layout.lap;
    HudPanelStyle panel = hud_panel_style(hud);
    HudTextValueStyle value_style = hud_value_style(
        hud, hud->font_normal, HUD_WIDGET_ALIGN_LEFT);
    bool lap_valid = state != NULL && state->lap_valid;
    bool time_valid = lap_valid && state != NULL &&
                      isfinite(state->lap_time_s);
    bool delta_valid = lap_valid && state != NULL &&
                       isfinite(state->delta_time_s);

    hud_panel(&hud->canvas, rect, &panel);
    (void)snprintf(lap, sizeof(lap), "%u", lap_valid ? state->lap_number : 0U);
    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, rect.y + 12, rect.w - 28, 48 },
                   &value_style, "LAP", lap_valid ? lap : "--", NULL);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 70, hud->theme->text_secondary, "TIME");
    hud_format_time(lap_time, sizeof(lap_time), time_valid,
                    state != NULL ? state->lap_time_s : 0.0);
    hud_draw_text_right(&hud->canvas, hud->font_small,
                        rect.x + rect.w - 14, rect.y + 68,
                        time_valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, lap_time);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 98, hud->theme->text_secondary, "DELTA");
    hud_format_number(delta, sizeof(delta), delta_valid,
                      state != NULL ? state->delta_time_s : 0.0, "%+.2f",
                      "");
    hud_draw_text_right(&hud->canvas, hud->font_small,
                        rect.x + rect.w - 14, rect.y + 96,
                        !delta_valid ? hud->theme->text_secondary :
                        (state->delta_time_s <= 0.0 ? hud->theme->green :
                         hud->theme->red), delta);
}

static void hud_draw_speed(HudContext *hud, const VehicleState *state,
                           bool recording)
{
    char speed[24];
    const char *status_text = "STATE --";
    bool speed_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_SPEED_KMH,
        state != NULL ? state->speed_kmh : 0.0);
    bool drive_valid = state != NULL &&
                       (state->valid_mask & VEHICLE_STATE_VALID_DRIVE_STATE) != 0U;
    bool ready = drive_valid && state->drive_state == VEHICLE_DRIVE_STATE_READY;
    HudRect rect = hud->layout.speed;
    HudPanelStyle panel = hud_panel_style(hud);
    HudTextValueStyle value_style = hud_value_style(
        hud, hud->font_speed, HUD_WIDGET_ALIGN_CENTER);
    HudStatusStyle status_style = hud_status_style(hud, hud->theme->green);

    hud_panel(&hud->canvas, rect, &panel);
    hud_format_number(speed, sizeof(speed), speed_valid,
                      state != NULL ? state->speed_kmh : 0.0, "%.0f", "");
    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, rect.y + 18, rect.w - 28, 116 },
                   &value_style, "SPEED", speed_valid ? speed : "---",
                   "km/h");
    if (drive_valid) status_text = hud_drive_state_name(state->drive_state);
    if (recording) {
        status_text = "REC";
        status_style.active_color = hud->theme->red;
    }
    hud_status_indicator(&hud->canvas, rect.x + 90, rect.y + 148,
                         &status_style, ready || recording, status_text);
}

static void hud_draw_vehicle_info(HudContext *hud, const VehicleState *state)
{
    char value[32];
    HudRect rect = hud->layout.vehicle_info;
    HudPanelStyle panel = hud_panel_style(hud);
    HudTextValueStyle style = hud_value_style(hud, hud->font_small,
                                              HUD_WIDGET_ALIGN_LEFT);
    int row_y = rect.y + 28;
    int row_gap = 38;
    bool valid;

    hud_panel(&hud->canvas, rect, &panel);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 10, hud->theme->panel_border, "VEHICLE");

    valid = hud_state_valid(state, VEHICLE_STATE_VALID_HV_VOLTAGE_V,
                            state != NULL ? state->hv_voltage_v : 0.0);
    hud_format_number(value, sizeof(value), valid,
                      state != NULL ? state->hv_voltage_v : 0.0, "%.0f", "");
    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, row_y, rect.w - 28, 40 }, &style,
                   "HV", value, "V");

    valid = hud_state_valid(state, VEHICLE_STATE_VALID_HV_SOC_PERCENT,
                            state != NULL ? state->hv_soc_percent : 0.0);
    hud_format_number(value, sizeof(value), valid,
                      state != NULL ? state->hv_soc_percent : 0.0, "%.0f", "");
    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, row_y + row_gap, rect.w - 28, 40 },
                   &style, "SOC", value, "%");

    valid = hud_state_valid(state, VEHICLE_STATE_VALID_HV_POWER_W,
                            state != NULL ? state->hv_power_w : 0.0);
    hud_format_number(value, sizeof(value), valid,
                      state != NULL ? state->hv_power_w / 1000.0 : 0.0,
                      "%.1f", "");
    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, row_y + row_gap * 2,
                              rect.w - 28, 40 }, &style, "POWER", value,
                   "kW");

    valid = hud_state_valid(state, VEHICLE_STATE_VALID_HV_CURRENT_A,
                            state != NULL ? state->hv_current_a : 0.0);
    hud_format_number(value, sizeof(value), valid,
                      state != NULL ? state->hv_current_a : 0.0, "%.0f", "");
    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, row_y + row_gap * 3,
                              rect.w - 28, 40 }, &style, "CURRENT", value,
                   "A");

    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, row_y + row_gap * 4,
                              rect.w - 28, 40 }, &style, "TEMP", "--", "C");

    valid = state != NULL &&
            (state->valid_mask & VEHICLE_STATE_VALID_DRIVE_STATE) != 0U;
    hud_text_value(&hud->canvas,
                   (HudRect){ rect.x + 14, row_y + row_gap * 5,
                              rect.w - 28, 40 }, &style, "MODE",
                   valid ? hud_drive_state_name(state->drive_state) : "--",
                   NULL);
}

static void hud_draw_g_meter(HudContext *hud, const VehicleState *state)
{
    HudRect rect = hud->layout.g_meter;
    HudPanelStyle panel = hud_panel_style(hud);
    bool lateral_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_ACC_Y_MPS2,
        state != NULL ? state->acc_y : 0.0);
    bool longitudinal_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_ACC_X_MPS2,
        state != NULL ? state->acc_x : 0.0);
    double lateral = lateral_valid ? state->acc_y / HUD_G_TO_MPS2 : 0.0;
    double longitudinal = longitudinal_valid ?
                          state->acc_x / HUD_G_TO_MPS2 : 0.0;
    int center_x = rect.x + rect.w / 2;
    int center_y = rect.y + 62;
    int radius = 29;
    int dot_x;
    int dot_y;

    hud_panel(&hud->canvas, rect, &panel);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 8, hud->theme->text_secondary, "G FORCE");
    ui_circle(&hud->canvas, center_x, center_y, radius, 1,
              hud->theme->panel_border);
    ui_line(&hud->canvas, center_x - radius, center_y, center_x + radius,
            center_y, 1, hud->theme->segment_inactive);
    ui_line(&hud->canvas, center_x, center_y - radius, center_x,
            center_y + radius, 1, hud->theme->segment_inactive);
    if (!lateral_valid && !longitudinal_valid) return;
    dot_x = center_x + (int)lround(hud_clamp(lateral / 1.5, -1.0, 1.0) *
                                   (double)(radius - 4));
    dot_y = center_y - (int)lround(hud_clamp(longitudinal / 1.5, -1.0, 1.0) *
                                   (double)(radius - 4));
    ui_fill_circle(&hud->canvas, dot_x, dot_y, 4, hud->theme->green);
}

static void hud_draw_bar(HudContext *hud, HudRect rect, const char *label,
                         double value, bool valid, uint32_t color)
{
    char display[24];
    HudPanelStyle panel = hud_panel_style(hud);
    HudSegmentBarStyle bar = hud_bar_style(hud, color);

    hud_panel(&hud->canvas, rect, &panel);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 10, hud->theme->text_secondary, label);
    hud_format_number(display, sizeof(display), valid, value, "%.0f", "%");
    hud_draw_text_right(&hud->canvas, hud->font_small, rect.x + rect.w - 14,
                        rect.y + 10,
                        valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, display);
    hud_segment_bar(&hud->canvas,
                    (HudRect){ rect.x + 14, rect.y + 42, rect.w - 28, 20 },
                    &bar, valid ? (float)(value / 100.0) : 0.0f);
}

static void hud_draw_throttle(HudContext *hud, const VehicleState *state)
{
    bool valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_THROTTLE_PERCENT,
        state != NULL ? state->throttle_percent : 0.0);

    hud_draw_bar(hud, hud->layout.throttle, "THROTTLE",
                 valid ? hud_clamp(state->throttle_percent, 0.0, 100.0) : 0.0,
                 valid, hud->theme->throttle);
}

static void hud_draw_brake(HudContext *hud, const VehicleState *state)
{
    bool valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_BRAKE_PERCENT,
        state != NULL ? state->brake_percent : 0.0);

    hud_draw_bar(hud, hud->layout.brake, "BRAKE",
                 valid ? hud_clamp(state->brake_percent, 0.0, 100.0) : 0.0,
                 valid, hud->theme->brake);
}

static void hud_draw_rpm(HudContext *hud, const VehicleState *state)
{
    char display[24];
    HudRect rect = hud->layout.rpm;
    HudPanelStyle panel = hud_panel_style(hud);
    bool valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_MOTOR_RPM,
        state != NULL ? state->motor_rpm : 0.0);
    double rpm = valid ? hud_clamp(state->motor_rpm, 0.0,
                                  (double)hud->layout.rpm_max) : 0.0;
    uint32_t color = valid && rpm >= hud->layout.rpm_red_start ?
                     hud->theme->red :
                     (valid && rpm >= hud->layout.rpm_red_start / 2.0 ?
                      hud->theme->yellow : hud->theme->rpm);
    HudSegmentBarStyle bar = hud_bar_style(hud, color);

    hud_panel(&hud->canvas, rect, &panel);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 10, hud->theme->text_secondary, "RPM");
    hud_format_number(display, sizeof(display), valid, rpm, "%.0f", "");
    hud_draw_text_right(&hud->canvas, hud->font_small, rect.x + rect.w - 14,
                        rect.y + 10,
                        valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, display);
    hud_segment_bar(&hud->canvas,
                    (HudRect){ rect.x + 14, rect.y + 42, rect.w - 28, 20 },
                    &bar, valid ? (float)(rpm / hud->layout.rpm_max) : 0.0f);
}

static void hud_draw_accel_event(HudContext *hud, const VehicleState *state)
{
    char time_50m[24];
    char time_75m[24];
    HudRect rect = hud->layout.accel_event;
    HudPanelStyle panel = hud_panel_style(hud);
    bool valid = state != NULL && isfinite(state->accel_50m_time_s) &&
                 isfinite(state->accel_time_s) &&
                 state->accel_50m_time_s > 0.0 && state->accel_time_s > 0.0;

    if (!valid) return;
    hud_panel(&hud->canvas, rect, &panel);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 12, hud->theme->panel_border, "ACCEL EVENT");
    (void)snprintf(time_50m, sizeof(time_50m), "%.2f s",
                   state->accel_50m_time_s);
    (void)snprintf(time_75m, sizeof(time_75m), "%.2f s", state->accel_time_s);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 52, hud->theme->text_secondary, "50 m");
    hud_draw_text_right(&hud->canvas, hud->font_small, rect.x + rect.w - 14,
                        rect.y + 50, hud->theme->text_primary, time_50m);
    hud_draw_text_top(&hud->canvas, hud->font_small, rect.x + 14,
                      rect.y + 84, hud->theme->text_secondary, "75 m");
    hud_draw_text_right(&hud->canvas, hud->font_small, rect.x + rect.w - 14,
                        rect.y + 82, hud->theme->text_primary, time_75m);
}

static void hud_draw_status(HudContext *hud, const VehicleState *state)
{
    HudStatusStyle can_style = hud_status_style(hud, hud->theme->green);
    HudStatusStyle gps_style = hud_status_style(hud, hud->theme->green);
    bool can_ok = state != NULL && state->can_valid;
    bool gps_ok = state != NULL && state->gps_valid && state->gps_fix_type >= 3U;
    HudRect rect = hud->layout.status;

    hud_status_indicator(&hud->canvas, rect.x, rect.y, &can_style, can_ok,
                         "CAN");
    hud_status_indicator(&hud->canvas, rect.x + 100, rect.y, &gps_style,
                         gps_ok, "GPS");
}

static bool hud_warm_font(UiFont *font)
{
    static const char warm_text[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        " +-./:%";
    int width;
    int height;

    return hud_text_measure(font, warm_text, &width, &height);
}

int hud_init(HudContext **context, const char *font_path)
{
    HudContext *hud;

    if (context == NULL || font_path == NULL || font_path[0] == '\0') {
        return -1;
    }
    *context = NULL;
    hud = calloc(1U, sizeof(*hud));
    if (hud == NULL) return -1;
    hud->font_small = ui_font_load(font_path, HUD_FONT_SMALL);
    hud->font_normal = ui_font_load(font_path, HUD_FONT_NORMAL);
    hud->font_speed = ui_font_load(font_path, HUD_FONT_SPEED);
    if (hud->font_small == NULL || hud->font_normal == NULL ||
        hud->font_speed == NULL ||
        !hud_warm_font(hud->font_small) || !hud_warm_font(hud->font_normal) ||
        !hud_warm_font(hud->font_speed)) {
        hud_shutdown(hud);
        return -1;
    }
    hud_ui_layout_default(&hud->layout);
    hud->theme = &hud_default_theme;
    *context = hud;
    return 0;
}

void hud_shutdown(HudContext *context)
{
    if (context == NULL) return;
    ui_font_free(context->font_speed);
    ui_font_free(context->font_normal);
    ui_font_free(context->font_small);
    free(context);
}

int hud_render(HudContext *context, UiCanvas *canvas,
               const VehicleState *state, bool recording)
{
    if (context == NULL || canvas == NULL || canvas->pixels == NULL ||
        canvas->width <= 0 || canvas->height <= 0 || canvas->stride <= 0) {
        return -1;
    }
    context->canvas = *canvas;
    ui_clear(&context->canvas, context->theme->transparent);
    hud_draw_lap(context, state);
    hud_draw_speed(context, state, recording);
    hud_draw_vehicle_info(context, state);
    hud_draw_g_meter(context, state);
    hud_draw_throttle(context, state);
    hud_draw_brake(context, state);
    hud_draw_rpm(context, state);
    hud_draw_accel_event(context, state);
    hud_draw_status(context, state);
    return 0;
}
