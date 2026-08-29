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
#include <time.h>

#define HUD_FONT_SMALL 16
#define HUD_FONT_NORMAL 24
#define HUD_FONT_SPEED 48
#define HUD_G_TO_MPS2 9.80665

struct HudContext {
    UiCanvas canvas;
    UiCanvas static_canvas;
    uint8_t *static_pixels;
    size_t static_bytes;
    bool static_ready;
    UiFont *font_small;
    UiFont *font_normal;
    UiFont *font_speed;
    HudLayout layout;
    const HudTheme *theme;
};

static const HudTheme hud_default_theme = HUD_THEME_DEFAULT;

static uint64_t hud_now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) return 0U;
    return (uint64_t)timestamp.tv_sec * 1000000ULL +
           (uint64_t)timestamp.tv_nsec / 1000ULL;
}

static uint64_t hud_profile_elapsed(uint64_t start)
{
    uint64_t finish = hud_now_us();

    return finish >= start ? finish - start : 0U;
}

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

static void hud_draw_text_center(UiCanvas *canvas, UiFont *font, int center_x,
                                 int y, uint32_t color, const char *text)
{
    int width;
    int height;

    if (!hud_text_measure(font, text, &width, &height)) return;
    (void)ui_text(canvas, font,
                  hud_int_from_i64((int64_t)center_x - width / 2),
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
    size_t length;

    if (buffer == NULL || size == 0U) return;
    if (!valid || !isfinite(value)) {
        (void)snprintf(buffer, size, "--");
        return;
    }
    (void)snprintf(buffer, size, format, value);
    if (suffix == NULL || suffix[0] == '\0') return;
    length = strlen(buffer);
    (void)snprintf(buffer + length, size > length ? size - length : 0U,
                   "%s", suffix);
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
    case VEHICLE_DRIVE_STATE_READY: return "READY";
    case VEHICLE_DRIVE_STATE_UNKNOWN:
    default: return "--";
    }
}

static HudPanelStyle hud_panel_style(const HudContext *hud, int radius)
{
    return (HudPanelStyle){ hud->theme->panel_bg, hud->theme->panel_border,
                            1, radius };
}

static void hud_draw_rule(UiCanvas *canvas, int x0, int y, int x1,
                          uint32_t color)
{
    ui_line(canvas, x0, y, x1, y, 1, color);
}

static void hud_draw_stopwatch_icon(UiCanvas *canvas, int x, int y,
                                    uint32_t color)
{
    ui_circle(canvas, x + 7, y + 8, 6, 1, color);
    ui_fill_rect(canvas, x + 5, y, 5, 2, color);
    ui_line(canvas, x + 7, y + 8, x + 7, y + 4, 1, color);
    ui_line(canvas, x + 7, y + 8, x + 10, y + 10, 1, color);
}

static void hud_draw_flag_icon(UiCanvas *canvas, int x, int y,
                               uint32_t color)
{
    ui_line(canvas, x + 1, y, x + 1, y + 17, 1, color);
    ui_line(canvas, x + 2, y + 1, x + 13, y + 1, 1, color);
    ui_line(canvas, x + 13, y + 1, x + 9, y + 7, 1, color);
    ui_line(canvas, x + 9, y + 7, x + 2, y + 7, 1, color);
    ui_line(canvas, x + 1, y + 17, x + 14, y + 17, 1, color);
}

static void hud_draw_battery_icon(UiCanvas *canvas, int x, int y,
                                  uint32_t color)
{
    ui_rect(canvas, x, y + 2, 14, 9, 1, color);
    ui_fill_rect(canvas, x + 14, y + 5, 2, 3, color);
    ui_fill_rect(canvas, x + 3, y + 5, 7, 3, color);
}

static void hud_draw_power_icon(UiCanvas *canvas, int x, int y,
                                uint32_t color)
{
    ui_line(canvas, x + 8, y, x + 2, y + 9, 1, color);
    ui_line(canvas, x + 2, y + 9, x + 7, y + 9, 1, color);
    ui_line(canvas, x + 7, y + 9, x + 4, y + 17, 1, color);
    ui_line(canvas, x + 4, y + 17, x + 14, y + 6, 1, color);
    ui_line(canvas, x + 14, y + 6, x + 9, y + 6, 1, color);
    ui_line(canvas, x + 9, y + 6, x + 12, y, 1, color);
}

