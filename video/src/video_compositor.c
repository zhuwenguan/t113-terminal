#include "video_compositor.h"

#include "g2d_driver.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define VIDEO_G2D_DEVICE "/dev/g2d"
#define VIDEO_ION_LIBRARY "libawion.so"

static uint64_t video_compositor_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

const char *video_compositor_mode_name(VideoCompositorMode mode)
{
    switch (mode) {
    case VIDEO_COMPOSITOR_MODE_G2D: return "g2d";
    case VIDEO_COMPOSITOR_MODE_CPU: return "cpu";
    case VIDEO_COMPOSITOR_MODE_AUTO:
    default: return "auto";
    }
}

bool video_compositor_parse_mode(const char *value, VideoCompositorMode *mode)
{
    if (value == NULL || mode == NULL) {
        return false;
    }
    if (strcmp(value, "auto") == 0) {
        *mode = VIDEO_COMPOSITOR_MODE_AUTO;
    } else if (strcmp(value, "g2d") == 0) {
        *mode = VIDEO_COMPOSITOR_MODE_G2D;
    } else if (strcmp(value, "cpu") == 0) {
        *mode = VIDEO_COMPOSITOR_MODE_CPU;
    } else {
        return false;
    }
    return true;
}

static bool video_compositor_load_symbol(void *library, const char *name,
                                         void *target, size_t target_size)
{
    void *symbol;

    if (library == NULL || target == NULL || target_size != sizeof(symbol)) {
        return false;
    }
    dlerror();
    symbol = dlsym(library, name);
    if (symbol == NULL || dlerror() != NULL) {
        return false;
    }
    memcpy(target, &symbol, sizeof(symbol));
    return true;
}

static void video_ion_buffer_reset(VideoIonBuffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;
}

static bool video_ion_buffer_alloc(VideoCompositor *compositor,
                                   VideoIonBuffer *buffer, size_t size)
{
    video_ion_buffer_reset(buffer);
    if (size == 0U || size > INT_MAX) {
        return false;
    }
    buffer->memory = compositor->ion_alloc((int)size);
    if (buffer->memory == NULL) {
        return false;
    }
    buffer->fd = compositor->ion_get_fd(buffer->memory);
    buffer->size = size;
    if (buffer->fd < 0) {
        compositor->ion_free(buffer->memory);
        video_ion_buffer_reset(buffer);
        return false;
    }
    return true;
}

static void video_ion_buffer_free(VideoCompositor *compositor,
                                  VideoIonBuffer *buffer)
{
    if (buffer->memory != NULL && compositor->ion_free != NULL) {
        compositor->ion_free(buffer->memory);
    }
    video_ion_buffer_reset(buffer);
}

static void video_ion_buffer_sync(VideoCompositor *compositor,
                                  const VideoIonBuffer *buffer)
{
    if (compositor->ion_flush != NULL && buffer->memory != NULL &&
        buffer->size <= INT_MAX) {
        compositor->ion_flush(buffer->memory, (int)buffer->size);
    }
}

static void video_g2d_set_image(g2d_image_enh *image, int fd,
                                g2d_fmt_enh format, uint32_t width,
                                uint32_t height)
{
    memset(image, 0, sizeof(*image));
    image->bbuff = 1;
    image->format = format;
    image->width = width;
    image->height = height;
    image->clip_rect.w = width;
    image->clip_rect.h = height;
    image->gamut = G2D_BT601;
    image->alpha = 0xffU;
    image->mode = G2D_PIXEL_ALPHA;
    image->fd = fd;
    image->color_range = COLOR_RANGE_0_255;
}

static void video_compositor_disable_g2d(VideoCompositor *compositor)
{
    if (compositor == NULL || !compositor->g2d_healthy) {
        return;
    }
    compositor->g2d_healthy = false;
    compositor->fatal_failures++;
    if (compositor->g2d_fd >= 0) {
        close(compositor->g2d_fd);
        compositor->g2d_fd = -1;
    }
    if (compositor->requested_mode == VIDEO_COMPOSITOR_MODE_AUTO) {
        compositor->active_mode = VIDEO_COMPOSITOR_MODE_CPU;
    }
}

