#include "hud_blend.h"

#include <stddef.h>

static inline unsigned int hud_div255(unsigned int value)
{
    unsigned int rounded = value + 128U;

    return (rounded + (rounded >> 8U)) >> 8U;
}

static inline void hud_read_pixel(const HudLayer *layer, int x, int y,
                                  uint8_t *red, uint8_t *green,
                                  uint8_t *blue, uint8_t *alpha)
{
    const uint8_t *pixel = layer->pixels + (size_t)y * layer->stride +
                           (size_t)x * 4U;

    if (layer->pixel_format == HUD_PIXEL_FORMAT_ARGB8888) {
        *blue = pixel[0];
        *green = pixel[1];
        *red = pixel[2];
    } else {
        *red = pixel[0];
        *green = pixel[1];
        *blue = pixel[2];
    }
    *alpha = pixel[3];
}

static inline void hud_write_pixel(HudLayer *layer, int x, int y,
                                   uint8_t red, uint8_t green, uint8_t blue,
                                   uint8_t alpha)
{
    uint8_t *pixel = layer->pixels + (size_t)y * layer->stride +
                     (size_t)x * 4U;

    if (layer->pixel_format == HUD_PIXEL_FORMAT_ARGB8888) {
        pixel[0] = blue;
        pixel[1] = green;
        pixel[2] = red;
    } else {
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
    }
    pixel[3] = alpha;
}

static inline void hud_blend_pixel_unchecked(HudLayer *destination, int x,
                                             int y, uint8_t red,
                                             uint8_t green, uint8_t blue,
                                             uint8_t alpha)
{
    uint8_t old_red;
    uint8_t old_green;
    uint8_t old_blue;
    uint8_t old_alpha;
    uint8_t output_alpha;

    if (alpha == 0U) return;
    if (alpha == 255U) {
        hud_write_pixel(destination, x, y, red, green, blue, 255U);
        return;
    }
    hud_read_pixel(destination, x, y, &old_red, &old_green, &old_blue,
                   &old_alpha);
    if (old_alpha == 0U) {
        hud_write_pixel(destination, x, y, red, green, blue, alpha);
        return;
    }
    if (old_alpha == 255U) {
        unsigned int inverse = 255U - alpha;

        hud_write_pixel(destination, x, y,
                        (uint8_t)hud_div255((unsigned int)red * alpha +
                                            (unsigned int)old_red * inverse),
                        (uint8_t)hud_div255((unsigned int)green * alpha +
                                            (unsigned int)old_green * inverse),
                        (uint8_t)hud_div255((unsigned int)blue * alpha +
                                            (unsigned int)old_blue * inverse),
                        255U);
        return;
    }
    output_alpha = (uint8_t)(alpha + hud_div255(
        (unsigned int)old_alpha * (255U - alpha)));
    if (output_alpha == 0U) {
        hud_write_pixel(destination, x, y, 0U, 0U, 0U, 0U);
        return;
    }
    /* Keep the destination straight-alpha too.  The destination may already
     * contain a translucent panel, so normalize by output_alpha instead of
     * treating its RGB as opaque. */
    {
        unsigned int inverse = 255U - alpha;
        unsigned int red_premul = (unsigned int)red * alpha +
            hud_div255((unsigned int)old_red * old_alpha * inverse);
        unsigned int green_premul = (unsigned int)green * alpha +
            hud_div255((unsigned int)old_green * old_alpha * inverse);
        unsigned int blue_premul = (unsigned int)blue * alpha +
            hud_div255((unsigned int)old_blue * old_alpha * inverse);

        hud_write_pixel(destination, x, y,
                        (uint8_t)((red_premul + output_alpha / 2U) /
                                  output_alpha),
                        (uint8_t)((green_premul + output_alpha / 2U) /
                                  output_alpha),
                        (uint8_t)((blue_premul + output_alpha / 2U) /
                                  output_alpha), output_alpha);
    }
}

