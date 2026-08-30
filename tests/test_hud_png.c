#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "hud_renderer.h"
#include "hud_blend.h"
#include "tiny_png.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void assert_layer_bounds(const HudLayer *layer)
{
    size_t index;

    assert(layer != NULL);
    for (index = 0U; index < layer->region_count; ++index) {
        assert(layer->regions[index].x <= layer->width);
        assert(layer->regions[index].y <= layer->height);
        assert(layer->regions[index].width <=
               layer->width - layer->regions[index].x);
        assert(layer->regions[index].height <=
               layer->height - layer->regions[index].y);
    }
}

static void test_channel_conversion(const char *asset_dir,
                                    HudPixelFormat format)
{
    HudAssets assets;
    HudAssetId id;

    memset(&assets, 0, sizeof(assets));
    assert(hud_assets_init(&assets, asset_dir, format, false) == 0);
    for (id = HUD_ASSET_STATIC_FRAME; id < HUD_ASSET_COUNT; ++id) {
        TinyPngImage decoded;
        char path[HUD_ASSET_PATH_MAX];
        char error_message[160];
        const HudImage *image = hud_assets_get(&assets, id);
        size_t pixel;

        if (image == NULL) continue;
        assert(snprintf(path, sizeof(path), "%s/%s", asset_dir,
                        hud_asset_name(id)) < (int)sizeof(path));
        memset(&decoded, 0, sizeof(decoded));
        memset(error_message, 0, sizeof(error_message));
        assert(tiny_png_load_rgba(path, &decoded, error_message,
                                  sizeof(error_message)) == 0);
        assert(decoded.width == image->width);
        assert(decoded.height == image->height);
        for (pixel = 0U; pixel < image->bytes / 4U; ++pixel) {
            const uint8_t *rgba = decoded.rgba + pixel * 4U;
            const uint8_t *stored = image->pixels + pixel * 4U;

            if (format == HUD_PIXEL_FORMAT_ARGB8888) {
                assert(stored[0] == rgba[2]);
                assert(stored[1] == rgba[1]);
                assert(stored[2] == rgba[0]);
            } else {
                assert(stored[0] == rgba[0]);
                assert(stored[1] == rgba[1]);
                assert(stored[2] == rgba[2]);
            }
            assert(stored[3] == rgba[3]);
        }
        tiny_png_image_close(&decoded);
    }
    hud_assets_close(&assets);
}

static void test_alpha_blend(HudPixelFormat format)
{
    uint8_t pixel[4] = { 0U, 0U, 0U, 255U };
    HudLayer layer = { 1U, 1U, 4U, pixel, format,
                       { { 0U, 0U, 0U, 0U } }, 0U };

    if (format == HUD_PIXEL_FORMAT_ARGB8888) pixel[0] = 255U;
    else pixel[2] = 255U;
    hud_blend_pixel(&layer, 0, 0, 255U, 0U, 0U, 128U);
    if (format == HUD_PIXEL_FORMAT_ARGB8888) {
        assert(pixel[0] >= 126U && pixel[0] <= 129U);
        assert(pixel[2] >= 126U && pixel[2] <= 129U);
    } else {
        assert(pixel[0] >= 126U && pixel[0] <= 129U);
        assert(pixel[2] >= 126U && pixel[2] <= 129U);
    }
    assert(pixel[1] == 0U && pixel[3] == 255U);
}

static size_t count_alpha_rect(const HudLayer *layer, uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height)
{
    size_t count = 0U;
    uint32_t row;

    assert(layer != NULL);
    assert(x <= layer->width && width <= layer->width - x);
    assert(y <= layer->height && height <= layer->height - y);
    for (row = y; row < y + height; ++row) {
        uint32_t column;

        for (column = x; column < x + width; ++column) {
            if (layer->pixels[(size_t)row * layer->stride +
                              (size_t)column * 4U + 3U] != 0U) {
                count++;
            }
        }
    }
    return count;
}

static void wait_for_hud_text_refresh(void)
{
    struct timespec delay = { 0, 120000000L };

    assert(nanosleep(&delay, NULL) == 0);
}

static VehicleState make_state(double rpm, double throttle, double brake,
                               double lateral_g, double longitudinal_g)
{
    VehicleState state;

    memset(&state, 0, sizeof(state));
    state.valid_mask = VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_LV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT |
                       VEHICLE_STATE_VALID_ACC_X_MPS2 |
                       VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                       VEHICLE_STATE_VALID_DRIVE_STATE;
    state.motor_rpm = rpm;
    state.hv_voltage_v = 356.0;
    state.lv_voltage_v = 13.6;
    state.hv_soc_percent = 78.0;
    state.throttle_percent = throttle;
    state.brake_percent = brake;
    state.acc_x = longitudinal_g * 9.80665;
    state.acc_y = lateral_g * 9.80665;
    state.drive_state = VEHICLE_DRIVE_STATE_READY;
    state.lap_valid = true;
    state.lap_number = 2U;
    state.lap_time_s = 83.486;
    state.best_lap_time_s = 82.910;
    state.delta_time_s = -0.42;
    state.gps_valid = true;
    state.gps_fix_type = 4U;
    state.can_valid = true;
    return state;
}

