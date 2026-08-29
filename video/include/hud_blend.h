#ifndef T113_HUD_BLEND_H
#define T113_HUD_BLEND_H

#include "hud_renderer.h"

/* All coordinates are clipped before a pixel is touched. */
void hud_blend_pixel(HudLayer *destination, int x, int y, uint8_t red,
                     uint8_t green, uint8_t blue, uint8_t alpha);
bool hud_blend_coverage(HudLayer *destination, int destination_x,
                        int destination_y, uint32_t width, uint32_t height,
                        const uint8_t *coverage, uint32_t coverage_stride,
                        uint8_t red, uint8_t green, uint8_t blue,
                        uint8_t alpha);

#endif
