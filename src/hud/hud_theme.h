#ifndef T113_HUD_THEME_H
#define T113_HUD_THEME_H

#include "ui_draw.h"

typedef struct {
    uint32_t transparent;
    uint32_t panel_bg;
    uint32_t panel_border;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t green;
    uint32_t yellow;
    uint32_t red;
    uint32_t throttle;
    uint32_t brake;
    uint32_t rpm;
    uint32_t segment_inactive;
} HudTheme;

/* Fixed Phase 4 palette.  Colors are AARRGGBB and are written as B,G,R,A. */
#define HUD_THEME_DEFAULT { \
    UI_COLOR_TRANSPARENT, \
    UI_ARGB(166U, 0U, 8U, 14U), \
    UI_ARGB(230U, 44U, 210U, 232U), \
    UI_ARGB(255U, 245U, 249U, 252U), \
    UI_ARGB(235U, 143U, 180U, 190U), \
    UI_ARGB(245U, 64U, 230U, 122U), \
    UI_ARGB(245U, 255U, 210U, 64U), \
    UI_ARGB(245U, 255U, 66U, 72U), \
    UI_ARGB(245U, 44U, 210U, 232U), \
    UI_ARGB(245U, 255U, 66U, 72U), \
    UI_ARGB(245U, 255U, 210U, 64U), \
    UI_ARGB(135U, 55U, 75U, 85U) \
}

#endif
