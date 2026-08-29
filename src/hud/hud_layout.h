#ifndef T113_UI_HUD_LAYOUT_H
#define T113_UI_HUD_LAYOUT_H

#include "hud_widget.h"

#include <stddef.h>
#include <stdint.h>

#define HUD_UI_WIDTH 800
#define HUD_UI_HEIGHT 600

typedef struct {
    HudRect lap;
    HudRect speed;
    HudRect vehicle_info;
    HudRect g_meter;
    HudRect throttle;
    HudRect brake;
    HudRect rpm;
    HudRect accel_event;
    HudRect status;
    int segment_count;
    int rpm_max;
    int rpm_red_start;
} HudLayout;

/* All rectangles are fixed 800x600 design-space coordinates. */
static inline void hud_ui_layout_default(HudLayout *layout)
{
    if (layout == NULL) return;
    *layout = (HudLayout){
        .lap = { 24, 20, 220, 130 },
        .speed = { 260, 20, 280, 190 },
        .vehicle_info = { 556, 20, 220, 260 },
        .g_meter = { 24, 180, 220, 100 },
        .throttle = { 24, 466, 240, 105 },
        .brake = { 280, 466, 240, 105 },
        .rpm = { 536, 466, 240, 105 },
        .accel_event = { 24, 300, 220, 130 },
        .status = { 300, 220, 200, 32 },
        .segment_count = 16,
        .rpm_max = 10000,
        .rpm_red_start = 8000
    };
}

#endif
