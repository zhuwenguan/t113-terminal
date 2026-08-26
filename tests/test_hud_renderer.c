#include "hud_renderer.h"
#include "video_compositor.h"
#include "video_frame.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t count_opaque_pixels(const HudLayer *layer)
{
    size_t count = 0U;
    size_t pixels = (size_t)layer->width * layer->height;
    size_t index;

    for (index = 0U; index < pixels; ++index) {
        if (layer->pixels[index * VIDEO_FRAME_CHANNELS + 3U] != 0U) {
            count++;
        }
    }
    return count;
}

static void assert_regions_in_bounds(const HudLayer *layer)
{
    size_t index;

    for (index = 0U; index < layer->region_count; ++index) {
        const HudRegion *region = &layer->regions[index];
        assert(region->x <= layer->width);
        assert(region->y <= layer->height);
        assert(region->width <= layer->width - region->x);
        assert(region->height <= layer->height - region->y);
    }
}

static void assert_region(const HudLayer *layer, size_t index, uint32_t x,
                          uint32_t y, uint32_t width, uint32_t height)
{
    const HudRegion *region;

    assert(layer != NULL);
    assert(index < layer->region_count);
    region = &layer->regions[index];
    assert(region->x == x);
    assert(region->y == y);
    assert(region->width == width);
    assert(region->height == height);
}

static bool row_has_opaque_pixel(const HudLayer *layer, uint32_t row,
                                 uint32_t first_column, uint32_t last_column)
{
    uint32_t column;

    assert(layer != NULL);
    assert(row < layer->height);
    assert(first_column <= last_column && last_column < layer->width);
    for (column = first_column; column <= last_column; ++column) {
        if (layer->pixels[(size_t)row * layer->stride +
                          (size_t)column * VIDEO_FRAME_CHANNELS + 3U] != 0U) {
            return true;
        }
    }
    return false;
}