static int video_compositor_init_g2d(VideoCompositor *compositor)
{
    size_t pixels;
    size_t yuv_size;
    size_t argb_size;

    if ((uint64_t)compositor->width * compositor->height > SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    pixels = (size_t)compositor->width * compositor->height;
    yuv_size = pixels + pixels / 2U;
    argb_size = pixels * VIDEO_FRAME_CHANNELS;
    compositor->ion_library = dlopen(VIDEO_ION_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    if (compositor->ion_library == NULL ||
        !video_compositor_load_symbol(compositor->ion_library,
            "sunxi_ion_alloc_open", &compositor->ion_open,
            sizeof(compositor->ion_open)) ||
        !video_compositor_load_symbol(compositor->ion_library,
            "sunxi_ion_alloc_close", &compositor->ion_close,
            sizeof(compositor->ion_close)) ||
        !video_compositor_load_symbol(compositor->ion_library,
            "sunxi_ion_alloc_palloc", &compositor->ion_alloc,
            sizeof(compositor->ion_alloc)) ||
        !video_compositor_load_symbol(compositor->ion_library,
            "sunxi_ion_alloc_pfree", &compositor->ion_free,
            sizeof(compositor->ion_free)) ||
        !video_compositor_load_symbol(compositor->ion_library,
            "sunxi_ion_alloc_get_bufferFd", &compositor->ion_get_fd,
            sizeof(compositor->ion_get_fd)) ||
        !video_compositor_load_symbol(compositor->ion_library,
            "sunxi_ion_alloc_flush_cache", &compositor->ion_flush,
            sizeof(compositor->ion_flush)) || compositor->ion_open() != 0) {
        errno = ENOSYS;
        return -1;
    }
    compositor->ion_opened = true;
    if (!video_ion_buffer_alloc(compositor, &compositor->yuv_input, yuv_size) ||
        !video_ion_buffer_alloc(compositor, &compositor->argb_camera, argb_size) ||
        !video_ion_buffer_alloc(compositor, &compositor->argb_hud, argb_size) ||
        !video_ion_buffer_alloc(compositor, &compositor->argb_final, argb_size)) {
        errno = ENOMEM;
        return -1;
    }
    memset(compositor->argb_hud.memory, 0, argb_size);
    memset(compositor->argb_final.memory, 0, argb_size);
    video_ion_buffer_sync(compositor, &compositor->argb_hud);
    video_ion_buffer_sync(compositor, &compositor->argb_final);
    compositor->g2d_fd = open(VIDEO_G2D_DEVICE, O_RDWR | O_CLOEXEC);
    if (compositor->g2d_fd < 0) {
        return -1;
    }
    compositor->argb_stride = compositor->width * VIDEO_FRAME_CHANNELS;
    compositor->hud_surface.pixels = compositor->argb_hud.memory;
    compositor->hud_surface.width = compositor->width;
    compositor->hud_surface.height = compositor->height;
    compositor->hud_surface.stride_bytes = compositor->argb_stride;
    compositor->hud_surface.pixel_format = HUD_PIXEL_FORMAT_ARGB8888;
    compositor->final_frame.width = compositor->width;
    compositor->final_frame.height = compositor->height;
    compositor->final_frame.stride = compositor->argb_stride;
    compositor->final_frame.capacity = argb_size;
    compositor->final_frame.pixels = compositor->argb_final.memory;
    compositor->g2d_healthy = true;
    compositor->active_mode = VIDEO_COMPOSITOR_MODE_G2D;
    return 0;
}

int video_compositor_init_mode(VideoCompositor *compositor, uint32_t width,
                               uint32_t height, VideoCompositorMode mode)
{
    if (compositor == NULL || width == 0U || height == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(compositor, 0, sizeof(*compositor));
    compositor->g2d_fd = -1;
    video_ion_buffer_reset(&compositor->yuv_input);
    video_ion_buffer_reset(&compositor->argb_camera);
    video_ion_buffer_reset(&compositor->argb_hud);
    video_ion_buffer_reset(&compositor->argb_final);
    compositor->width = width;
    compositor->height = height;
    compositor->requested_mode = mode;
    compositor->active_mode = VIDEO_COMPOSITOR_MODE_CPU;
    compositor->initialized = true;
    if (mode != VIDEO_COMPOSITOR_MODE_CPU &&
        video_compositor_init_g2d(compositor) < 0) {
        int saved_errno = errno;

        video_compositor_close(compositor);
        if (mode == VIDEO_COMPOSITOR_MODE_G2D) {
            errno = saved_errno;
            return -1;
        }
        memset(compositor, 0, sizeof(*compositor));
        compositor->g2d_fd = -1;
        compositor->width = width;
        compositor->height = height;
        compositor->requested_mode = mode;
        compositor->active_mode = VIDEO_COMPOSITOR_MODE_CPU;
        compositor->initialized = true;
    }
    return 0;
}

int video_compositor_init(VideoCompositor *compositor, uint32_t width,
                          uint32_t height)
{
    return video_compositor_init_mode(compositor, width, height,
                                      VIDEO_COMPOSITOR_MODE_CPU);
}

void video_compositor_close(VideoCompositor *compositor)
{
    if (compositor == NULL) {
        return;
    }
    if (compositor->g2d_fd >= 0) {
        close(compositor->g2d_fd);
    }
    video_ion_buffer_free(compositor, &compositor->argb_final);
    video_ion_buffer_free(compositor, &compositor->argb_hud);
    video_ion_buffer_free(compositor, &compositor->argb_camera);
    video_ion_buffer_free(compositor, &compositor->yuv_input);
    if (compositor->ion_opened && compositor->ion_close != NULL) {
        compositor->ion_close();
    }
    if (compositor->ion_library != NULL) {
        dlclose(compositor->ion_library);
    }
    memset(compositor, 0, sizeof(*compositor));
    compositor->g2d_fd = -1;
}

VideoCompositorMode video_compositor_active_mode(
    const VideoCompositor *compositor)
{
    return compositor != NULL ? compositor->active_mode :
           VIDEO_COMPOSITOR_MODE_CPU;
}

bool video_compositor_is_g2d_healthy(const VideoCompositor *compositor)
{
    return compositor != NULL && compositor->initialized &&
           compositor->active_mode == VIDEO_COMPOSITOR_MODE_G2D &&
           compositor->g2d_healthy;
}

const HudSurface *video_compositor_hud_surface(
    const VideoCompositor *compositor)
{
    return video_compositor_is_g2d_healthy(compositor) ?
           &compositor->hud_surface : NULL;
}

bool video_compositor_sync_hud(VideoCompositor *compositor,
                               VideoCompositorTiming *timing)
{
    uint64_t start;

    if (!video_compositor_is_g2d_healthy(compositor)) {
        return false;
    }
    start = video_compositor_now_us();
    video_ion_buffer_sync(compositor, &compositor->argb_hud);
    if (timing != NULL) {
        timing->hud_cache_sync_time_us += video_compositor_now_us() - start;
    }
    return true;
}

static bool video_compositor_copy_yuv420p(VideoCompositor *compositor,
                                          const VideoYuvFrame *camera)
{
    uint8_t *destination_y;
    uint8_t *destination_u;
    uint8_t *destination_v;
    const uint8_t *source_u;
    const uint8_t *source_v;
    uint32_t row;
    uint32_t chroma_width;
    uint32_t chroma_height;

    if (camera == NULL || camera->plane[0] == NULL || camera->plane[1] == NULL ||
        camera->plane[2] == NULL || camera->width != compositor->width ||
        camera->height != compositor->height || (camera->width & 1U) != 0U ||
        (camera->height & 1U) != 0U ||
        (camera->format != VIDEO_YUV_FORMAT_YUV420P &&
         camera->format != VIDEO_YUV_FORMAT_YV12) ||
        camera->stride[0] < camera->width ||
        camera->stride[1] < camera->width / 2U ||
        camera->stride[2] < camera->width / 2U) {
        return false;
    }
    destination_y = compositor->yuv_input.memory;
    chroma_width = camera->width / 2U;
    chroma_height = camera->height / 2U;
    /* T113 G2D_FORMAT_YUV420_PLANAR consumes Y + V + U (YV12 order). */
    destination_v = destination_y + (size_t)camera->width * camera->height;
    destination_u = destination_v + (size_t)chroma_width * chroma_height;
    source_u = camera->format == VIDEO_YUV_FORMAT_YV12 ? camera->plane[2] :
               camera->plane[1];
    source_v = camera->format == VIDEO_YUV_FORMAT_YV12 ? camera->plane[1] :
               camera->plane[2];
    for (row = 0U; row < camera->height; ++row) {
        memcpy(destination_y + (size_t)row * camera->width,
               camera->plane[0] +
                   (size_t)(row + camera->top_offset) * camera->stride[0] +
                   camera->left_offset,
               camera->width);
    }
    for (row = 0U; row < chroma_height; ++row) {
        size_t source_offset =
            (size_t)(row + camera->top_offset / 2U) * camera->stride[1] +
            camera->left_offset / 2U;
        memcpy(destination_u + (size_t)row * chroma_width,
               source_u + source_offset, chroma_width);
        memcpy(destination_v + (size_t)row * chroma_width,
               source_v + source_offset, chroma_width);
    }
    return true;
}

const VideoFrame *video_compositor_compose_g2d(
    VideoCompositor *compositor, const VideoYuvFrame *camera,
    uint64_t timestamp_us, VideoCompositorTiming *timing)
{
    g2d_blt_h blt;
    g2d_bld blend;
    uint64_t stage;

    if (!video_compositor_is_g2d_healthy(compositor)) {
        return NULL;
    }
    stage = video_compositor_now_us();
    if (!video_compositor_copy_yuv420p(compositor, camera)) {
        return NULL;
    }
    if (timing != NULL) {
        timing->yuv_copy_time_us += video_compositor_now_us() - stage;
    }
    stage = video_compositor_now_us();
    video_ion_buffer_sync(compositor, &compositor->yuv_input);
    if (timing != NULL) {
        timing->yuv_cache_sync_time_us += video_compositor_now_us() - stage;
    }
    memset(&blt, 0, sizeof(blt));
    blt.flag_h = G2D_BLT_NONE_H;
    video_g2d_set_image(&blt.src_image_h, compositor->yuv_input.fd,
                        G2D_FORMAT_YUV420_PLANAR, compositor->width,
                        compositor->height);
    video_g2d_set_image(&blt.dst_image_h, compositor->argb_camera.fd,
                        G2D_FORMAT_ARGB8888, compositor->width,
                        compositor->height);
    stage = video_compositor_now_us();
    if (ioctl(compositor->g2d_fd, G2D_CMD_BITBLT_H, &blt) < 0) {
        video_compositor_disable_g2d(compositor);
        return NULL;
    }
    video_ion_buffer_sync(compositor, &compositor->argb_camera);
    if (timing != NULL) {
        timing->yuv_to_argb_time_us += video_compositor_now_us() - stage;
    }
    memset(&blend, 0, sizeof(blend));
    blend.bld_cmd = G2D_BLD_SRCOVER;
    video_g2d_set_image(&blend.src_image[0], compositor->argb_camera.fd,
                        G2D_FORMAT_ARGB8888, compositor->width,
                        compositor->height);
    video_g2d_set_image(&blend.src_image[1], compositor->argb_hud.fd,
                        G2D_FORMAT_ARGB8888, compositor->width,
                        compositor->height);
    video_g2d_set_image(&blend.dst_image, compositor->argb_final.fd,
                        G2D_FORMAT_ARGB8888, compositor->width,
                        compositor->height);
    stage = video_compositor_now_us();
    if (ioctl(compositor->g2d_fd, G2D_CMD_BLD_H, &blend) < 0) {
        video_compositor_disable_g2d(compositor);
        return NULL;
    }
    video_ion_buffer_sync(compositor, &compositor->argb_final);
    if (timing != NULL) {
        timing->blend_time_us += video_compositor_now_us() - stage;
    }
    compositor->final_frame.timestamp_us = timestamp_us;
    return &compositor->final_frame;
}

static void video_compositor_blend_layer(VideoFrame *output,
                                         const HudLayer *layer)
{
    size_t region_index;
    bool argb_layer;

    if (output == NULL || layer == NULL || layer->pixels == NULL) {
        return;
    }
    argb_layer = layer->pixel_format == HUD_PIXEL_FORMAT_ARGB8888;
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
                red = vaddq_u16(vmull_u8(argb_layer ? overlay_pixels.val[2] :
                                                   overlay_pixels.val[0], alpha),
                                vmull_u8(destination_pixels.val[0], inverse));
                green = vaddq_u16(vmull_u8(overlay_pixels.val[1], alpha),
                                  vmull_u8(destination_pixels.val[1], inverse));
                blue = vaddq_u16(vmull_u8(argb_layer ? overlay_pixels.val[0] :
                                                   overlay_pixels.val[2], alpha),
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
                    uint32_t red = argb_layer ? overlay[2] : overlay[0];
                    uint32_t blue = argb_layer ? overlay[0] : overlay[2];
                    pixel[0] = (uint8_t)((red * alpha +
                                          pixel[0] * inverse + 128U) >> 8);
                    pixel[1] = (uint8_t)((overlay[1] * alpha +
                                          pixel[1] * inverse + 128U) >> 8);
                    pixel[2] = (uint8_t)((blue * alpha +
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
                    pixel[0] = argb_layer ? overlay[2] : overlay[0];
                    pixel[1] = overlay[1];
                    pixel[2] = argb_layer ? overlay[0] : overlay[2];
                    pixel[3] = 255U;
                    continue;
                }
                inverse = 255U - alpha;
                pixel[0] = (uint8_t)(((argb_layer ? overlay[2] : overlay[0]) *
                                      alpha +
                                      pixel[0] * inverse + 128U) >> 8);
                pixel[1] = (uint8_t)((overlay[1] * alpha +
                                      pixel[1] * inverse + 128U) >> 8);
                pixel[2] = (uint8_t)(((argb_layer ? overlay[0] : overlay[2]) *
                                      alpha +
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
                                       uint32_t y, const uint8_t *overlay,
                                       bool argb_layer)
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
    video_compositor_rgb_to_yuv(argb_layer ? overlay[2] : overlay[0],
                                overlay[1], argb_layer ? overlay[0] : overlay[2],
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
    bool argb_layer;

    if (output == NULL || layer == NULL || layer->pixels == NULL) {
        return false;
    }
    argb_layer = layer->pixel_format == HUD_PIXEL_FORMAT_ARGB8888;
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
                if (!video_compositor_yuv_pixel(output, column, row, overlay,
                                               argb_layer)) {
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