static void hud_draw_temperature_icon(UiCanvas *canvas, int x, int y,
                                      uint32_t color)
{
    ui_line(canvas, x + 5, y, x + 5, y + 11, 1, color);
    ui_circle(canvas, x + 5, y + 13, 4, 1, color);
    ui_fill_circle(canvas, x + 5, y + 13, 2, color);
}

static void hud_draw_controller_icon(UiCanvas *canvas, int x, int y,
                                     uint32_t color)
{
    ui_round_rect(canvas, x, y + 2, 14, 13, 2, 1, color);
    ui_line(canvas, x + 4, y, x + 4, y + 4, 1, color);
    ui_line(canvas, x + 10, y, x + 10, y + 4, 1, color);
    ui_fill_circle(canvas, x + 5, y + 9, 1, color);
    ui_fill_circle(canvas, x + 10, y + 9, 1, color);
}

static void hud_draw_mode_icon(UiCanvas *canvas, int x, int y,
                               uint32_t color)
{
    ui_rect(canvas, x, y + 2, 14, 13, 1, color);
    ui_line(canvas, x + 3, y + 9, x + 6, y + 12, 1, color);
    ui_line(canvas, x + 6, y + 12, x + 11, y + 6, 1, color);
}

static void hud_draw_lap_static(HudContext *hud)
{
    const HudRect rect = hud->layout.lap;
    const HudPanelStyle panel = hud_panel_style(hud, 6);

    hud_panel(&hud->canvas, rect, &panel);
    hud_draw_stopwatch_icon(&hud->canvas, 20, 32, hud->theme->panel_border);
    hud_draw_text_top(&hud->canvas, hud->font_small, 40, 35,
                      hud->theme->panel_border, "LAP");
    hud_draw_rule(&hud->canvas, 20, 55, 160, hud->theme->segment_inactive);
    hud_draw_text_top(&hud->canvas, hud->font_small, 20, 65,
                      hud->theme->text_secondary, "LAP TIME");
    hud_draw_rule(&hud->canvas, 20, 115, 160, hud->theme->segment_inactive);
    hud_draw_text_top(&hud->canvas, hud->font_small, 20, 122,
                      hud->theme->text_secondary, "BEST");
    hud_draw_rule(&hud->canvas, 20, 160, 160, hud->theme->segment_inactive);
    hud_draw_text_top(&hud->canvas, hud->font_small, 20, 142,
                      hud->theme->text_secondary, "DELTA");
}

static void hud_draw_lap_dynamic(HudContext *hud, const VehicleState *state)
{
    char lap[24];
    char lap_time[32];
    char best[32];
    char delta[32];
    bool lap_valid = state != NULL && state->lap_valid;
    bool time_valid = lap_valid && state != NULL &&
                      isfinite(state->lap_time_s);
    bool best_valid = lap_valid && state != NULL &&
                      isfinite(state->best_lap_time_s);
    bool delta_valid = lap_valid && state != NULL &&
                       isfinite(state->delta_time_s);

    (void)snprintf(lap, sizeof(lap), "%u", lap_valid ? state->lap_number : 0U);
    hud_draw_text_top(&hud->canvas, hud->font_normal, 20, 80,
                      lap_valid ? hud->theme->text_primary :
                      hud->theme->text_secondary,
                      lap_valid ? lap : "--");
    hud_format_time(best, sizeof(best), best_valid,
                    state != NULL ? state->best_lap_time_s : 0.0);
    hud_draw_text_right(&hud->canvas, hud->font_small, 155, 122,
                        best_valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, best);
    hud_format_number(delta, sizeof(delta), delta_valid,
                      state != NULL ? state->delta_time_s : 0.0, "%+.2f", "");
    hud_draw_text_right(&hud->canvas, hud->font_small, 155, 142,
                        !delta_valid ? hud->theme->text_secondary :
                        (state->delta_time_s <= 0.0 ? hud->theme->green :
                         hud->theme->red), delta);
    hud_format_time(lap_time, sizeof(lap_time), time_valid,
                    state != NULL ? state->lap_time_s : 0.0);
    hud_draw_text_right(&hud->canvas, hud->font_small, 155, 80,
                        time_valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, lap_time);
}