static void assert_g_circle_is_round(const HudLayer *layer)
{
    uint32_t min_x = layer->width;
    uint32_t min_y = layer->height;
    uint32_t max_x = 0U;
    uint32_t max_y = 0U;
    uint32_t x;
    uint32_t y;
    bool found = false;

    /* Exclude the G-panel outline, header and LONG G label.  The remaining
     * cyan pixels are the outer ring/crosshair and must have equal extents. */
    for (y = 168U; y <= 288U; ++y) {
        for (x = 640U; x <= 764U; ++x) {
            const uint8_t *pixel = layer->pixels +
                (size_t)y * layer->stride + (size_t)x * VIDEO_FRAME_CHANNELS;
            if (pixel[0] == 44U && pixel[1] == 210U && pixel[2] == 232U &&
                pixel[3] != 0U) {
                found = true;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }
    assert(found);
    assert(max_x - min_x == max_y - min_y);
}

static void test_render_at_size(uint32_t width, uint32_t height)
{
    HudRenderer renderer;
    VideoCompositor compositor;
    VideoFrame camera;
    VideoFrame output;
    VehicleState state;
    const HudLayer *dynamic;
    uint8_t *camera_pixel;

    memset(&renderer, 0, sizeof(renderer));
    memset(&camera, 0, sizeof(camera));
    memset(&output, 0, sizeof(output));
    assert(hud_renderer_init(&renderer, width, height) == 0);
    assert(video_frame_init(&camera, width, height) == 0);
    assert(video_frame_init(&output, width, height) == 0);
    assert(video_compositor_init(&compositor, width, height) == 0);
    memset(camera.pixels, 20, camera.capacity);
    memset(&state, 0, sizeof(state));
    state.valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                       VEHICLE_STATE_VALID_ACC_X_MPS2 |
                       VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                       VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT;
    state.speed_kmh = 72.0;
    state.acc_x = -30.0;
    state.acc_y = 30.0;
    state.motor_rpm = 15000.0;
    state.hv_soc_percent = 100.0;
    state.throttle_percent = 100.0;
    state.brake_percent = 0.0;
    state.lap_valid = true;
    state.lap_number = 2U;
    state.lap_time_s = 83.486;
    state.best_lap_time_s = 82.910;
    state.delta_time_s = 0.0;
    dynamic = hud_renderer_render_dynamic(&renderer, &state, true);
    assert(dynamic != NULL);
    assert(count_opaque_pixels(hud_renderer_static_layer(&renderer)) > 1000U);
    assert(count_opaque_pixels(dynamic) > 100U);
    assert_regions_in_bounds(hud_renderer_static_layer(&renderer));
    assert_regions_in_bounds(dynamic);
    assert(video_compositor_compose(&compositor, &camera,
                                    hud_renderer_static_layer(&renderer),
                                    dynamic, &output));
    camera_pixel = output.pixels + (size_t)(height / 2U) * output.stride +
                   (size_t)(width / 2U) * VIDEO_FRAME_CHANNELS;
    /* The center of the road remains visible; the G panel is at the edge. */
    assert(camera_pixel[0] == 20U && camera_pixel[1] == 20U &&
           camera_pixel[2] == 20U);

    memset(&state, 0, sizeof(state));
    dynamic = hud_renderer_render_dynamic(&renderer, &state, false);
    assert(dynamic != NULL);
    assert(count_opaque_pixels(dynamic) > 0U);

    state.valid_mask = VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT |
                       VEHICLE_STATE_VALID_ACC_X_MPS2 |
                       VEHICLE_STATE_VALID_ACC_Y_MPS2;
    state.motor_rpm = 0.0;
    state.hv_soc_percent = 0.0;
    state.throttle_percent = 0.0;
    state.brake_percent = 100.0;
    state.acc_x = -100.0;
    state.acc_y = 100.0;
    state.lap_valid = true;
    state.delta_time_s = 1.25;
    dynamic = hud_renderer_render_dynamic(&renderer, &state, false);
    assert(dynamic != NULL);
    assert(count_opaque_pixels(dynamic) > 0U);
    video_frame_close(&output);
    video_frame_close(&camera);
    hud_renderer_close(&renderer);
}

static void test_800x600_native_layout(void)
{
    HudRenderer renderer;
    const HudLayer *static_layer;
    const HudLayer *dynamic_layer;
    static const HudRegion expected_regions[] = {
        { 15U, 0U, 145U, 25U },
        { 15U, 40U, 158U, 221U },
        { 618U, 14U, 168U, 118U },
        { 618U, 148U, 168U, 168U },
        { 15U, 484U, 219U, 98U },
        { 288U, 484U, 225U, 98U },
        { 531U, 484U, 254U, 98U }
    };
    static const HudRegion expected_dynamic_regions[] = {
        { 15U, 0U, 140U, 25U },
        { 27U, 68U, 137U, 180U },
        { 690U, 32U, 90U, 60U },
        { 640U, 168U, 125U, 136U },
        { 27U, 492U, 193U, 83U },
        { 305U, 493U, 180U, 45U },
        { 542U, 498U, 242U, 45U }
    };
    VehicleState state;
    size_t index;

    memset(&renderer, 0, sizeof(renderer));
    memset(&state, 0, sizeof(state));
    assert(hud_renderer_init(&renderer, 800U, 600U) == 0);
    assert(fabs(renderer.scale_x - 1.0) < 0.000001);
    assert(fabs(renderer.scale_y - renderer.scale_x) < 0.000001);
    assert(renderer.offset_x == 0);
    assert(renderer.offset_y == 0);

    static_layer = hud_renderer_static_layer(&renderer);
    assert(static_layer != NULL);
    assert(static_layer->region_count == sizeof(expected_regions) /
           sizeof(expected_regions[0]));
    assert_regions_in_bounds(static_layer);
    for (index = 0U; index < sizeof(expected_regions) /
                          sizeof(expected_regions[0]); ++index) {
        assert_region(static_layer, index, expected_regions[index].x,
                      expected_regions[index].y, expected_regions[index].width,
                      expected_regions[index].height);
    }
    dynamic_layer = hud_renderer_render_dynamic(&renderer, &state, true);
    assert(dynamic_layer != NULL);
    assert(dynamic_layer->region_count == sizeof(expected_dynamic_regions) /
           sizeof(expected_dynamic_regions[0]));
    assert_regions_in_bounds(dynamic_layer);
    for (index = 0U; index < sizeof(expected_dynamic_regions) /
                          sizeof(expected_dynamic_regions[0]); ++index) {
        assert_region(dynamic_layer, index, expected_dynamic_regions[index].x,
                      expected_dynamic_regions[index].y,
                      expected_dynamic_regions[index].width,
                      expected_dynamic_regions[index].height);
    }
    assert(row_has_opaque_pixel(dynamic_layer, 13U, 0U, 799U));
    assert(row_has_opaque_pixel(static_layer, 18U, 0U, 799U));
    assert(row_has_opaque_pixel(static_layer, 484U, 0U, 799U));
    assert(row_has_opaque_pixel(static_layer, 581U, 0U, 799U));
    assert_g_circle_is_round(static_layer);
    hud_renderer_close(&renderer);
}

static void test_yuv_compose_with_stride_and_crop(void)
{
    VideoCompositor compositor;
    VideoYuvFrame camera;
    HudLayer static_layer;
    HudLayer dynamic_layer;
    uint8_t y_plane[32];
    uint8_t u_plane[16];
    uint8_t v_plane[16];
    uint8_t *overlay;

    memset(&camera, 0, sizeof(camera));
    memset(&static_layer, 0, sizeof(static_layer));
    memset(&dynamic_layer, 0, sizeof(dynamic_layer));
    memset(y_plane, 16, sizeof(y_plane));
    memset(u_plane, 128, sizeof(u_plane));
    memset(v_plane, 128, sizeof(v_plane));
    assert(video_compositor_init(&compositor, 4U, 2U) == 0);
    camera.format = VIDEO_YUV_FORMAT_YUV420P;
    camera.width = 4U;
    camera.height = 2U;
    camera.stride[0] = 8U;
    camera.stride[1] = 4U;
    camera.stride[2] = 4U;
    camera.left_offset = 1U;
    camera.top_offset = 1U;
    camera.plane[0] = y_plane;
    camera.plane[1] = u_plane;
    camera.plane[2] = v_plane;
    static_layer.width = dynamic_layer.width = 4U;
    static_layer.height = dynamic_layer.height = 2U;
    static_layer.stride = dynamic_layer.stride = 4U * VIDEO_FRAME_CHANNELS;
    static_layer.pixels = calloc(1U, (size_t)static_layer.stride * 2U);
    dynamic_layer.pixels = calloc(1U, (size_t)dynamic_layer.stride * 2U);
    assert(static_layer.pixels != NULL && dynamic_layer.pixels != NULL);
    static_layer.region_count = 1U;
    static_layer.regions[0].width = 4U;
    static_layer.regions[0].height = 2U;
    overlay = static_layer.pixels;
    overlay[0] = 255U;
    overlay[3] = 255U;
    dynamic_layer.region_count = 1U;
    dynamic_layer.regions[0].width = 4U;
    dynamic_layer.regions[0].height = 2U;
    assert(video_compositor_compose_yuv(&compositor, &camera,
                                        &static_layer, &dynamic_layer));
    /* The crop starts at (1, 1), and the padded luma stride must be honored. */
    assert(y_plane[1U * 8U + 1U] > 60U);
    assert(y_plane[0] == 16U);
    /* The red overlay must update U and V in the same 2x2 chroma sample. */
    assert(u_plane[0] != 128U && v_plane[0] != 128U);
    free(dynamic_layer.pixels);
    free(static_layer.pixels);
}

static void test_render_to_external_argb_surface(void)
{
    const uint32_t width = 640U;
    const uint32_t height = 360U;
    const uint32_t stride = width * VIDEO_FRAME_CHANNELS + 16U;
    HudRenderer renderer;
    HudRenderer reference_renderer;
    HudSurface surface;
    HudSurface reference_surface;
    HudRenderTiming timing;
    VehicleState state;
    const HudLayer *layer;
    uint8_t *pixels;
    uint8_t *reference_pixels;
    size_t index;
    uint32_t row;
    bool saw_red = false;

    memset(&renderer, 0, sizeof(renderer));
    memset(&reference_renderer, 0, sizeof(reference_renderer));
    memset(&surface, 0, sizeof(surface));
    memset(&reference_surface, 0, sizeof(reference_surface));
    memset(&timing, 0, sizeof(timing));
    memset(&state, 0, sizeof(state));
    pixels = calloc(1U, (size_t)stride * height);
    assert(pixels != NULL);
    surface.pixels = pixels;
    surface.width = width;
    surface.height = height;
    surface.stride_bytes = stride;
    surface.pixel_format = HUD_PIXEL_FORMAT_ARGB8888;
    state.valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                       VEHICLE_STATE_VALID_MOTOR_RPM;
    state.speed_kmh = 88.0;
    state.motor_rpm = 6400.0;
    state.lap_valid = true;
    state.lap_number = 7U;
    assert(hud_renderer_init_external(&renderer, width, height) == 0);
    assert(hud_renderer_render_dynamic(&renderer, &state, true) == NULL);
    layer = hud_renderer_render_dynamic_to_surface(&renderer, &state, true,
                                                   &surface, &timing);
    assert(layer != NULL);
    assert(layer->pixels == pixels);
    assert(layer->width == width && layer->height == height);
    assert(layer->stride == stride);
    assert(layer->pixel_format == HUD_PIXEL_FORMAT_ARGB8888);
    assert(timing.total_time_us >= timing.clear_time_us);
    assert(timing.total_time_us >= timing.draw_time_us);
    for (index = 0U; index + 3U < (size_t)stride * height; index += 4U) {
        if (pixels[index + 2U] > pixels[index] && pixels[index + 3U] != 0U) {
            saw_red = true;
            break;
        }
    }
    assert(saw_red);
    memset(pixels, 0, (size_t)stride * height);
    layer = hud_renderer_render_frame_to_surface(&renderer, &state, true,
                                                 &surface, &timing);
    assert(layer != NULL);
    assert(layer->region_count == 1U);
    assert(layer->regions[0].width == width);
    assert(layer->regions[0].height == height);
    /* The complete direct surface includes the static left panel as well as
     * dynamic values.  At 640x360 the 800x600 canvas fits at scale .6 and is
     * centered horizontally (x offset 80). */
    assert(pixels[(size_t)(24U) * stride + (size_t)89U * 4U + 3U] != 0U);

    /* Reusing the ION-like surface must restore every dynamic region before
     * drawing the next snapshot, including removal of REC and shorter text. */
    memset(&state, 0, sizeof(state));
    layer = hud_renderer_render_frame_to_surface(&renderer, &state, false,
                                                 &surface, &timing);
    assert(layer != NULL);
    reference_pixels = calloc(1U, (size_t)stride * height);
    assert(reference_pixels != NULL);
    reference_surface = surface;
    reference_surface.pixels = reference_pixels;
    assert(hud_renderer_init_external(&reference_renderer, width, height) == 0);
    assert(hud_renderer_render_frame_to_surface(&reference_renderer, &state,
                                                false, &reference_surface,
                                                NULL) != NULL);
    for (row = 0U; row < height; ++row) {
        assert(memcmp(pixels + (size_t)row * stride,
                      reference_pixels + (size_t)row * stride,
                      (size_t)width * VIDEO_FRAME_CHANNELS) == 0);
    }
    hud_renderer_close(&reference_renderer);
    free(reference_pixels);
    hud_renderer_close(&renderer);
    free(pixels);
}

static void test_render_to_external_800x600_surface(void)
{
    const uint32_t width = 800U;
    const uint32_t height = 600U;
    const uint32_t stride = width * VIDEO_FRAME_CHANNELS + 16U;
    HudRenderer renderer;
    HudRenderer reference_renderer;
    HudSurface surface;
    HudSurface reference_surface;
    VehicleState state;
    const HudLayer *layer;
    uint8_t *pixels;
    uint8_t *reference_pixels;
    const uint8_t *left_panel_pixel;
    const uint8_t *recording_pixel;
    uint32_t row;

    memset(&renderer, 0, sizeof(renderer));
    memset(&reference_renderer, 0, sizeof(reference_renderer));
    memset(&surface, 0, sizeof(surface));
    memset(&reference_surface, 0, sizeof(reference_surface));
    memset(&state, 0, sizeof(state));
    pixels = calloc(1U, (size_t)stride * height);
    assert(pixels != NULL);
    surface.pixels = pixels;
    surface.width = width;
    surface.height = height;
    surface.stride_bytes = stride;
    surface.pixel_format = HUD_PIXEL_FORMAT_ARGB8888;
    state.valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                       VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT;
    state.speed_kmh = 123.0;
    state.motor_rpm = 15000.0;
    state.throttle_percent = 100.0;
    state.brake_percent = 100.0;
    state.lap_valid = true;
    state.lap_number = 99U;

    assert(hud_renderer_init_external(&renderer, width, height) == 0);
    layer = hud_renderer_render_frame_to_surface(&renderer, &state, true,
                                                 &surface, NULL);
    assert(layer != NULL);
    assert(layer->region_count == 1U);
    assert(layer->regions[0].width == width);
    assert(layer->regions[0].height == height);
    left_panel_pixel = pixels + (size_t)40U * stride + (size_t)15U * 4U;
    assert(left_panel_pixel[3] != 0U);
    recording_pixel = pixels + (size_t)18U * stride + (size_t)110U * 4U;
    assert(recording_pixel[3] != 0U);
    assert(pixels[3] == 0U);
    assert(pixels[(size_t)(height - 1U) * stride + 3U] == 0U);

    memset(&state, 0, sizeof(state));
    layer = hud_renderer_render_frame_to_surface(&renderer, &state, false,
                                                 &surface, NULL);
    assert(layer != NULL);
    assert(recording_pixel[3] == 0U);

    reference_pixels = calloc(1U, (size_t)stride * height);
    assert(reference_pixels != NULL);
    reference_surface = surface;
    reference_surface.pixels = reference_pixels;
    assert(hud_renderer_init_external(&reference_renderer, width, height) == 0);
    assert(hud_renderer_render_frame_to_surface(&reference_renderer, &state,
                                                false, &reference_surface,
                                                NULL) != NULL);
    for (row = 0U; row < height; ++row) {
        assert(memcmp(pixels + (size_t)row * stride,
                      reference_pixels + (size_t)row * stride,
                      (size_t)width * VIDEO_FRAME_CHANNELS) == 0);
    }

    hud_renderer_close(&reference_renderer);
    free(reference_pixels);
    hud_renderer_close(&renderer);
    free(pixels);
}

int main(void)
{
    test_render_at_size(HUD_BASE_WIDTH, HUD_BASE_HEIGHT);
    test_render_at_size(640U, 360U);
    test_render_at_size(800U, 600U);
    test_800x600_native_layout();
    test_yuv_compose_with_stride_and_crop();
    test_render_to_external_argb_surface();
    test_render_to_external_800x600_surface();
    return 0;
}
