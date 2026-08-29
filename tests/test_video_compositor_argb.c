#include "video_compositor.h"
#include "video_frame.h"

#include "test_check.h"
#include <stdint.h>
#include <string.h>

static void test_layer_format(HudPixelFormat format)
{
    VideoCompositor compositor;
    VideoFrame camera;
    VideoFrame output;
    HudLayer static_layer;
    HudLayer dynamic_layer;
    uint8_t overlay[8] = { 0 };

    memset(&compositor, 0, sizeof(compositor));
    memset(&camera, 0, sizeof(camera));
    memset(&output, 0, sizeof(output));
    memset(&static_layer, 0, sizeof(static_layer));
    memset(&dynamic_layer, 0, sizeof(dynamic_layer));
    TEST_CHECK(video_compositor_init(&compositor, 2U, 1U) == 0);
    TEST_CHECK(video_frame_init(&camera, 2U, 1U) == 0);
    TEST_CHECK(video_frame_init(&output, 2U, 1U) == 0);

    camera.pixels[0] = 10U;
    camera.pixels[1] = 20U;
    camera.pixels[2] = 30U;
    camera.pixels[3] = 255U;
    camera.pixels[4] = 40U;
    camera.pixels[5] = 50U;
    camera.pixels[6] = 60U;
    camera.pixels[7] = 255U;
    if (format == HUD_PIXEL_FORMAT_ARGB8888) {
        overlay[0] = 0U;
        overlay[1] = 0U;
        overlay[2] = 255U;
    } else {
        overlay[0] = 255U;
        overlay[1] = 0U;
        overlay[2] = 0U;
    }
    overlay[3] = 255U;

    static_layer.width = dynamic_layer.width = 2U;
    static_layer.height = dynamic_layer.height = 1U;
    static_layer.stride = dynamic_layer.stride = 8U;
    static_layer.pixels = overlay;
    static_layer.pixel_format = format;
    dynamic_layer.pixels = overlay;
    dynamic_layer.pixel_format = format;
    dynamic_layer.region_count = 1U;
    dynamic_layer.regions[0] = (HudRegion){ 0U, 0U, 1U, 1U };

    TEST_CHECK(video_compositor_compose(&compositor, &camera, &static_layer,
                                    &dynamic_layer, &output));
    TEST_CHECK(output.pixels[0] == 255U);
    TEST_CHECK(output.pixels[1] == 0U);
    TEST_CHECK(output.pixels[2] == 0U);
    TEST_CHECK(output.pixels[4] == 40U);
    TEST_CHECK(output.pixels[5] == 50U);
    TEST_CHECK(output.pixels[6] == 60U);

    video_frame_close(&output);
    video_frame_close(&camera);
    video_compositor_close(&compositor);
}

int main(void)
{
    test_layer_format(HUD_PIXEL_FORMAT_RGBA8888);
    test_layer_format(HUD_PIXEL_FORMAT_ARGB8888);
    return 0;
}
