#include "hud_layout.h"

#include <string.h>

void hud_layout_default(HudLayout *layout)
{
    if (layout == NULL) return;
    memset(layout, 0, sizeof(*layout));
    layout->static_frame = (HudRect){ 0, 0, 800U, 600U };
    layout->left_top_panel = (HudRect){ 12, 12, 220U, 120U };
    layout->right_top_panel = (HudRect){ 568, 12, 220U, 120U };
    layout->rpm_arc = (HudRect){ 140, 430, 520U, 170U };
    layout->center_gear = (HudRect){ 320, 480, 160U, 110U };
    layout->g_meter = (HudRect){ 12, 428, 160U, 160U };
    layout->pedal_bar = (HudRect){ 668, 408, 120U, 180U };
    layout->status_pill = (HudRect){ 246, 12, 100U, 28U };
    layout->warning_status = (HudRect){ 246, 12, 100U, 28U };
    layout->rpm_max = 10000.0;
    layout->rpm_red_start = 8000.0;
    layout->rpm_segments = 25U;
    /* The supplied fill has a 24-pixel transparent lead-in before its first
     * segment and 473 pixels of actual segment span. */
    layout->rpm_content_x = 24U;
    layout->rpm_content_width = 473U;
    layout->g_max = 1.5;
    layout->g_radius = 52.0;
    layout->g_filter_alpha = 0.25;
    layout->pedal_active_height = 140U;
    layout->brake_pressure_max = 100.0;
    layout->brake_input_is_pressure = false;
    layout->text_size_small = 1;
    layout->text_size_value = 2;
    layout->text_size_gear = 8;
}
