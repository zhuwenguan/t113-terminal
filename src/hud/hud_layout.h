#ifndef T113_UI_HUD_LAYOUT_H
#define T113_UI_HUD_LAYOUT_H

#include "hud_widget.h"

#include <stddef.h>
#include <stdint.h>

#define HUD_UI_WIDTH 800
#define HUD_UI_HEIGHT 600

typedef struct {
    HudRect lap;
    HudRect accel_event;
    HudRect main;
    HudRect speed;
    HudRect throttle;
    HudRect brake;
    HudRect rpm;
    HudRect g_meter;
    HudRect vehicle_info;
    int segment_count;
    int rpm_max;
    int rpm_red_start;
    int main_div_1_x;
    int main_div_2_x;
    int main_div_3_x;
    int main_div_y1;
    int main_div_y2;
    int pedal_bar_x;
    int throttle_bar_y;
    int brake_bar_y;
    int rpm_bar_x;
    int rpm_bar_y;
    int pedal_segment_count;
    int pedal_segment_width;
    int pedal_segment_gap;
    int pedal_segment_height;
    int rpm_segment_count;
    int rpm_segment_width;
    int rpm_segment_gap;
    int rpm_segment_height;
    int g_center_x;
    int g_center_y;
    int g_radius;
} HudLayout;

/* All rectangles are fixed 800x600 design-space coordinates. */
static inline void hud_ui_layout_default(HudLayout *layout)
{
    if (layout == NULL) return;
    *layout = (HudLayout){
        .lap = { 10, 25, 160, 170 },
        .accel_event = { 10, 205, 160, 80 },
        .main = { 115, 450, 640, 140 },
        .speed = { 115, 450, 105, 140 },
        .throttle = { 220, 450, 215, 40 },
        .brake = { 220, 490, 215, 40 },
        .rpm = { 220, 530, 215, 60 },
        .g_meter = { 435, 450, 135, 140 },
        .vehicle_info = { 570, 450, 185, 140 },
        .segment_count = 16,
        .rpm_max = 10000,
        .rpm_red_start = 8000,
        .main_div_1_x = 220,
        .main_div_2_x = 435,
        .main_div_3_x = 570,
        .main_div_y1 = 460,
        .main_div_y2 = 580,
        .pedal_bar_x = 240,
        .throttle_bar_y = 485,
        .brake_bar_y = 525,
        .rpm_bar_x = 240,
        .rpm_bar_y = 565,
        .pedal_segment_count = 16,
        .pedal_segment_width = 6,
        .pedal_segment_gap = 2,
        .pedal_segment_height = 10,
        .rpm_segment_count = 20,
        .rpm_segment_width = 4,
        .rpm_segment_gap = 2,
        .rpm_segment_height = 9,
        .g_center_x = 502,
        .g_center_y = 528,
        .g_radius = 32
    };
}

#endif