void hud_blend_pixel(HudLayer *destination, int x, int y, uint8_t red,
                     uint8_t green, uint8_t blue, uint8_t alpha)
{
    if (destination == NULL || destination->pixels == NULL || x < 0 || y < 0 ||
        (uint32_t)x >= destination->width ||
        (uint32_t)y >= destination->height || alpha == 0U) {
        return;
    }
    hud_blend_pixel_unchecked(destination, x, y, red, green, blue, alpha);
}

bool hud_blend_coverage(HudLayer *destination, int destination_x,
                        int destination_y, uint32_t width, uint32_t height,
                        const uint8_t *coverage, uint32_t coverage_stride,
                        uint8_t red, uint8_t green, uint8_t blue,
                        uint8_t alpha)
{
    uint32_t source_y_begin;
    uint32_t source_y_end;
    uint32_t source_x_begin;
    uint32_t source_x_end;
    uint32_t source_y;

    if (destination == NULL || destination->pixels == NULL || coverage == NULL ||
        width == 0U || height == 0U || coverage_stride < width) {
        return false;
    }
    source_x_begin = destination_x < 0 ? (uint32_t)(-destination_x) : 0U;
    source_y_begin = destination_y < 0 ? (uint32_t)(-destination_y) : 0U;
    source_x_end = width;
    source_y_end = height;
    if (destination_x >= 0 && (uint64_t)destination_x + width >
        destination->width) {
        source_x_end = destination->width - (uint32_t)destination_x;
    }
    if (destination_y >= 0 && (uint64_t)destination_y + height >
        destination->height) {
        source_y_end = destination->height - (uint32_t)destination_y;
    }
    if (source_x_begin >= source_x_end || source_y_begin >= source_y_end) {
        return true;
    }
    for (source_y = source_y_begin; source_y < source_y_end; ++source_y) {
        uint32_t source_x;
        int output_y = destination_y + (int)source_y;

        for (source_x = source_x_begin; source_x < source_x_end; ++source_x) {
            uint8_t coverage_value = coverage[(size_t)source_y *
                                               coverage_stride + source_x];
            uint8_t pixel_alpha = (uint8_t)hud_div255(
                (unsigned int)alpha * coverage_value);

            if (pixel_alpha != 0U) {
                hud_blend_pixel_unchecked(destination,
                                           destination_x + (int)source_x,
                                           output_y, red, green, blue,
                                           pixel_alpha);
            }
        }
    }
    return true;
}

static inline uint8_t hud_image_component(const HudImage *source, size_t offset,
                                          bool red)
{
    if (source->pixel_format == HUD_PIXEL_FORMAT_ARGB8888) {
        if (red) return source->pixels[offset + 2U];
        return source->pixels[offset + 1U];
    }
    return source->pixels[offset + (red ? 0U : 1U)];
}

static inline uint8_t hud_image_blue(const HudImage *source, size_t offset)
{
    return source->pixels[offset +
        (source->pixel_format == HUD_PIXEL_FORMAT_ARGB8888 ? 0U : 2U)];
}

