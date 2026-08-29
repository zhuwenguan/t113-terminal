#ifndef T113_HUD_UI_RENDER_H
#define T113_HUD_UI_RENDER_H

#include "ui_draw.h"

#include <race/vehicle_state.h>

#include <stdbool.h>

typedef struct HudContext HudContext;

/* Loads all persistent Phase 4 resources.  The caller owns the context. */
int hud_init(HudContext **context, const char *font_path);
void hud_shutdown(HudContext *context);

/* Renders one complete fixed 800x600 HUD into an ARGB8888 canvas. */
int hud_render(HudContext *context, UiCanvas *canvas,
               const VehicleState *state, bool recording);

#endif