static void test_png_renderer(const char *asset_dir,
                              HudPixelFormat format)
{
    HudRenderer renderer;
    HudRenderer reference_renderer;
    HudSurface surface;
    HudSurface reference_surface;
    const HudLayer *dynamic;
    VehicleState states[4];
    uint8_t *pixels;
    uint8_t *reference_pixels;
    size_t index;
    uint32_t row;

    memset(&renderer, 0, sizeof(renderer));
    memset(&reference_renderer, 0, sizeof(reference_renderer));
    memset(&surface, 0, sizeof(surface));
    memset(&reference_surface, 0, sizeof(reference_surface));
    pixels = calloc(1U, HUD_BASE_WIDTH * HUD_BASE_HEIGHT * 4U);
    assert(pixels != NULL);
    assert(hud_renderer_init_style(&renderer, HUD_BASE_WIDTH, HUD_BASE_HEIGHT,
                                   HUD_STYLE_PNG, asset_dir,
                                   HUD_DEFAULT_FONT_PATH, format) == 0);
    assert(renderer.style == HUD_STYLE_PNG);
    assert(hud_renderer_png_assets_ready(&renderer));
    assert(hud_renderer_asset_memory(&renderer) > 0U);
    test_channel_conversion(asset_dir, format);
    test_alpha_blend(format);
    assert(renderer.text_atlas.initialized);
    assert(hud_renderer_static_layer(&renderer)->region_count > 0U);
    surface.pixels = pixels;
    surface.width = HUD_BASE_WIDTH;
    surface.height = HUD_BASE_HEIGHT;
    surface.stride_bytes = HUD_BASE_WIDTH * 4U;
    surface.pixel_format = format;
    states[0] = make_state(1200.0, 0.0, 0.0, 0.0, 0.0);
    states[1] = make_state(9600.0, 100.0, 0.0, 0.8, -0.8);
    states[2] = make_state(5000.0, 25.0, 85.0, -0.8, 0.8);
    memset(&states[3], 0, sizeof(states[3]));
    if (format == HUD_PIXEL_FORMAT_RGBA8888) {
        dynamic = hud_renderer_render_dynamic(&renderer, &states[0], false);
        assert(dynamic != NULL);
        assert(count_alpha_rect(dynamic, 684U, 444U, 20U, 122U) == 0U);
        dynamic = hud_renderer_render_dynamic(&renderer, &states[1], true);
        assert(dynamic != NULL);
        assert(count_alpha_rect(dynamic, 684U, 444U, 20U, 122U) > 0U);
        assert(count_alpha_rect(dynamic, 718U, 444U, 20U, 122U) == 0U);
        dynamic = hud_renderer_render_dynamic(&renderer, &states[2], false);
        assert(dynamic != NULL);
        assert(count_alpha_rect(dynamic, 684U, 444U, 20U, 122U) > 0U);
        assert(count_alpha_rect(dynamic, 718U, 444U, 20U, 122U) > 0U);
    }
    for (index = 0U; index < sizeof(states) / sizeof(states[0]); ++index) {
        const HudLayer *layer = hud_renderer_render_frame_to_surface(
            &renderer, &states[index], index == 1U, &surface, NULL);

        assert(layer != NULL);
        assert(layer->region_count == 1U);
        assert_layer_bounds(layer);
        assert_layer_bounds(hud_renderer_static_layer(&renderer));
        assert(layer->pixels == pixels);
    }

    /* Reusing the native external surface must clear the RPM text below the
     * gear frame as well as the gear value itself.  Render a long value first
     * and a short value second, then compare against a fresh renderer. */
    if (format == HUD_PIXEL_FORMAT_ARGB8888) {
        VehicleState long_rpm = make_state(9999.0, 0.0, 0.0, 0.0, 0.0);
        VehicleState short_rpm = make_state(1.0, 0.0, 0.0, 0.0, 0.0);

        wait_for_hud_text_refresh();
        assert(hud_renderer_render_frame_to_surface(&renderer, &long_rpm,
                                                    true, &surface, NULL) != NULL);
        wait_for_hud_text_refresh();
        assert(hud_renderer_render_frame_to_surface(&renderer, &short_rpm,
                                                    true, &surface, NULL) != NULL);
        reference_pixels = calloc(1U, HUD_BASE_WIDTH * HUD_BASE_HEIGHT * 4U);
        assert(reference_pixels != NULL);
        reference_surface = surface;
        reference_surface.pixels = reference_pixels;
        assert(hud_renderer_init_style(&reference_renderer,
                                       HUD_BASE_WIDTH, HUD_BASE_HEIGHT,
                                       HUD_STYLE_PNG, asset_dir,
                                       HUD_DEFAULT_FONT_PATH, format) == 0);
        assert(hud_renderer_render_frame_to_surface(&reference_renderer,
                                                    &short_rpm, true,
                                                    &reference_surface,
                                                    NULL) != NULL);
        for (row = 0U; row < HUD_BASE_HEIGHT; ++row) {
            assert(memcmp(pixels + (size_t)row * surface.stride_bytes,
                          reference_pixels + (size_t)row *
                              reference_surface.stride_bytes,
                          HUD_BASE_WIDTH * 4U) == 0);
        }
        hud_renderer_close(&reference_renderer);
        free(reference_pixels);
    }
    hud_renderer_close(&renderer);
    hud_renderer_close(&renderer);
    free(pixels);
}

