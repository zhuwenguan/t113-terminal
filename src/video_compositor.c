#include "video_compositor.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

int video_compositor_init(VideoCompositor *compositor, uint32_t width,
                          uint32_t height)
{
    if (compositor == NULL || width == 0U || height == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(compositor, 0, sizeof(*compositor));
    compositor->width = width;
    compositor->height = height;
    compositor->initialized = true;
    return 0;
}

static void video_compositor_blend_layer(VideoFrame *output,
                                         const HudLayer *layer)
{
    size_t region_index;

    if (output == NULL || layer == NULL || layer->pixels == NULL) {
        return;
    }
    for (region_index = 0U; region_index < layer->region_count;
         ++region_index) {
        const HudRegion *region = &layer->regions[region_index];
        uint32_t row_start = region->y < output->height ? region->y : output->height;
        uint32_t row_end = region->y + region->height < output->height ?
                           region->y + region->height : output->height;
        uint32_t row;

        for (row = row_start; row < row_end; ++row) {
            uint8_t *destination = output->pixels + (size_t)row * output->stride +
                                   (size_t)region->x * VIDEO_FRAME_CHANNELS;
            const uint8_t *source = layer->pixels + (size_t)row * layer->stride +
                                    (size_t)region->x * VIDEO_FRAME_CHANNELS;
            uint32_t column;

            for (column = 0U; column < region->width &&
                            region->x + column + 8U <= output->width;
                 column += 8U) {
#if defined(__ARM_NEON)
                uint8x8x4_t destination_pixels;
                uint8x8x4_t overlay_pixels;
                uint8x8_t alpha;
                uint8x8_t inverse;
                uint16x8_t red;
                uint16x8_t green;
                uint16x8_t blue;

                destination_pixels = vld4_u8(destination +
                                              (size_t)column * VIDEO_FRAME_CHANNELS);
                overlay_pixels = vld4_u8(source +
                                         (size_t)column * VIDEO_FRAME_CHANNELS);
                alpha = overlay_pixels.val[3];
                inverse = vsub_u8(vdup_n_u8(255U), alpha);
                red = vaddq_u16(vmull_u8(overlay_pixels.val[0], alpha),
                                vmull_u8(destination_pixels.val[0], inverse));
                green = vaddq_u16(vmull_u8(overlay_pixels.val[1], alpha),
                                  vmull_u8(destination_pixels.val[1], inverse));
                blue = vaddq_u16(vmull_u8(overlay_pixels.val[2], alpha),
                                 vmull_u8(destination_pixels.val[2], inverse));
                red = vaddq_u16(red, vdupq_n_u16(128U));
                green = vaddq_u16(green, vdupq_n_u16(128U));
                blue = vaddq_u16(blue, vdupq_n_u16(128U));
                destination_pixels.val[0] = vshrn_n_u16(red, 8);
                destination_pixels.val[1] = vshrn_n_u16(green, 8);
                destination_pixels.val[2] = vshrn_n_u16(blue, 8);
                destination_pixels.val[3] = vdup_n_u8(255U);
                vst4_u8(destination + (size_t)column * VIDEO_FRAME_CHANNELS,
                        destination_pixels);
#else
                /* The host test build retains a portable implementation. */
                uint32_t pixel_index;
                for (pixel_index = 0U; pixel_index < 8U; ++pixel_index) {
                    uint8_t *pixel = destination +
                        (size_t)(column + pixel_index) * VIDEO_FRAME_CHANNELS;
                    const uint8_t *overlay = source +
                        (size_t)(column + pixel_index) * VIDEO_FRAME_CHANNELS;
                    uint32_t alpha = overlay[3];
                    uint32_t inverse = 255U - alpha;
                    pixel[0] = (uint8_t)((overlay[0] * alpha +
                                          pixel[0] * inverse + 128U) >> 8);
                    pixel[1] = (uint8_t)((overlay[1] * alpha +
                                          pixel[1] * inverse + 128U) >> 8);
                    pixel[2] = (uint8_t)((overlay[2] * alpha +
                                          pixel[2] * inverse + 128U) >> 8);
                    pixel[3] = 255U;
                }
#endif
            }
            for (; column < region->width && region->x + column < output->width;
                 ++column) {
                uint8_t *pixel = destination + (size_t)column * VIDEO_FRAME_CHANNELS;
                const uint8_t *overlay = source + (size_t)column * VIDEO_FRAME_CHANNELS;
                uint32_t alpha = overlay[3];
                uint32_t inverse;

                if (alpha == 0U) {
                    continue;
                }
                if (alpha >= 255U) {
                    pixel[0] = overlay[0];
                    pixel[1] = overlay[1];
                    pixel[2] = overlay[2];
                    pixel[3] = 255U;
                    continue;
                }
                inverse = 255U - alpha;
                pixel[0] = (uint8_t)((overlay[0] * alpha +
                                      pixel[0] * inverse + 128U) >> 8);
                pixel[1] = (uint8_t)((overlay[1] * alpha +
                                      pixel[1] * inverse + 128U) >> 8);
                pixel[2] = (uint8_t)((overlay[2] * alpha +
                                      pixel[2] * inverse + 128U) >> 8);
                pixel[3] = 255U;
            }
        }
    }
}

static uint8_t video_compositor_blend_u8(uint8_t background,
                                         uint8_t foreground, uint32_t alpha)
{
    return (uint8_t)((foreground * alpha + background * (255U - alpha) +
                      127U) / 255U);
}

static uint8_t video_compositor_clamp_u8(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 255) {
        return 255U;
    }
    return (uint8_t)value;
}

