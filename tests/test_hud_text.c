#include "hud_renderer.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void clear_layer(HudLayer *layer)
{
    memset(layer->pixels, 0, (size_t)layer->stride * layer->height);
}

static void find_alpha_bounds(const HudLayer *layer, uint32_t *min_x,
                              uint32_t *max_x, uint32_t *min_y,
                              uint32_t *max_y)
{
    uint32_t x;
    uint32_t y;

    *min_x = layer->width;
    *max_x = 0U;
    *min_y = layer->height;
    *max_y = 0U;
    for (y = 0U; y < layer->height; ++y) {
        for (x = 0U; x < layer->width; ++x) {
            if (layer->pixels[(size_t)y * layer->stride + (size_t)x * 4U +
                              3U] != 0U) {
                if (x < *min_x) *min_x = x;
                if (x > *max_x) *max_x = x;
                if (y < *min_y) *min_y = y;
                if (y > *max_y) *max_y = y;
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *font_path = argc > 1 ? argv[1] : HUD_DEFAULT_FONT_PATH;
    const uint32_t sizes[] = { 16U, 24U, 42U, 20U, 72U };
    const char *sample = "GEAR 4 RPM 7850 SPEED 142 km/h -0.382 76% 68 °C";
    HudTextAtlas atlas;
    HudTextMetrics metrics;
    HudLayer layer;
    uint8_t *pixels;
    uint32_t min_x;
    uint32_t max_x;
    uint32_t min_y;
    uint32_t max_y;
    uint64_t misses_after_warm;
    size_t index;
    bool saw_antialias = false;

    memset(&atlas, 0, sizeof(atlas));
    assert(hud_text_atlas_init(&atlas, font_path) == 0);
    assert(hud_text_atlas_warm(&atlas, sizes,
                               sizeof(sizes) / sizeof(sizes[0])));
    assert(hud_text_cached_glyph_count(&atlas) >= 300U);
    assert(hud_text_cache_bytes(&atlas) > 0U);
    assert(hud_text_measure(&atlas, "1234567890", 24U, &metrics));
    assert(metrics.width > 0U && metrics.height > 0U);
    assert(hud_text_width(&atlas, "I", 24U) !=
           hud_text_width(&atlas, "W", 24U));

    pixels = calloc(1U, 400U * 160U * 4U);
    assert(pixels != NULL);
    memset(&layer, 0, sizeof(layer));
    layer.width = 400U;
    layer.height = 160U;
    layer.stride = 400U * 4U;
    layer.pixels = pixels;
    layer.pixel_format = HUD_PIXEL_FORMAT_RGBA8888;
    misses_after_warm = atlas.cache_misses;
    assert(hud_text_draw(&atlas, &layer, 20, 100, 24U,
                         (HudTextColor){ 245U, 249U, 252U, 200U },
                         HUD_ALIGN_LEFT, sample));
    assert(atlas.cache_misses == misses_after_warm);
    for (index = 0U; index < (size_t)layer.width * layer.height; ++index) {
        uint8_t alpha = pixels[index * 4U + 3U];

        if (alpha > 0U && alpha < 200U) {
            saw_antialias = true;
            break;
        }
    }
    assert(saw_antialias);

    clear_layer(&layer);
    assert(hud_text_draw(&atlas, &layer, 200, 60, 24U,
                         (HudTextColor){ 44U, 210U, 232U, 255U },
                         HUD_ALIGN_RIGHT, "I"));
    find_alpha_bounds(&layer, &min_x, &max_x, &min_y, &max_y);
    assert(max_x <= 200U);
    assert(min_y < 60U && max_y >= 60U - 24U);

    clear_layer(&layer);
    assert(hud_text_draw(&atlas, &layer, 200, 60, 24U,
                         (HudTextColor){ 64U, 230U, 122U, 255U },
                         HUD_ALIGN_CENTER, "123"));
    find_alpha_bounds(&layer, &min_x, &max_x, &min_y, &max_y);
    assert(min_x < 200U && max_x > 200U);

    free(pixels);
    hud_text_atlas_close(&atlas);
    hud_text_atlas_close(&atlas);
    return 0;
}
