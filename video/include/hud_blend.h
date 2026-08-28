#ifndef T113_HUD_BLEND_H
#define T113_HUD_BLEND_H

#include "hud_assets.h"
#include "hud_renderer.h"

/* All coordinates are clipped before a pixel is touched.  Source images are
 * resident HudAssets buffers; these calls never allocate. */
void hud_blend_pixel(HudLayer *destination, int x, int y, uint8_t red,
                     uint8_t green, uint8_t blue, uint8_t alpha);
bool hud_blend_coverage(HudLayer *destination, int destination_x,
                        int destination_y, uint32_t width, uint32_t height,
                        const uint8_t *coverage, uint32_t coverage_stride,
                        uint8_t red, uint8_t green, uint8_t blue,
                        uint8_t alpha);
bool hud_blend_image(const HudImage *source, HudLayer *destination,
                     int destination_x, int destination_y,
                     uint32_t destination_width, uint32_t destination_height);
bool hud_blend_image_region(const HudImage *source, HudLayer *destination,
                            uint32_t source_x, uint32_t source_y,
                            uint32_t source_width, uint32_t source_height,
                            int destination_x, int destination_y,
                            uint32_t destination_width,
                            uint32_t destination_height);

#endif