static void hud_draw_accel_event_static(HudContext *hud)
{
    const HudRect rect = hud->layout.accel_event;
    const HudPanelStyle panel = hud_panel_style(hud, 6);

    hud_panel(&hud->canvas, rect, &panel);
    hud_draw_flag_icon(&hud->canvas, 20, 212, hud->theme->panel_border);
    hud_draw_text_top(&hud->canvas, hud->font_small, 42, 213,
                      hud->theme->panel_border, "ACCEL EVENT");
    hud_draw_rule(&hud->canvas, 20, 230, 160, hud->theme->segment_inactive);
    hud_draw_text_top(&hud->canvas, hud->font_small, 30, 236,
                      hud->theme->text_secondary, "50m");
    hud_draw_rule(&hud->canvas, 20, 255, 160, hud->theme->segment_inactive);
    hud_draw_text_top(&hud->canvas, hud->font_small, 30, 262,
                      hud->theme->text_secondary, "75m");
}

static void hud_draw_accel_event_dynamic(HudContext *hud,
                                         const VehicleState *state)
{
    char time_50m[24];
    char time_75m[24];
    bool valid_50m = state != NULL && isfinite(state->accel_50m_time_s) &&
                     state->accel_50m_time_s > 0.0;
    bool valid_75m = state != NULL && isfinite(state->accel_time_s) &&
                     state->accel_time_s > 0.0;

    hud_format_number(time_50m, sizeof(time_50m), valid_50m,
                      state != NULL ? state->accel_50m_time_s : 0.0,
                      "%.2f", " s");
    hud_draw_text_right(&hud->canvas, hud->font_small, 150, 236,
                        valid_50m ? hud->theme->text_primary :
                        hud->theme->text_secondary, time_50m);
    /* The first value's glyph bitmap reaches the separator row.  Keep the
     * original draw order after moving the static separator into the cache. */
    hud_draw_rule(&hud->canvas, 20, 255, 160,
                  hud->theme->segment_inactive);
    hud_format_number(time_75m, sizeof(time_75m), valid_75m,
                      state != NULL ? state->accel_time_s : 0.0,
                      "%.2f", " s");
    hud_draw_text_right(&hud->canvas, hud->font_small, 150, 262,
                        valid_75m ? hud->theme->text_primary :
                        hud->theme->text_secondary, time_75m);
}

static void hud_draw_speed_static(HudContext *hud)
{
    hud_draw_text_top(&hud->canvas, hud->font_small, 135, 465,
                      hud->theme->text_secondary, "SPEED");
    hud_draw_text_center(&hud->canvas, hud->font_small, 167, 535,
                         hud->theme->text_secondary, "km/h");
}

static void hud_draw_speed_dynamic(HudContext *hud, const VehicleState *state,
                                   bool recording)
{
    char speed[24];
    bool speed_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_SPEED_KMH,
        state != NULL ? state->speed_kmh : 0.0);
    bool drive_valid = state != NULL &&
                       (state->valid_mask & VEHICLE_STATE_VALID_DRIVE_STATE) != 0U;
    bool ready = drive_valid && state->drive_state == VEHICLE_DRIVE_STATE_READY;
    const char *status = recording ? "REC" :
                         (drive_valid ? hud_drive_state_name(state->drive_state) :
                          "--");

    hud_format_number(speed, sizeof(speed), speed_valid,
                      state != NULL ? state->speed_kmh : 0.0, "%.0f", "");
    hud_draw_text_top(&hud->canvas, hud->font_speed, 132, 485,
                      speed_valid ? hud->theme->text_primary :
                      hud->theme->text_secondary,
                      speed_valid ? speed : "---");
    hud_draw_text_center(&hud->canvas, hud->font_small, 167, 558,
                         recording || ready ? hud->theme->green :
                         hud->theme->text_secondary, status);
}

