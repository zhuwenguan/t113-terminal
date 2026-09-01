#define _POSIX_C_SOURCE 200809L

#include "hud_renderer.h"

#include "hud_resources.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HUD_BYTES_PER_PIXEL 4U

static uint64_t hud_renderer_now_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0U;
    }
    return (uint64_t)timestamp.tv_sec * 1000000U +
           (uint64_t)timestamp.tv_nsec / 1000U;
}

static bool hud_renderer_size_valid(uint32_t width, uint32_t height,
                                    size_t *bytes)
{
    size_t row_bytes;

    if (width == 0U || height == 0U) {
        return false;
    }
#if SIZE_MAX < UINT32_MAX
    if (width > SIZE_MAX / HUD_BYTES_PER_PIXEL) return false;
#endif
    row_bytes = (size_t)width * HUD_BYTES_PER_PIXEL;
    if ((size_t)height > SIZE_MAX / row_bytes) return false;
    if (bytes != NULL) *bytes = row_bytes * (size_t)height;
    return true;
}

static void hud_layer_init(HudLayer *layer, uint32_t width, uint32_t height,
                           HudPixelFormat pixel_format)
{
    if (layer == NULL) return;
    memset(layer, 0, sizeof(*layer));
    layer->width = width;
    layer->height = height;
    layer->stride = width * HUD_BYTES_PER_PIXEL;
    layer->pixel_format = pixel_format;
}

static void hud_layer_clear(HudLayer *layer)
{
    uint32_t y;

    if (layer == NULL || layer->pixels == NULL) return;
    for (y = 0U; y < layer->height; ++y) {
        memset(layer->pixels + (size_t)y * layer->stride, 0U,
               (size_t)layer->width * HUD_BYTES_PER_PIXEL);
    }
    layer->region_count = 0U;
}

static bool hud_surface_valid(const HudRenderer *renderer,
                              const HudSurface *surface)
{
    size_t minimum_stride;

    if (renderer == NULL || surface == NULL || surface->pixels == NULL ||
        surface->width != renderer->width ||
        surface->height != renderer->height ||
        (surface->pixel_format != HUD_PIXEL_FORMAT_RGBA8888 &&
         surface->pixel_format != HUD_PIXEL_FORMAT_ARGB8888)) {
        return false;
    }
    if (!hud_renderer_size_valid(surface->width, surface->height, NULL)) {
        return false;
    }
    minimum_stride = (size_t)surface->width * HUD_BYTES_PER_PIXEL;
    if ((size_t)surface->stride_bytes < minimum_stride ||
        (size_t)surface->height > SIZE_MAX / surface->stride_bytes) {
        return false;
    }
    return true;
}

static void hud_layer_set_surface(HudLayer *layer, const HudSurface *surface)
{
    if (layer == NULL || surface == NULL) return;
    memset(layer, 0, sizeof(*layer));
    layer->width = surface->width;
    layer->height = surface->height;
    layer->stride = surface->stride_bytes;
    layer->pixels = surface->pixels;
    layer->pixel_format = surface->pixel_format;
}

static const HudLayer *hud_renderer_render_ui_layer(
    HudRenderer *renderer, HudLayer *layer, const VehicleState *state,
    bool recording, HudRenderTiming *timing)
{
    UiCanvas canvas;
    uint64_t start;
    uint64_t after_clear;
    uint64_t after_draw;

    if (renderer == NULL || renderer->ui_context == NULL || layer == NULL ||
        layer->pixels == NULL || layer->pixel_format != HUD_PIXEL_FORMAT_ARGB8888 ||
        layer->width > (uint32_t)INT_MAX ||
        layer->height > (uint32_t)INT_MAX ||
        layer->stride > (uint32_t)INT_MAX) {
        return NULL;
    }

    if (timing != NULL) memset(timing, 0, sizeof(*timing));
    start = hud_renderer_now_us();
    canvas.pixels = layer->pixels;
    canvas.width = (int)layer->width;
    canvas.height = (int)layer->height;
    canvas.stride = (int)layer->stride;
    after_clear = hud_renderer_now_us();
    if (hud_render(renderer->ui_context, &canvas, state, recording) < 0) {
        return NULL;
    }
    after_draw = hud_renderer_now_us();
    layer->region_count = 1U;
    layer->regions[0].x = 0U;
    layer->regions[0].y = 0U;
    layer->regions[0].width = layer->width;
    layer->regions[0].height = layer->height;
    if (timing != NULL) {
        timing->clear_time_us = after_clear - start;
        timing->draw_time_us = after_draw - after_clear;
        timing->total_time_us = after_draw - start;
    }
    return layer;
}

static const HudLayer *hud_renderer_render_none_layer(
    HudLayer *layer, HudRenderTiming *timing)
{
    uint64_t start;
    uint64_t finish;

    if (layer == NULL || layer->pixels == NULL) return NULL;
    if (timing != NULL) memset(timing, 0, sizeof(*timing));
    start = hud_renderer_now_us();
    hud_layer_clear(layer);
    finish = hud_renderer_now_us();
    if (timing != NULL) {
        timing->clear_time_us = finish - start;
        timing->total_time_us = finish - start;
    }
    return layer;
}

