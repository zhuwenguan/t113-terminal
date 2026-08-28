#ifndef T113_HUD_TEXT_H
#define T113_HUD_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HudLayer HudLayer;

typedef struct {
    uint8_t *coverage;
    uint32_t glyph_count;
    uint32_t glyph_width;
    uint32_t glyph_height;
    uint32_t advance;
    uint32_t stride;
    size_t bytes;
    bool initialized;
} HudTextAtlas;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} HudTextColor;

int hud_text_atlas_init(HudTextAtlas *atlas);
void hud_text_atlas_close(HudTextAtlas *atlas);
uint32_t hud_text_width(const HudTextAtlas *atlas, const char *text,
                        uint32_t pixel_size);
bool hud_text_draw(const HudTextAtlas *atlas, HudLayer *layer, int x, int y,
                   const char *text, uint32_t pixel_size, HudTextColor color);

#endif
