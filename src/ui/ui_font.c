#include "ui_font.h"

#include "hud_renderer.h"
#include "hud_text.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct UiFont {
    HudTextAtlas atlas;
    uint32_t pixel_size;
};

static bool ui_font_canvas_valid(const UiCanvas *canvas)
{
    if (canvas == NULL || canvas->pixels == NULL || canvas->width <= 0 ||
        canvas->height <= 0 || canvas->stride <= 0 ||
        canvas->width > INT_MAX / 4 ||
        canvas->stride < canvas->width * 4) {
        return false;
    }
    return (size_t)canvas->height <= SIZE_MAX / (size_t)canvas->stride;
}

static HudLayer ui_font_layer_from_canvas(const UiCanvas *canvas)
{
    HudLayer layer;

    memset(&layer, 0, sizeof(layer));
    layer.width = (uint32_t)canvas->width;
    layer.height = (uint32_t)canvas->height;
    layer.stride = (uint32_t)canvas->stride;
    layer.pixels = canvas->pixels;
    layer.pixel_format = HUD_PIXEL_FORMAT_ARGB8888;
    return layer;
}

static HudTextColor ui_font_color(uint32_t color)
{
    HudTextColor result;

    result.red = (uint8_t)((color >> 16U) & UINT32_C(0xff));
    result.green = (uint8_t)((color >> 8U) & UINT32_C(0xff));
    result.blue = (uint8_t)(color & UINT32_C(0xff));
    result.alpha = (uint8_t)((color >> 24U) & UINT32_C(0xff));
    return result;
}

UiFont *ui_font_load(const char *path, int pixel_size)
{
    UiFont *font;

    if (path == NULL || path[0] == '\0' || pixel_size <= 0 ||
        pixel_size > UINT16_MAX) {
        return NULL;
    }
    font = calloc(1U, sizeof(*font));
    if (font == NULL) return NULL;
    if (hud_text_atlas_init(&font->atlas, path) < 0) {
        free(font);
        return NULL;
    }
    font->pixel_size = (uint32_t)pixel_size;
    return font;
}

void ui_font_free(UiFont *font)
{
    if (font == NULL) return;
    hud_text_atlas_close(&font->atlas);
    free(font);
}

int ui_text(UiCanvas *canvas, UiFont *font, int x, int baseline_y,
            uint32_t color, const char *text)
{
    HudLayer layer;

    if (!ui_font_canvas_valid(canvas) || font == NULL || text == NULL) {
        return -1;
    }
    layer = ui_font_layer_from_canvas(canvas);
    return hud_text_draw(&font->atlas, &layer, x, baseline_y,
                         font->pixel_size, ui_font_color(color),
                         HUD_ALIGN_LEFT, text) ? 0 : -1;
}

int ui_text_measure(UiFont *font, const char *text, int *width, int *height)
{
    HudTextMetrics metrics;

    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (font == NULL || text == NULL || width == NULL || height == NULL ||
        !hud_text_measure(&font->atlas, text, font->pixel_size, &metrics) ||
        metrics.width > (uint32_t)INT_MAX ||
        metrics.height > (uint32_t)INT_MAX) {
        return -1;
    }
    *width = (int)metrics.width;
    *height = (int)metrics.height;
    return 0;
}