static void test_missing_assets(const char *asset_dir)
{
    HudRenderer renderer;
    HudSurface surface;
    uint8_t *pixels = calloc(800U * 600U, 4U);

    memset(&renderer, 0, sizeof(renderer));
    memset(&surface, 0, sizeof(surface));
    assert(pixels != NULL);
    memset(pixels, 0, 800U * 600U * 4U);
    assert(hud_renderer_init_style(&renderer, 800U, 600U, HUD_STYLE_PNG,
                                   asset_dir, HUD_DEFAULT_FONT_PATH,
                                   HUD_PIXEL_FORMAT_RGBA8888) == 0);
    assert(!hud_renderer_png_assets_ready(&renderer));
    surface.pixels = pixels;
    surface.width = 800U;
    surface.height = 600U;
    surface.stride_bytes = 800U * 4U;
    surface.pixel_format = HUD_PIXEL_FORMAT_RGBA8888;
    assert(hud_renderer_render_frame_to_surface(&renderer, NULL, false,
                                                &surface, NULL) != NULL);
    hud_renderer_close(&renderer);
    free(pixels);
}

static void test_single_missing_asset(const char *asset_dir)
{
    char source_dir[PATH_MAX];
    char temporary_dir[] = "/tmp/t113-hud-assets-single-XXXXXX";
    HudRenderer renderer;
    HudSurface surface;
    uint8_t *pixels;
    HudAssetId id;

    if (asset_dir[0] == '/') {
        assert(snprintf(source_dir, sizeof(source_dir), "%s", asset_dir) <
               (int)sizeof(source_dir));
    } else {
        char working_dir[PATH_MAX];

        assert(getcwd(working_dir, sizeof(working_dir)) != NULL);
        assert(snprintf(source_dir, sizeof(source_dir), "%s/%s",
                        working_dir, asset_dir) < (int)sizeof(source_dir));
    }
    assert(mkdtemp(temporary_dir) != NULL);
    for (id = HUD_ASSET_STATIC_FRAME; id < HUD_ASSET_COUNT; ++id) {
        char source_path[PATH_MAX];
        char target_path[PATH_MAX];

        if (id == HUD_ASSET_G_METER_DOT) continue;
        assert(snprintf(source_path, sizeof(source_path), "%s/%s",
                        source_dir, hud_asset_name(id)) <
               (int)sizeof(source_path));
        assert(snprintf(target_path, sizeof(target_path), "%s/%s",
                        temporary_dir, hud_asset_name(id)) <
               (int)sizeof(target_path));
        assert(symlink(source_path, target_path) == 0);
    }

    memset(&renderer, 0, sizeof(renderer));
    memset(&surface, 0, sizeof(surface));
    pixels = calloc(1U, 800U * 600U * 4U);
    assert(pixels != NULL);
    assert(hud_renderer_init_style(&renderer, 800U, 600U, HUD_STYLE_PNG,
                                   temporary_dir, HUD_DEFAULT_FONT_PATH,
                                   HUD_PIXEL_FORMAT_RGBA8888) == 0);
    assert(!hud_renderer_png_assets_ready(&renderer));
    surface.pixels = pixels;
    surface.width = 800U;
    surface.height = 600U;
    surface.stride_bytes = 800U * 4U;
    surface.pixel_format = HUD_PIXEL_FORMAT_RGBA8888;
    assert(hud_renderer_render_frame_to_surface(&renderer, NULL, false,
                                                &surface, NULL) != NULL);
    hud_renderer_close(&renderer);
    hud_renderer_close(&renderer);
    free(pixels);

    for (id = HUD_ASSET_STATIC_FRAME; id < HUD_ASSET_COUNT; ++id) {
        char target_path[PATH_MAX];

        if (id == HUD_ASSET_G_METER_DOT) continue;
        assert(snprintf(target_path, sizeof(target_path), "%s/%s",
                        temporary_dir, hud_asset_name(id)) <
               (int)sizeof(target_path));
        assert(unlink(target_path) == 0);
    }
    assert(rmdir(temporary_dir) == 0);
}

int main(int argc, char **argv)
{
    const char *asset_dir = argc > 1 ? argv[1] : "assets/hud";

    test_png_renderer(asset_dir, HUD_PIXEL_FORMAT_RGBA8888);
    test_png_renderer(asset_dir, HUD_PIXEL_FORMAT_ARGB8888);
    test_missing_assets("/tmp/t113-hud-assets-does-not-exist");
    test_single_missing_asset(asset_dir);
    return 0;
}
