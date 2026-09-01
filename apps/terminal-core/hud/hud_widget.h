#ifndef T113_HUD_WIDGET_H
#define T113_HUD_WIDGET_H

#include "ui_draw.h"
#include "ui_font.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x;
    int y;
    int w;
    int h;
} HudRect;

typedef struct {
    uint32_t background_color;
    uint32_t border_color;
    int border_width;
    int radius;
} HudPanelStyle;

void hud_panel(UiCanvas *canvas, HudRect rect,
               const HudPanelStyle *style);

typedef enum {
    HUD_WIDGET_ALIGN_LEFT = 0,
    HUD_WIDGET_ALIGN_CENTER = 1,
    HUD_WIDGET_ALIGN_RIGHT = 2
} HudAlign;

typedef struct {
    UiFont *label_font;
    UiFont *value_font;
    UiFont *unit_font;

    uint32_t label_color;
    uint32_t value_color;
    uint32_t unit_color;

    int label_gap;
    int unit_gap;
    HudAlign align;
} HudTextValueStyle;

/* rect is the layout box; label is above the value and unit baseline. */
void hud_text_value(UiCanvas *canvas, HudRect rect,
                    const HudTextValueStyle *style, const char *label,
                    const char *value, const char *unit);

typedef struct {
    uint32_t active_color;
    uint32_t inactive_color;
    uint32_t border_color;
    int segment_count;
    int gap;
    int border_width;
    int radius;
} HudSegmentBarStyle;

/* A horizontal bar.  active_count is floor(normalized_value * segment_count). */
void hud_segment_bar(UiCanvas *canvas, HudRect rect,
                     const HudSegmentBarStyle *style,
                     float normalized_value);

typedef struct {
    UiFont *font;
    uint32_t active_color;
    uint32_t inactive_color;
    uint32_t text_color;
    int dot_radius;
    int gap;
} HudStatusStyle;

/* x,y is the top-left of the indicator row. */
void hud_status_indicator(UiCanvas *canvas, int x, int y,
                          const HudStatusStyle *style, bool active,
                          const char *text);

#endif