static void hud_draw_fixed_segment_bar(HudContext *hud, int x, int y,
                                       int segment_count, int segment_width,
                                       int gap, int height, double value,
                                       uint32_t active_color)
{
    HudSegmentBarStyle style = {
        active_color, hud->theme->segment_inactive, 0U,
        segment_count, gap, 0, 0
    };
    int width = segment_count * segment_width +
                (segment_count - 1) * gap;

    hud_segment_bar(&hud->canvas, (HudRect){ x, y, width, height }, &style,
                    (float)value);
}

static void hud_draw_pedal_row(HudContext *hud, int label_y, int bar_y,
                               double value, bool valid, uint32_t color)
{
    char display[24];

    hud_format_number(display, sizeof(display), valid, value, "%.0f", "%");
    hud_draw_text_right(&hud->canvas, hud->font_small, 420, label_y,
                        valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, display);
    hud_draw_fixed_segment_bar(hud, 240, bar_y,
                               hud->layout.pedal_segment_count,
                               hud->layout.pedal_segment_width,
                               hud->layout.pedal_segment_gap,
                               hud->layout.pedal_segment_height,
                               valid ? hud_clamp(value, 0.0, 100.0) / 100.0 :
                               0.0, color);
}

static void hud_draw_pedal_label(HudContext *hud, int label_y,
                                 const char *label)
{
    hud_draw_text_top(&hud->canvas, hud->font_small, 240, label_y,
                      hud->theme->text_secondary, label);
}

static void hud_draw_g_meter_static(HudContext *hud)
{
    const HudLayout *layout = &hud->layout;

    hud_draw_text_center(&hud->canvas, hud->font_small, layout->g_center_x,
                         460, hud->theme->text_secondary, "G FORCE");
    ui_circle(&hud->canvas, layout->g_center_x, layout->g_center_y,
              layout->g_radius, 1, hud->theme->panel_border);
    ui_circle(&hud->canvas, layout->g_center_x, layout->g_center_y, 16, 1,
              hud->theme->segment_inactive);
    ui_line(&hud->canvas, layout->g_center_x - layout->g_radius,
            layout->g_center_y, layout->g_center_x + layout->g_radius,
            layout->g_center_y, 1, hud->theme->segment_inactive);
    ui_line(&hud->canvas, layout->g_center_x,
            layout->g_center_y - layout->g_radius, layout->g_center_x,
            layout->g_center_y + layout->g_radius, 1,
            hud->theme->segment_inactive);
}

static void hud_draw_g_meter_dynamic(HudContext *hud,
                                     const VehicleState *state)
{
    const HudLayout *layout = &hud->layout;
    bool lateral_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_ACC_Y_MPS2,
        state != NULL ? state->acc_y : 0.0);
    bool longitudinal_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_ACC_X_MPS2,
        state != NULL ? state->acc_x : 0.0);
    double lateral = lateral_valid ? state->acc_y / HUD_G_TO_MPS2 : 0.0;
    double longitudinal = longitudinal_valid ?
                          state->acc_x / HUD_G_TO_MPS2 : 0.0;
    int dot_x;
    int dot_y;

    if (!lateral_valid && !longitudinal_valid) return;
    dot_x = layout->g_center_x +
            (int)lround(hud_clamp(lateral / 1.5, -1.0, 1.0) *
                        (double)(layout->g_radius - 4));
    dot_y = layout->g_center_y -
            (int)lround(hud_clamp(longitudinal / 1.5, -1.0, 1.0) *
                        (double)(layout->g_radius - 4));
    ui_fill_circle(&hud->canvas, dot_x, dot_y, 4, hud->theme->green);
}

static void hud_draw_rpm_static(HudContext *hud)
{
    hud_draw_text_top(&hud->canvas, hud->font_small, 240, 545,
                      hud->theme->text_secondary, "MOTOR RPM");
}

