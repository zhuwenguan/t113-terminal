#include "hud_widget.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static int hud_int_from_i64(int64_t value)
{
    if (value < INT_MIN) return INT_MIN;
    if (value > INT_MAX) return INT_MAX;
    return (int)value;
}

static bool hud_text_present(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static bool hud_measure_text(UiFont *font, const char *text, int *width,
                             int *height)
{
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (font == NULL || !hud_text_present(text) || width == NULL ||
        height == NULL) {
        return false;
    }
    return ui_text_measure(font, text, width, height) == 0 && *width >= 0 &&
           *height > 0;
}

static int hud_text_x(HudRect rect, int64_t text_width, HudAlign align)
{
    int64_t x = rect.x;

    if (align == HUD_WIDGET_ALIGN_CENTER) {
        x += ((int64_t)rect.w - (int64_t)text_width) / 2;
    } else if (align == HUD_WIDGET_ALIGN_RIGHT) {
        x += (int64_t)rect.w - (int64_t)text_width;
    }
    return hud_int_from_i64(x);
}

void hud_panel(UiCanvas *canvas, HudRect rect,
               const HudPanelStyle *style)
{
    if (style == NULL || rect.w <= 0 || rect.h <= 0) return;

    if (style->radius > 0) {
        ui_fill_round_rect(canvas, rect.x, rect.y, rect.w, rect.h,
                           style->radius, style->background_color);
        if (style->border_width > 0) {
            ui_round_rect(canvas, rect.x, rect.y, rect.w, rect.h,
                          style->radius, style->border_width,
                          style->border_color);
        }
        return;
    }

    ui_fill_rect(canvas, rect.x, rect.y, rect.w, rect.h,
                 style->background_color);
    if (style->border_width > 0) {
        ui_rect(canvas, rect.x, rect.y, rect.w, rect.h,
                style->border_width, style->border_color);
    }
}

void hud_text_value(UiCanvas *canvas, HudRect rect,
                    const HudTextValueStyle *style, const char *label,
                    const char *value, const char *unit)
{
    int label_width;
    int label_height;
    int value_width;
    int value_height;
    int unit_width;
    int unit_height;
    bool has_label;
    bool has_value;
    bool has_unit;
    int gap_label;
    int gap_unit;
    int64_t label_baseline;
    int64_t value_baseline;

    if (style == NULL || rect.w <= 0 || rect.h <= 0) return;

    has_label = hud_measure_text(style->label_font, label, &label_width,
                                 &label_height);
    has_value = hud_measure_text(style->value_font, value, &value_width,
                                 &value_height);
    has_unit = hud_measure_text(style->unit_font, unit, &unit_width,
                                &unit_height);
    if (!has_label && !has_value && !has_unit) return;

    gap_label = style->label_gap > 0 ? style->label_gap : 0;
    gap_unit = style->unit_gap > 0 ? style->unit_gap : 0;
    label_baseline = rect.y;
    if (has_label) {
        label_baseline += label_height;
        (void)ui_text(canvas, style->label_font,
                      hud_text_x(rect, label_width, style->align),
                      hud_int_from_i64(label_baseline), style->label_color,
                      label);
        value_baseline = label_baseline + gap_label;
    } else {
        value_baseline = rect.y;
    }

    if (has_value) {
        int64_t run_width = value_width;

        if (has_unit) run_width += gap_unit + unit_width;
        value_baseline += value_height;
        {
            int value_x = hud_text_x(rect, run_width, style->align);

            (void)ui_text(canvas, style->value_font, value_x,
                          hud_int_from_i64(value_baseline), style->value_color,
                          value);
            if (has_unit) {
                int64_t unit_x = (int64_t)value_x + value_width + gap_unit;

                (void)ui_text(canvas, style->unit_font,
                              hud_int_from_i64(unit_x),
                              hud_int_from_i64(value_baseline),
                              style->unit_color, unit);
            }
        }
    } else if (has_unit) {
        value_baseline += unit_height;
        (void)ui_text(canvas, style->unit_font,
                      hud_text_x(rect, unit_width, style->align),
                      hud_int_from_i64(value_baseline), style->unit_color,
                      unit);
    }
}

void hud_segment_bar(UiCanvas *canvas, HudRect rect,
                     const HudSegmentBarStyle *style,
                     float normalized_value)
{
    int64_t segment_count;
    int64_t gap;
    int64_t available;
    int64_t segment_width;
    int64_t remainder;
    int64_t active_count;
    int64_t segment_x;
    int64_t index;

    if (style == NULL || rect.w <= 0 || rect.h <= 0 ||
        style->segment_count <= 0) {
        return;
    }

    segment_count = style->segment_count;
    gap = style->gap > 0 ? style->gap : 0;
    available = (int64_t)rect.w - gap * (segment_count - 1);
    if (available <= 0) return;
    segment_width = available / segment_count;
    if (segment_width <= 0) return;
    remainder = available % segment_count;

    /* NaN is treated like a value below zero; infinities clamp normally. */
    if (!(normalized_value > 0.0f)) {
        normalized_value = 0.0f;
    } else if (normalized_value >= 1.0f) {
        normalized_value = 1.0f;
    }
    active_count = (int64_t)((double)normalized_value *
                             (double)segment_count);
    if (active_count < 0) active_count = 0;
    if (active_count > segment_count) active_count = segment_count;

    segment_x = rect.x;
    for (index = 0; index < segment_count; ++index) {
        int64_t current_width = segment_width +
                                (index < remainder ? 1 : 0);
        uint32_t color = index < active_count ? style->active_color :
                         style->inactive_color;

        if (style->radius > 0) {
            int64_t segment_radius = style->radius;
            int64_t limit = current_width / 2 < rect.h / 2 ?
                            current_width / 2 : rect.h / 2;

            if (segment_radius > limit) segment_radius = limit;
            ui_fill_round_rect(canvas, hud_int_from_i64(segment_x), rect.y,
                               hud_int_from_i64(current_width), rect.h,
                               hud_int_from_i64(segment_radius), color);
        } else {
            ui_fill_rect(canvas, hud_int_from_i64(segment_x), rect.y,
                         hud_int_from_i64(current_width), rect.h, color);
        }
        segment_x += current_width + gap;
    }

    if (style->border_width > 0) {
        if (style->radius > 0) {
            ui_round_rect(canvas, rect.x, rect.y, rect.w, rect.h,
                          style->radius, style->border_width,
                          style->border_color);
        } else {
            ui_rect(canvas, rect.x, rect.y, rect.w, rect.h,
                    style->border_width, style->border_color);
        }
    }
}

void hud_status_indicator(UiCanvas *canvas, int x, int y,
                          const HudStatusStyle *style, bool active,
                          const char *text)
{
    int text_width;
    int text_height;
    int radius;
    int gap;
    bool has_text;
    int64_t line_height;
    int64_t dot_center_x;
    int64_t dot_center_y;
    int64_t text_x;

    if (style == NULL) return;
    radius = style->dot_radius > 0 ? style->dot_radius : 0;
    gap = style->gap > 0 ? style->gap : 0;
    has_text = hud_measure_text(style->font, text, &text_width,
                                &text_height);
    line_height = has_text ? text_height : (int64_t)radius * 2;

    if (radius > 0) {
        dot_center_x = (int64_t)x + radius;
        dot_center_y = (int64_t)y + line_height / 2;
        ui_fill_circle(canvas, hud_int_from_i64(dot_center_x),
                       hud_int_from_i64(dot_center_y), radius,
                       active ? style->active_color : style->inactive_color);
    }
    if (!has_text) return;

    text_x = (int64_t)x + (int64_t)radius * 2 + gap;
    (void)ui_text(canvas, style->font, hud_int_from_i64(text_x),
                  hud_int_from_i64((int64_t)y + text_height),
                  style->text_color, text);
}
