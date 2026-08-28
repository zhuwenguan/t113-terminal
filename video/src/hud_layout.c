#include "hud_layout.h"

#include <string.h>

void hud_layout_default(HudLayout *layout)
{
    if (layout == NULL) return;
    memset(layout, 0, sizeof(*layout));
    layout->static_frame = (HudRect){ 0, 0, 800U, 600U };
    layout->left_top_panel = (HudRect){ 12, 12, 220U, 120U };
    layout->right_top_panel = (HudRect){ 568, 12, 220U, 120U };
    /* V3's visible RPM arc occupies y=57..118 inside its canvas.  Moving the
     * canvas down keeps that arc close to the bottom edge while leaving a
     * clear gap for the gear frame above it. */
    layout->rpm_arc = (HudRect){ 140, 480, 520U, 170U };
    layout->center_gear = (HudRect){ 320, 420, 160U, 110U };
    /* Keep the G meter on the left, centered vertically in the 600 px frame. */
    layout->g_meter = (HudRect){ 12, 220, 160U, 160U };
    layout->pedal_bar = (HudRect){ 668, 408, 120U, 180U };
    layout->status_pill = (HudRect){ 246, 12, 100U, 28U };
    layout->warning_status = (HudRect){ 246, 12, 100U, 28U };
    layout->rpm_max = 10000.0;
    layout->rpm_red_start = 8000.0;
    /* V3 contains 24 complete cells.  The X span is refreshed from the
     * decode-time alpha bbox when the selected asset pack is loaded. */
    layout->rpm_segments = 24U;
    layout->rpm_content_x = 29U;
    layout->rpm_content_width = 462U;
    layout->g_max = 1.5;
    layout->g_radius = 52.0;
    layout->g_filter_alpha = 0.25;
    layout->pedal_segments = 10U;
    layout->brake_pressure_max = 100.0;
    layout->brake_input_is_pressure = false;
    layout->text_size_small = 1;
    layout->text_size_value = 2;
    layout->text_size_gear = 8;
}