static void hud_draw_rpm_dynamic(HudContext *hud, const VehicleState *state)
{
    char display[24];
    bool valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_MOTOR_RPM,
        state != NULL ? state->motor_rpm : 0.0);
    double rpm = valid ? hud_clamp(state->motor_rpm, 0.0,
                                  (double)hud->layout.rpm_max) : 0.0;
    uint32_t color = valid && rpm >= hud->layout.rpm_red_start ?
                     hud->theme->red :
                     (valid && rpm >= hud->layout.rpm_red_start / 2.0 ?
                      hud->theme->yellow : hud->theme->rpm);

    hud_format_number(display, sizeof(display), valid, rpm, "%.0f", "");
    hud_draw_text_right(&hud->canvas, hud->font_small, 420, 545,
                        valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, display);
    hud_draw_fixed_segment_bar(hud, hud->layout.rpm_bar_x,
                               hud->layout.rpm_bar_y,
                               hud->layout.rpm_segment_count,
                               hud->layout.rpm_segment_width,
                               hud->layout.rpm_segment_gap,
                               hud->layout.rpm_segment_height,
                               valid ? rpm / (double)hud->layout.rpm_max : 0.0,
                               color);
}

static void hud_draw_vehicle_info_static(HudContext *hud)
{
    hud_draw_battery_icon(&hud->canvas, 592, 465, hud->theme->panel_border);
    hud_draw_text_top(&hud->canvas, hud->font_small, 618, 466,
                      hud->theme->text_secondary, "BATTERY");
    hud_draw_rule(&hud->canvas, 590, 490, 742, hud->theme->segment_inactive);

    hud_draw_power_icon(&hud->canvas, 592, 497, hud->theme->panel_border);
    hud_draw_text_top(&hud->canvas, hud->font_small, 618, 498,
                      hud->theme->text_secondary, "POWER");
    hud_draw_rule(&hud->canvas, 590, 525, 742, hud->theme->segment_inactive);

    hud_draw_temperature_icon(&hud->canvas, 592, 530,
                              hud->theme->panel_border);
    hud_draw_text_top(&hud->canvas, hud->font_small, 620, 530,
                      hud->theme->text_secondary, "-- C");
    hud_draw_controller_icon(&hud->canvas, 665, 530,
                             hud->theme->panel_border);
    hud_draw_text_top(&hud->canvas, hud->font_small, 695, 530,
                      hud->theme->text_secondary, "-- C");
    hud_draw_rule(&hud->canvas, 590, 555, 742, hud->theme->segment_inactive);

    hud_draw_mode_icon(&hud->canvas, 592, 562, hud->theme->panel_border);
    hud_draw_text_top(&hud->canvas, hud->font_small, 618, 563,
                      hud->theme->text_secondary, "MODE");
    hud_draw_text_top(&hud->canvas, hud->font_small, 680, 563,
                      hud->theme->text_primary, "TRACK");
}

static void hud_draw_vehicle_info_dynamic(HudContext *hud,
                                          const VehicleState *state)
{
    char soc[24];
    char voltage[24];
    char power[24];
    char current[24];
    bool soc_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_HV_SOC_PERCENT,
        state != NULL ? state->hv_soc_percent : 0.0);
    bool voltage_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_HV_VOLTAGE_V,
        state != NULL ? state->hv_voltage_v : 0.0);
    bool power_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_HV_POWER_W,
        state != NULL ? state->hv_power_w : 0.0);
    bool current_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_HV_CURRENT_A,
        state != NULL ? state->hv_current_a : 0.0);

    hud_format_number(soc, sizeof(soc), soc_valid,
                      state != NULL ? state->hv_soc_percent : 0.0,
                      "%.0f", "%");
    hud_draw_text_right(&hud->canvas, hud->font_small, 685, 466,
                        soc_valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, soc);
    hud_format_number(voltage, sizeof(voltage), voltage_valid,
                      state != NULL ? state->hv_voltage_v : 0.0,
                      "%.0f", " V");
    hud_draw_text_right(&hud->canvas, hud->font_small, 742, 466,
                        voltage_valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, voltage);
    hud_format_number(power, sizeof(power), power_valid,
                      state != NULL ? state->hv_power_w / 1000.0 : 0.0,
                      "%.1f", " kW");
    hud_draw_text_top(&hud->canvas, hud->font_small, 670, 498,
                      power_valid ? hud->theme->text_primary :
                      hud->theme->text_secondary, power);
    hud_format_number(current, sizeof(current), current_valid,
                      state != NULL ? state->hv_current_a : 0.0,
                      "%.0f", " A");
    hud_draw_text_right(&hud->canvas, hud->font_small, 742, 498,
                        current_valid ? hud->theme->text_primary :
                        hud->theme->text_secondary, current);
}

