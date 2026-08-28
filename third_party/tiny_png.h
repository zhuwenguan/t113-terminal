#ifndef T113_TINY_PNG_H
#define T113_TINY_PNG_H

#include <stddef.h>
#include <stdint.h>

/*
 * Project-owned, deliberately small PNG reader for the fixed HUD asset set.
 * It accepts PNG RGBA8/non-interlaced images and expands them to RGBA8.
 * A small internal DEFLATE decoder keeps the target free of a desktop
 * graphics stack and an additional runtime PNG dependency.
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *rgba;
    size_t bytes;
    uint8_t alpha_min;
    uint8_t alpha_max;
    uint32_t alpha_x_min;
    uint32_t alpha_y_min;
    uint32_t alpha_x_max;
    uint32_t alpha_y_max;
} TinyPngImage;

int tiny_png_load_rgba(const char *path, TinyPngImage *image,
                       char *error_message, size_t error_message_size);
void tiny_png_image_close(TinyPngImage *image);

#endif
