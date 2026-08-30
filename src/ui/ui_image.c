#include "ui_image.h"

#include "tiny_png.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool ui_image_canvas_valid(const UiCanvas *canvas)
{
    if (canvas == NULL || canvas->pixels == NULL || canvas->width <= 0 ||
        canvas->height <= 0 || canvas->stride <= 0 ||
        canvas->width > INT_MAX / 4 || canvas->stride < canvas->width * 4) {
        return false;
    }
    return (size_t)canvas->height <= SIZE_MAX / (size_t)canvas->stride;
}

static bool ui_image_valid(const UiImage *image)
{
    if (image == NULL || image->pixels == NULL || image->width <= 0 ||
        image->height <= 0 || image->stride <= 0 ||
        image->width > INT_MAX / 4 || image->stride < image->width * 4) {
        return false;
    }
    return (size_t)image->height <= SIZE_MAX / (size_t)image->stride;
}

UiImage *ui_image_load_png(const char *path)
{
    TinyPngImage decoded;
    UiImage *image;
    char error_message[160];
    size_t pixel_count;
    size_t bytes;
    size_t pixel_index;

    if (path == NULL || path[0] == '\0') return NULL;
    memset(&decoded, 0, sizeof(decoded));
    memset(error_message, 0, sizeof(error_message));
    if (tiny_png_load_rgba(path, &decoded, error_message,
                           sizeof(error_message)) < 0) {
        return NULL;
    }
    if (decoded.width > (uint32_t)INT_MAX ||
        decoded.height > (uint32_t)INT_MAX ||
        decoded.width > (uint32_t)(INT_MAX / 4) || decoded.width == 0U ||
        decoded.height == 0U ||
        (size_t)decoded.height >
            SIZE_MAX / ((size_t)decoded.width * 4U)) {
        tiny_png_image_close(&decoded);
        return NULL;
    }
    pixel_count = (size_t)decoded.width * (size_t)decoded.height;
    bytes = pixel_count * 4U;
    image = calloc(1U, sizeof(*image));
    if (image == NULL) {
        tiny_png_image_close(&decoded);
        return NULL;
    }
    image->pixels = malloc(bytes);
    if (image->pixels == NULL) {
        tiny_png_image_close(&decoded);
        free(image);
        return NULL;
    }
    image->width = (int)decoded.width;
    image->height = (int)decoded.height;
    image->stride = image->width * 4;
    for (pixel_index = 0U; pixel_index < pixel_count; ++pixel_index) {
        const uint8_t *source = decoded.rgba + pixel_index * 4U;
        uint8_t *destination = image->pixels + pixel_index * 4U;
        uint8_t alpha = source[3] < 128U ? 0U : 255U;

        destination[0] = source[2];
        destination[1] = source[1];
        destination[2] = source[0];
        destination[3] = alpha;
    }
    tiny_png_image_close(&decoded);
    return image;
}

void ui_image_draw(UiCanvas *canvas, const UiImage *image, int x, int y)
{
    int64_t source_x = 0;
    int64_t source_y = 0;
    int64_t destination_x = (int64_t)x;
    int64_t destination_y = (int64_t)y;
    int64_t draw_width;
    int64_t draw_height;
    int64_t row;

    if (!ui_image_canvas_valid(canvas) || !ui_image_valid(image)) return;
    if (destination_x < 0) {
        source_x = -destination_x;
        destination_x = 0;
    }
    if (destination_y < 0) {
        source_y = -destination_y;
        destination_y = 0;
    }
    if (destination_x >= canvas->width || destination_y >= canvas->height ||
        source_x >= image->width || source_y >= image->height) {
        return;
    }
    draw_width = image->width - source_x;
    draw_height = image->height - source_y;
    if (draw_width > (int64_t)canvas->width - destination_x) {
        draw_width = (int64_t)canvas->width - destination_x;
    }
    if (draw_height > (int64_t)canvas->height - destination_y) {
        draw_height = (int64_t)canvas->height - destination_y;
    }
    for (row = 0; row < draw_height; ++row) {
        int64_t column;
        const uint8_t *source = image->pixels +
            (size_t)(source_y + row) * (size_t)image->stride +
            (size_t)source_x * 4U;
        uint8_t *destination = canvas->pixels +
            (size_t)(destination_y + row) * (size_t)canvas->stride +
            (size_t)destination_x * 4U;

        for (column = 0; column < draw_width; ++column) {
            const uint8_t *source_pixel = source + (size_t)column * 4U;
            uint8_t *destination_pixel = destination +
                (size_t)column * 4U;
            uint8_t blue;
            uint8_t green;
            uint8_t red;
            uint8_t alpha;

            alpha = source_pixel[3];
            if (alpha == 0U) continue;
            blue = source_pixel[0];
            green = source_pixel[1];
            red = source_pixel[2];
            destination_pixel[0] = blue;
            destination_pixel[1] = green;
            destination_pixel[2] = red;
            destination_pixel[3] = alpha;
        }
    }
}

void ui_image_free(UiImage *image)
{
    if (image == NULL) return;
    free(image->pixels);
    free(image);
}
