#include "hud_renderer.h"
#include "video_compositor.h"
#include "video_frame.h"

#include <assert.h>
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
    HudSurface surface;
    HudRenderTiming timing;
    VehicleState state;
    const HudLayer *layer;
    uint8_t *pixels;
    size_t index;
    bool saw_red = false;

    memset(&renderer, 0, sizeof(renderer));
    memset(&surface, 0, sizeof(surface));
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
     * dynamic values. */
    assert(pixels[(size_t)(75U) * stride + (size_t)20U * 4U + 3U] != 0U);
    hud_renderer_close(&renderer);
    free(pixels);
}

int main(void)
{
    test_render_at_size(HUD_BASE_WIDTH, HUD_BASE_HEIGHT);
    test_render_at_size(640U, 360U);
    test_yuv_compose_with_stride_and_crop();
    test_render_to_external_argb_surface();
    return 0;
}
