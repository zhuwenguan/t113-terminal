#include "hud_renderer.h"
#include "video_compositor.h"
#include "video_frame.h"

#include <assert.h>
#include <stdint.h>
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

int main(void)
{
    test_render_at_size(HUD_BASE_WIDTH, HUD_BASE_HEIGHT);
    test_render_at_size(640U, 360U);
    return 0;
}
