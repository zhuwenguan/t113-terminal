#ifndef T113_HUD_LAYOUT_H
#define T113_HUD_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x;
    int y;
    uint32_t width;
    uint32_t height;
} HudRect;

typedef struct {
    HudRect static_frame;
    HudRect left_top_panel;
    HudRect right_top_panel;
    HudRect rpm_arc;
    HudRect speed_plate;
    HudRect center_gear;
    HudRect g_meter;
    HudRect pedal_bar;
    HudRect status_pill;
    HudRect warning_status;
    double rpm_max;
    double rpm_red_start;
    uint32_t rpm_segments;
    uint32_t rpm_content_x;
    uint32_t rpm_content_width;
    double g_max;
    double g_radius;
    double g_filter_alpha;
    uint32_t pedal_segments;
    double brake_pressure_max;
    bool brake_input_is_pressure;
    int text_size_small;
    int text_size_value;
    int text_size_speed;
    int text_size_rpm;
    int text_size_gear;
} HudLayout;

void hud_layout_default(HudLayout *layout);

#endif
