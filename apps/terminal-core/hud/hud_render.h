#ifndef T113_HUD_UI_RENDER_H
#define T113_HUD_UI_RENDER_H

#include "ui_draw.h"

#include <race/vehicle_state.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct HudContext HudContext;

typedef struct {
    uint64_t clear_time_us;
    uint64_t static_copy_time_us;
    uint64_t lap_time_us;
    uint64_t accel_event_time_us;
    uint64_t speed_time_us;
    uint64_t throttle_time_us;
    uint64_t brake_time_us;
    uint64_t rpm_time_us;
    uint64_t g_meter_time_us;
    uint64_t vehicle_info_time_us;
    uint64_t total_time_us;
} HudRenderProfile;

/* Loads all persistent Phase 4 resources.  The caller owns the context. */
int hud_init(HudContext **context, const char *font_path);
void hud_shutdown(HudContext *context);

/* Renders one complete fixed 800x600 HUD into an ARGB8888 canvas. */
int hud_render(HudContext *context, UiCanvas *canvas,
               const VehicleState *state, bool recording);

/* Benchmark-only entry point; a NULL profile keeps the production path lean. */
int hud_render_profiled(HudContext *context, UiCanvas *canvas,
                        const VehicleState *state, bool recording,
                        HudRenderProfile *profile);

#endif