static void video_compositor_rgb_to_yuv(uint8_t red, uint8_t green,
                                        uint8_t blue, uint8_t *y,
                                        uint8_t *u, uint8_t *v)
{
    *y = video_compositor_clamp_u8(((66 * (int)red + 129 * (int)green +
                                     25 * (int)blue + 128) >> 8) + 16);
    *u = video_compositor_clamp_u8(((-38 * (int)red - 74 * (int)green +
                                     112 * (int)blue + 128) >> 8) + 128);
    *v = video_compositor_clamp_u8(((112 * (int)red - 94 * (int)green -
                                     18 * (int)blue + 128) >> 8) + 128);
}

static bool video_compositor_yuv_pixel(VideoYuvFrame *frame, uint32_t x,
                                       uint32_t y, const uint8_t *overlay)
{
    uint32_t source_x;
    uint32_t source_y;
    uint8_t foreground_y;
    uint8_t foreground_u;
    uint8_t foreground_v;
    uint32_t alpha;
    uint8_t *y_plane;
    uint8_t *u_plane;
    uint8_t *v_plane;

    if (frame == NULL || overlay == NULL || frame->plane[0] == NULL ||
        x >= frame->width || y >= frame->height ||
        x > UINT32_MAX - frame->left_offset ||
        y > UINT32_MAX - frame->top_offset) {
        return false;
    }
    source_x = x + frame->left_offset;
    source_y = y + frame->top_offset;
    alpha = overlay[3];
    if (alpha == 0U) {
        return true;
    }
    video_compositor_rgb_to_yuv(overlay[0], overlay[1], overlay[2],
                                &foreground_y, &foreground_u, &foreground_v);
    y_plane = frame->plane[0] + (size_t)source_y * frame->stride[0] + source_x;
    *y_plane = video_compositor_blend_u8(*y_plane, foreground_y, alpha);
    if (frame->format == VIDEO_YUV_FORMAT_YUV420P ||
        frame->format == VIDEO_YUV_FORMAT_YV12) {
        if (frame->plane[1] == NULL || frame->plane[2] == NULL) {
            return false;
        }
        u_plane = frame->plane[1] + (size_t)(source_y / 2U) * frame->stride[1] +
                  source_x / 2U;
        v_plane = frame->plane[2] + (size_t)(source_y / 2U) * frame->stride[2] +
                  source_x / 2U;
        if (frame->format == VIDEO_YUV_FORMAT_YV12) {
            uint8_t *swap = u_plane;
            u_plane = v_plane;
            v_plane = swap;
        }
        *u_plane = video_compositor_blend_u8(*u_plane, foreground_u, alpha);
        *v_plane = video_compositor_blend_u8(*v_plane, foreground_v, alpha);
        return true;
    }
    if (frame->format == VIDEO_YUV_FORMAT_NV12 ||
        frame->format == VIDEO_YUV_FORMAT_NV21) {
        uint8_t *chroma;

        if (frame->plane[1] == NULL) {
            return false;
        }
        chroma = frame->plane[1] + (size_t)(source_y / 2U) * frame->stride[1] +
                 (source_x / 2U) * 2U;
        if (frame->format == VIDEO_YUV_FORMAT_NV12) {
            chroma[0] = video_compositor_blend_u8(chroma[0], foreground_u,
                                                  alpha);
            chroma[1] = video_compositor_blend_u8(chroma[1], foreground_v,
                                                  alpha);
        } else {
            chroma[0] = video_compositor_blend_u8(chroma[0], foreground_v,
                                                  alpha);
            chroma[1] = video_compositor_blend_u8(chroma[1], foreground_u,
                                                  alpha);
        }
        return true;
    }
    return false;
}