int hud_renderer_init_style(HudRenderer *renderer, uint32_t width,
                            uint32_t height, HudStyle style,
                            const char *font_path,
                            HudPixelFormat surface_format)
{
    const char *selected_font = font_path;
    HudPixelFormat layer_format;
    size_t layer_bytes;

    if (renderer == NULL || (style != HUD_STYLE_UI && style != HUD_STYLE_NONE) ||
        !hud_renderer_size_valid(width, height, &layer_bytes)) {
        return -1;
    }
    if (selected_font == NULL || selected_font[0] == '\0') {
        selected_font = HUD_DEFAULT_FONT_PATH;
    }
    layer_format = style == HUD_STYLE_UI ||
                   surface_format == HUD_PIXEL_FORMAT_ARGB8888 ?
                   HUD_PIXEL_FORMAT_ARGB8888 : HUD_PIXEL_FORMAT_RGBA8888;

    memset(renderer, 0, sizeof(*renderer));
    renderer->width = width;
    renderer->height = height;
    renderer->style = style;
    renderer->dynamic_surface_only = surface_format == HUD_PIXEL_FORMAT_ARGB8888;
    hud_layer_init(&renderer->static_layer, width, height, layer_format);
    hud_layer_init(&renderer->dynamic_layer, width, height, layer_format);
    renderer->static_layer.pixels = calloc(1U, layer_bytes);
    if (renderer->static_layer.pixels == NULL) goto fail;

    if (!renderer->dynamic_surface_only) {
        renderer->dynamic_layer.pixels = calloc(1U, layer_bytes);
        if (renderer->dynamic_layer.pixels == NULL) goto fail;
    }
    if (style == HUD_STYLE_UI && hud_init(&renderer->ui_context, selected_font) < 0) {
        goto fail;
    }
    renderer->initialized = true;
    return 0;

fail:
    hud_renderer_close(renderer);
    return -1;
}

const char *hud_style_name(HudStyle style)
{
    switch (style) {
    case HUD_STYLE_UI: return "ui";
    case HUD_STYLE_NONE: return "none";
    default: return "unknown";
    }
}

bool hud_style_parse(const char *value, HudStyle *style)
{
    if (value == NULL || style == NULL) return false;
    if (strcmp(value, "ui") == 0) {
        *style = HUD_STYLE_UI;
        return true;
    }
    if (strcmp(value, "none") == 0) {
        *style = HUD_STYLE_NONE;
        return true;
    }
    return false;
}

void hud_renderer_close(HudRenderer *renderer)
{
    if (renderer == NULL) return;
    hud_shutdown(renderer->ui_context);
    free(renderer->static_layer.pixels);
    free(renderer->dynamic_layer.pixels);
    memset(renderer, 0, sizeof(*renderer));
}

size_t hud_renderer_resident_memory(const HudRenderer *renderer)
{
    size_t layer_bytes = 0U;

    if (renderer == NULL || !renderer->initialized) return 0U;
    if (hud_renderer_size_valid(renderer->static_layer.width,
                                renderer->static_layer.height, &layer_bytes) &&
        renderer->static_layer.pixels != NULL) {
        if (renderer->dynamic_layer.pixels == NULL) return layer_bytes;
        if (layer_bytes > SIZE_MAX / 2U) return 0U;
        return layer_bytes * 2U;
    }
    return 0U;
}

const HudLayer *hud_renderer_static_layer(const HudRenderer *renderer)
{
    if (renderer == NULL || !renderer->initialized) return NULL;
    return &renderer->static_layer;
}

const HudLayer *hud_renderer_render_dynamic(HudRenderer *renderer,
                                             const VehicleState *state,
                                             bool recording)
{
    if (renderer == NULL || !renderer->initialized ||
        renderer->dynamic_surface_only) {
        return NULL;
    }
    if (renderer->style == HUD_STYLE_NONE) {
        return hud_renderer_render_none_layer(&renderer->dynamic_layer, NULL);
    }
    return hud_renderer_render_ui_layer(renderer, &renderer->dynamic_layer,
                                         state, recording, NULL);
}

const HudLayer *hud_renderer_render_dynamic_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing)
{
    if (renderer == NULL || !renderer->initialized ||
        !hud_surface_valid(renderer, surface)) {
        return NULL;
    }
    hud_layer_set_surface(&renderer->external_dynamic_layer, surface);
    if (renderer->style == HUD_STYLE_NONE) {
        return hud_renderer_render_none_layer(&renderer->external_dynamic_layer,
                                              timing);
    }
    if (surface->pixel_format != HUD_PIXEL_FORMAT_ARGB8888) return NULL;
    return hud_renderer_render_ui_layer(renderer,
                                        &renderer->external_dynamic_layer,
                                        state, recording, timing);
}

const HudLayer *hud_renderer_render_frame_to_surface(
    HudRenderer *renderer, const VehicleState *state, bool recording,
    const HudSurface *surface, HudRenderTiming *timing)
{
    return hud_renderer_render_dynamic_to_surface(renderer, state, recording,
                                                  surface, timing);
}