static void hud_draw_main_static(HudContext *hud)
{
    const HudPanelStyle panel = hud_panel_style(hud, 8);

    hud_panel(&hud->canvas, hud->layout.main, &panel);
    ui_line(&hud->canvas, hud->layout.main_div_1_x, hud->layout.main_div_y1,
            hud->layout.main_div_1_x, hud->layout.main_div_y2, 1,
            hud->theme->segment_inactive);
    ui_line(&hud->canvas, hud->layout.main_div_2_x, hud->layout.main_div_y1,
            hud->layout.main_div_2_x, hud->layout.main_div_y2, 1,
            hud->theme->segment_inactive);
    ui_line(&hud->canvas, hud->layout.main_div_3_x, hud->layout.main_div_y1,
            hud->layout.main_div_3_x, hud->layout.main_div_y2, 1,
            hud->theme->segment_inactive);
    hud_draw_speed_static(hud);
    hud_draw_pedal_label(hud, 465, "THROTTLE");
    hud_draw_pedal_label(hud, 505, "BRAKE");
    hud_draw_rpm_static(hud);
    hud_draw_g_meter_static(hud);
    hud_draw_vehicle_info_static(hud);
}

static void hud_draw_main_dynamic(HudContext *hud, const VehicleState *state,
                                  bool recording, HudRenderProfile *profile)
{
    bool throttle_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_THROTTLE_PERCENT,
        state != NULL ? state->throttle_percent : 0.0);
    bool brake_valid = hud_state_valid(
        state, VEHICLE_STATE_VALID_BRAKE_PERCENT,
        state != NULL ? state->brake_percent : 0.0);

    if (profile != NULL) {
        uint64_t start = hud_now_us();

        hud_draw_speed_dynamic(hud, state, recording);
        profile->speed_time_us = hud_profile_elapsed(start);
    } else {
        hud_draw_speed_dynamic(hud, state, recording);
    }
    {
        uint64_t start = profile != NULL ? hud_now_us() : 0U;

    hud_draw_pedal_row(hud, 465, hud->layout.throttle_bar_y,
                       state != NULL ? state->throttle_percent : 0.0,
                       throttle_valid, hud->theme->throttle);
        if (profile != NULL) {
            profile->throttle_time_us = hud_profile_elapsed(start);
        }
    }
    {
        uint64_t start = profile != NULL ? hud_now_us() : 0U;

    hud_draw_pedal_row(hud, 505, hud->layout.brake_bar_y,
                       state != NULL ? state->brake_percent : 0.0,
                       brake_valid, hud->theme->brake);
        if (profile != NULL) {
            profile->brake_time_us = hud_profile_elapsed(start);
        }
    }
    if (profile != NULL) {
        uint64_t start = hud_now_us();

        hud_draw_rpm_dynamic(hud, state);
        profile->rpm_time_us = hud_profile_elapsed(start);
    } else {
        hud_draw_rpm_dynamic(hud, state);
    }
    if (profile != NULL) {
        uint64_t start = hud_now_us();

        hud_draw_g_meter_dynamic(hud, state);
        profile->g_meter_time_us = hud_profile_elapsed(start);
    } else {
        hud_draw_g_meter_dynamic(hud, state);
    }
    if (profile != NULL) {
        uint64_t start = hud_now_us();

        hud_draw_vehicle_info_dynamic(hud, state);
        profile->vehicle_info_time_us = hud_profile_elapsed(start);
    } else {
        hud_draw_vehicle_info_dynamic(hud, state);
    }
}

static bool hud_canvas_copy_valid(const UiCanvas *canvas)
{
    if (canvas == NULL || canvas->pixels == NULL || canvas->width <= 0 ||
        canvas->height <= 0 || canvas->stride <= 0 ||
        canvas->width > INT_MAX / 4 ||
        canvas->stride < canvas->width * 4) {
        return false;
    }
    return (size_t)canvas->height <= SIZE_MAX / (size_t)canvas->stride;
}