static bool video_compositor_blend_yuv_layer(VideoYuvFrame *output,
                                             const HudLayer *layer)
{
    size_t region_index;

    if (output == NULL || layer == NULL || layer->pixels == NULL) {
        return false;
    }
    for (region_index = 0U; region_index < layer->region_count;
         ++region_index) {
        const HudRegion *region = &layer->regions[region_index];
        uint32_t row_end = region->height > output->height -
                           (region->y < output->height ? region->y : output->height) ?
                           output->height : region->y + region->height;
        uint32_t row;

        if (region->x >= output->width || region->y >= output->height) {
            continue;
        }
        if (region->x > UINT32_MAX - region->width) {
            return false;
        }
        for (row = region->y; row < row_end; ++row) {
            uint32_t column_end = region->width > output->width - region->x ?
                                  output->width : region->x + region->width;
            uint32_t column;

            for (column = region->x; column < column_end; ++column) {
                const uint8_t *overlay = layer->pixels +
                    (size_t)row * layer->stride +
                    (size_t)column * VIDEO_FRAME_CHANNELS;
                if (!video_compositor_yuv_pixel(output, column, row, overlay)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool video_compositor_compose(const VideoCompositor *compositor,
                              const VideoFrame *camera,
                              const HudLayer *static_layer,
                              const HudLayer *dynamic_layer,
                              VideoFrame *output)
{
    uint32_t row;

    if (compositor == NULL || !compositor->initialized || camera == NULL ||
        output == NULL || camera->pixels == NULL || output->pixels == NULL ||
        camera->width != compositor->width || camera->height != compositor->height ||
        output->width != compositor->width || output->height != compositor->height ||
        static_layer == NULL || dynamic_layer == NULL ||
        static_layer->width != compositor->width ||
        dynamic_layer->width != compositor->width) {
        return false;
    }
    if (output != camera) {
        for (row = 0U; row < output->height; ++row) {
            memcpy(output->pixels + (size_t)row * output->stride,
                   camera->pixels + (size_t)row * camera->stride,
                   (size_t)output->width * VIDEO_FRAME_CHANNELS);
        }
    }
    video_compositor_blend_layer(output, static_layer);
    video_compositor_blend_layer(output, dynamic_layer);
    output->timestamp_us = camera->timestamp_us;
    return true;
}

bool video_compositor_compose_yuv(const VideoCompositor *compositor,
                                  VideoYuvFrame *camera,
                                  const HudLayer *static_layer,
                                  const HudLayer *dynamic_layer)
{
    if (compositor == NULL || !compositor->initialized || camera == NULL ||
        camera->plane[0] == NULL || camera->width != compositor->width ||
        camera->height != compositor->height || static_layer == NULL ||
        dynamic_layer == NULL || static_layer->width != compositor->width ||
        dynamic_layer->width != compositor->width ||
        (camera->format != VIDEO_YUV_FORMAT_YUV420P &&
         camera->format != VIDEO_YUV_FORMAT_YV12 &&
         camera->format != VIDEO_YUV_FORMAT_NV12 &&
         camera->format != VIDEO_YUV_FORMAT_NV21)) {
        return false;
    }
    return video_compositor_blend_yuv_layer(camera, static_layer) &&
           video_compositor_blend_yuv_layer(camera, dynamic_layer);
}