bool hud_blend_image_region(const HudImage *source, HudLayer *destination,
                            uint32_t source_x, uint32_t source_y,
                            uint32_t source_width, uint32_t source_height,
                            int destination_x, int destination_y,
                            uint32_t destination_width,
                            uint32_t destination_height)
{
    uint32_t output_y;
    uint32_t output_y_begin = 0U;
    uint32_t output_y_end = destination_height;
    uint32_t output_x_begin = 0U;
    uint32_t output_x_end = destination_width;
    bool same_size;

    if (source == NULL || destination == NULL || source->pixels == NULL ||
        destination->pixels == NULL || !source->loaded || source_width == 0U ||
        source_height == 0U || destination_width == 0U ||
        destination_height == 0U || source_x >= source->width ||
        source_y >= source->height || source_width > source->width - source_x ||
        source_height > source->height - source_y) {
        return false;
    }
    same_size = source_width == destination_width &&
                source_height == destination_height;
    /* Skip transparent margins using the decode-time alpha bounds.  This is
     * important for the large arc canvases: their visible pixels occupy only
     * a narrow horizontal strip, while the source rectangle remains aligned
     * with the supplied artwork. */
    if (source->alpha_max > 0U) {
        uint32_t active_min;
        uint32_t active_max;

        active_min = source->alpha_y_min > source_y ? source->alpha_y_min :
                     source_y;
        active_max = source->alpha_y_max < source_y + source_height - 1U ?
                     source->alpha_y_max : source_y + source_height - 1U;
        if (active_min > active_max) return true;
        output_y_begin = (uint32_t)(((uint64_t)(active_min - source_y) *
                                     destination_height + source_height - 1U) /
                                    source_height);
        output_y_end = (uint32_t)(((uint64_t)(active_max - source_y + 1U) *
                                   destination_height + source_height - 1U) /
                                  source_height);
        if (output_y_begin > destination_height) output_y_begin = destination_height;
        if (output_y_end > destination_height) output_y_end = destination_height;

        active_min = source->alpha_x_min > source_x ? source->alpha_x_min :
                     source_x;
        active_max = source->alpha_x_max < source_x + source_width - 1U ?
                     source->alpha_x_max : source_x + source_width - 1U;
        if (active_min > active_max) return true;
        output_x_begin = (uint32_t)(((uint64_t)(active_min - source_x) *
                                     destination_width + source_width - 1U) /
                                    source_width);
        output_x_end = (uint32_t)(((uint64_t)(active_max - source_x + 1U) *
                                   destination_width + source_width - 1U) /
                                  source_width);
        if (output_x_begin > destination_width) output_x_begin = destination_width;
        if (output_x_end > destination_width) output_x_end = destination_width;
    }
    for (output_y = output_y_begin; output_y < output_y_end; ++output_y) {
        int y = destination_y + (int)output_y;
        uint32_t source_row = same_size ? source_y + output_y : source_y +
            (uint32_t)(((uint64_t)output_y * source_height) /
                       destination_height);
        uint32_t output_x;

        if (y < 0 || (uint32_t)y >= destination->height) {
            continue;
        }
        for (output_x = output_x_begin; output_x < output_x_end; ++output_x) {
            int x = destination_x + (int)output_x;
            uint32_t source_column = same_size ? source_x + output_x :
                source_x + (uint32_t)(((uint64_t)output_x * source_width) /
                                      destination_width);
            size_t source_offset;
            uint8_t alpha;

            if (x < 0 || (uint32_t)x >= destination->width) {
                continue;
            }
            source_offset = (size_t)source_row * source->stride +
                            (size_t)source_column * 4U;
            alpha = source->pixels[source_offset + 3U];
            if (alpha != 0U) {
                uint8_t red = hud_image_component(source, source_offset, true);
                uint8_t green = hud_image_component(source, source_offset, false);
                uint8_t blue = hud_image_blue(source, source_offset);

                if (source->premultiplied) {
                    red = (uint8_t)(((unsigned int)red * 255U + alpha / 2U) /
                                    alpha);
                    green = (uint8_t)(((unsigned int)green * 255U + alpha / 2U) /
                                      alpha);
                    blue = (uint8_t)(((unsigned int)blue * 255U + alpha / 2U) /
                                     alpha);
                }
                if (alpha == 255U) {
                    hud_write_pixel(destination, x, y, red, green, blue, 255U);
                } else {
                    hud_blend_pixel(destination, x, y, red, green, blue, alpha);
                }
            }
        }
    }
    return true;
}

bool hud_blend_image(const HudImage *source, HudLayer *destination,
                     int destination_x, int destination_y,
                     uint32_t destination_width, uint32_t destination_height)
{
    if (source == NULL) {
        return false;
    }
    return hud_blend_image_region(source, destination, 0U, 0U, source->width,
                                  source->height, destination_x, destination_y,
                                  destination_width, destination_height);
}