static bool hud_build_static_cache(HudContext *hud)
{
    size_t row_bytes = (size_t)HUD_UI_WIDTH * 4U;

    if (hud == NULL || (size_t)HUD_UI_HEIGHT > SIZE_MAX / row_bytes) {
        return false;
    }
    hud->static_bytes = row_bytes * HUD_UI_HEIGHT;
    hud->static_pixels = calloc(1U, hud->static_bytes);
    if (hud->static_pixels == NULL) return false;
    hud->static_canvas = (UiCanvas){ hud->static_pixels, HUD_UI_WIDTH,
                                    HUD_UI_HEIGHT, (int)row_bytes };
    hud->canvas = hud->static_canvas;
    ui_clear(&hud->static_canvas, hud->theme->transparent);
    hud_draw_lap_static(hud);
    hud_draw_accel_event_static(hud);
    hud_draw_main_static(hud);
    hud->static_ready = true;
    return true;
}

static bool hud_copy_static_cache(const HudContext *hud, UiCanvas *canvas)
{
    int copy_width;
    int copy_height;
    size_t row_bytes;
    int row;

    if (hud == NULL || !hud->static_ready || hud->static_pixels == NULL ||
        !hud_canvas_copy_valid(canvas)) {
        return false;
    }
    copy_width = canvas->width < hud->static_canvas.width ?
                 canvas->width : hud->static_canvas.width;
    copy_height = canvas->height < hud->static_canvas.height ?
                  canvas->height : hud->static_canvas.height;
    row_bytes = (size_t)copy_width * 4U;
    for (row = 0; row < copy_height; ++row) {
        memcpy(canvas->pixels + (size_t)row * (size_t)canvas->stride,
               hud->static_pixels + (size_t)row *
                   (size_t)hud->static_canvas.stride,
               row_bytes);
    }
    return true;
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
    if (!hud_build_static_cache(hud)) {
        hud_shutdown(hud);
        return -1;
    }
    *context = hud;
    return 0;
}

void hud_shutdown(HudContext *context)
{
    if (context == NULL) return;
    free(context->static_pixels);
    ui_font_free(context->font_speed);
    ui_font_free(context->font_normal);
    ui_font_free(context->font_small);
    free(context);
}

int hud_render_profiled(HudContext *context, UiCanvas *canvas,
                        const VehicleState *state, bool recording,
                        HudRenderProfile *profile)
{
    uint64_t start;
    uint64_t main_time;

    if (context == NULL || canvas == NULL || canvas->pixels == NULL ||
        canvas->width <= 0 || canvas->height <= 0 || canvas->stride <= 0) {
        return -1;
    }
    if (profile != NULL) memset(profile, 0, sizeof(*profile));
    start = profile != NULL ? hud_now_us() : 0U;
    if (!hud_copy_static_cache(context, canvas)) return -1;
    if (profile != NULL) {
        profile->static_copy_time_us = hud_profile_elapsed(start);
    }
    context->canvas = *canvas;
    start = profile != NULL ? hud_now_us() : 0U;
    hud_draw_lap_dynamic(context, state);
    if (profile != NULL) {
        profile->lap_time_us = hud_profile_elapsed(start);
    }
    start = profile != NULL ? hud_now_us() : 0U;
    hud_draw_accel_event_dynamic(context, state);
    if (profile != NULL) {
        profile->accel_event_time_us = hud_profile_elapsed(start);
    }
    start = profile != NULL ? hud_now_us() : 0U;
    hud_draw_main_dynamic(context, state, recording, profile);
    main_time = profile != NULL ? hud_profile_elapsed(start) : 0U;
    if (profile != NULL) {
        profile->total_time_us = main_time +
                                 profile->static_copy_time_us +
                                 profile->lap_time_us +
                                 profile->accel_event_time_us;
    }
    return 0;
}

int hud_render(HudContext *context, UiCanvas *canvas,
               const VehicleState *state, bool recording)
{
    return hud_render_profiled(context, canvas, state, recording, NULL);
}
