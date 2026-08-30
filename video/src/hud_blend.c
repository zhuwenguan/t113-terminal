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
    /* Keep the destination straight-alpha too. */
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
    int64_t destination_x64 = destination_x;
    int64_t destination_y64 = destination_y;
    uint32_t source_y_begin;
    uint32_t source_y_end;
    uint32_t source_x_begin;
    uint32_t source_x_end;
    uint32_t source_y;

    if (destination == NULL || destination->pixels == NULL || coverage == NULL ||
        width == 0U || height == 0U || coverage_stride < width) {
        return false;
    }
    if (destination_x64 >= (int64_t)destination->width ||
        destination_y64 >= (int64_t)destination->height) {
        return true;
    }
    source_x_begin = destination_x64 < 0 ? (uint32_t)(-destination_x64) : 0U;
    source_y_begin = destination_y64 < 0 ? (uint32_t)(-destination_y64) : 0U;
    source_x_end = width;
    source_y_end = height;
    if (destination_x64 >= 0 && (uint64_t)destination_x64 + width >
        destination->width) {
        source_x_end = destination->width - (uint32_t)destination_x64;
    }
    if (destination_y64 >= 0 && (uint64_t)destination_y64 + height >
        destination->height) {
        source_y_end = destination->height - (uint32_t)destination_y64;
    }
    if (source_x_begin >= source_x_end || source_y_begin >= source_y_end) {
        return true;
    }
    for (source_y = source_y_begin; source_y < source_y_end; ++source_y) {
        uint32_t source_x;
        int output_y = (int)(destination_y64 + (int64_t)source_y);

        for (source_x = source_x_begin; source_x < source_x_end; ++source_x) {
            uint8_t coverage_value = coverage[(size_t)source_y *
                                               coverage_stride + source_x];
            uint8_t pixel_alpha = (uint8_t)hud_div255(
                (unsigned int)alpha * coverage_value);

            if (pixel_alpha != 0U) {
                hud_blend_pixel_unchecked(destination,
                                           (int)(destination_x64 +
                                                 (int64_t)source_x),
                                           output_y, red, green, blue,
                                           pixel_alpha);
            }
        }
    }
    return true;
}
