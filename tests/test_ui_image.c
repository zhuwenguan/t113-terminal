#include "ui_image.h"

#include "test_check.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_WIDTH 800
#define TEST_HEIGHT 600
#define TEST_GUARD_SIZE 4096U
#define TEST_GUARD_BYTE 0xCDU
#define TEST_PADDING_BYTE 0xA5U

static const uint8_t test_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00, 0x00, 0x00,
    0x48, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x05, 0xc1, 0xa1, 0x01, 0x00,
    0x21, 0x08, 0x00, 0x40, 0xbe, 0x92, 0xc9, 0xaf, 0xe8, 0x07, 0x32, 0x13,
    0x98, 0xc8, 0x0c, 0x41, 0x66, 0x05, 0x1d, 0xcf, 0xad, 0xf8, 0x3b, 0x40,
    0x7a, 0x41, 0xa8, 0x2d, 0xa3, 0xbe, 0x83, 0xf8, 0x00, 0x6a, 0xbb, 0xa2,
    0xbd, 0x4c, 0x19, 0x42, 0x47, 0x01, 0x7a, 0x2f, 0x71, 0x3e, 0xe6, 0x63,
    0x87, 0xcf, 0x05, 0x98, 0x0c, 0x92, 0xe3, 0x5a, 0xce, 0x8a, 0xfc, 0x9e,
    0x1f, 0x16, 0x50, 0x11, 0x9c, 0xba, 0x73, 0xe1, 0xfc, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

typedef struct {
    uint8_t *allocation;
    uint8_t *pixels;
    size_t allocation_size;
    UiCanvas canvas;
} GuardedCanvas;

static const uint8_t *test_pixel(const UiCanvas *canvas, int x, int y)
{
    return canvas->pixels + (size_t)y * (size_t)canvas->stride +
           (size_t)x * 4U;
}

static void guarded_canvas_init(GuardedCanvas *guarded, int width, int height,
                                int stride)
{
    int row;

    guarded->allocation_size = TEST_GUARD_SIZE * 2U +
                               (size_t)stride * (size_t)height;
    guarded->allocation = malloc(guarded->allocation_size);
    TEST_CHECK(guarded->allocation != NULL);
    memset(guarded->allocation, TEST_GUARD_BYTE, guarded->allocation_size);
    guarded->pixels = guarded->allocation + TEST_GUARD_SIZE;
    guarded->canvas = (UiCanvas){ guarded->pixels, width, height, stride };
    for (row = 0; row < height; ++row) {
        memset(guarded->pixels + (size_t)row * (size_t)stride, 0,
               (size_t)width * 4U);
        memset(guarded->pixels + (size_t)row * (size_t)stride +
                   (size_t)width * 4U,
               TEST_PADDING_BYTE, (size_t)stride - (size_t)width * 4U);
    }
}

static void guarded_canvas_close(const GuardedCanvas *guarded)
{
    size_t index;
    size_t framebuffer_size = (size_t)guarded->canvas.stride *
                              (size_t)guarded->canvas.height;

    for (index = 0U; index < TEST_GUARD_SIZE; ++index) {
        TEST_CHECK(guarded->allocation[index] == TEST_GUARD_BYTE);
        TEST_CHECK(guarded->allocation[TEST_GUARD_SIZE + framebuffer_size + index] ==
               TEST_GUARD_BYTE);
    }
    for (int row = 0; row < guarded->canvas.height; ++row) {
        const uint8_t *padding = guarded->pixels +
            (size_t)row * (size_t)guarded->canvas.stride +
            (size_t)guarded->canvas.width * 4U;

        for (int column = 0;
             column < guarded->canvas.stride - guarded->canvas.width * 4;
             ++column) {
            TEST_CHECK(padding[column] == TEST_PADDING_BYTE);
        }
    }
    free(guarded->allocation);
}

static void write_test_png(const char *path)
{
    FILE *file = fopen(path, "wb");

    TEST_CHECK(file != NULL);
    TEST_CHECK(fwrite(test_png, sizeof(test_png), 1U, file) == 1U);
    TEST_CHECK(fclose(file) == 0);
}

static void test_load_and_binary_alpha(const char *path)
{
    static const uint8_t expected_alpha[16] = {
        0U, 0U, 0U, 255U,
        255U, 255U, 0U, 255U,
        255U, 255U, 0U, 0U,
        0U, 255U, 255U, 0U
    };
    UiImage *image;
    int row;
    int column;

    image = ui_image_load_png(path);
    TEST_CHECK(image != NULL);
    TEST_CHECK(image->width == 4 && image->height == 4 && image->stride == 16);
    for (row = 0; row < image->height; ++row) {
        for (column = 0; column < image->width; ++column) {
            const uint8_t *pixel = image->pixels +
                (size_t)row * (size_t)image->stride + (size_t)column * 4U;

            TEST_CHECK(pixel[3] == expected_alpha[row * image->width + column]);
        }
    }
    /* Source pixel (3, 0) is RGBA (100, 20, 33, 128), stored as BGRA. */
    TEST_CHECK(image->pixels[12U] == 33U);
    TEST_CHECK(image->pixels[13U] == 20U);
    TEST_CHECK(image->pixels[14U] == 100U);
    TEST_CHECK(image->pixels[15U] == 255U);
    ui_image_free(image);
}

static void test_draw_clipping_stride_and_reuse(const char *path)
{
    UiImage *image = ui_image_load_png(path);
    uint8_t original_pixels[4U * 4U * 4U];
    uint8_t padded_pixels[4U * 20U];
    UiImage padded_image;
    GuardedCanvas guarded;
    const uint8_t *pixel;
    int row;

    TEST_CHECK(image != NULL);
    memcpy(original_pixels, image->pixels, sizeof(original_pixels));
    guarded_canvas_init(&guarded, TEST_WIDTH, TEST_HEIGHT,
                        TEST_WIDTH * 4 + 64);
    ui_image_draw(&guarded.canvas, image, 10, 10);
    pixel = test_pixel(&guarded.canvas, 13, 10);
    TEST_CHECK(pixel[0] == 33U && pixel[1] == 20U && pixel[2] == 100U &&
           pixel[3] == 255U);
    pixel = test_pixel(&guarded.canvas, 10, 10);
    TEST_CHECK(pixel[0] == 0U && pixel[1] == 0U && pixel[2] == 0U &&
           pixel[3] == 0U);

    ui_image_draw(&guarded.canvas, image, -1, -1);
    pixel = test_pixel(&guarded.canvas, 0, 0);
    TEST_CHECK(pixel[0] == 32U && pixel[1] == 50U && pixel[2] == 40U &&
           pixel[3] == 255U);
    ui_image_draw(&guarded.canvas, image, 795, 595);
    ui_image_draw(&guarded.canvas, image, 900, 900);
    ui_image_draw(&guarded.canvas, image, -20, -20);
    ui_image_draw(&guarded.canvas, NULL, 10, 10);

    memset(padded_pixels, 0xee, sizeof(padded_pixels));
    for (row = 0; row < 4; ++row) {
        memcpy(padded_pixels + (size_t)row * 20U,
               image->pixels + (size_t)row * 16U, 16U);
    }
    padded_image = (UiImage){ padded_pixels, 4, 4, 20 };
    ui_image_draw(&guarded.canvas, &padded_image, 30, 30);
    pixel = test_pixel(&guarded.canvas, 33, 30);
    TEST_CHECK(pixel[0] == 33U && pixel[1] == 20U && pixel[2] == 100U &&
           pixel[3] == 255U);

    for (int repeat = 0; repeat < 1000; ++repeat) {
        ui_image_draw(&guarded.canvas, image, 100, 100);
    }
    TEST_CHECK(memcmp(original_pixels, image->pixels, sizeof(original_pixels)) == 0);
    guarded_canvas_close(&guarded);
    ui_image_free(image);
}

static void test_load_real_icon(const char *path)
{
    UiImage *image = ui_image_load_png(path);
    size_t pixel_count;
    size_t index;
    bool has_opaque_pixel = false;

    TEST_CHECK(image != NULL);
    TEST_CHECK(image->width > 0 && image->height > 0);
    pixel_count = (size_t)image->width * (size_t)image->height;
    for (index = 0U; index < pixel_count; ++index) {
        if (image->pixels[index * 4U + 3U] == 255U) {
            has_opaque_pixel = true;
            break;
        }
    }
    TEST_CHECK(has_opaque_pixel);
    ui_image_free(image);
}

int main(int argc, char **argv)
{
    char path[96];

    (void)snprintf(path, sizeof(path), "/tmp/t113_ui_image_%ld.png",
                   (long)getpid());
    write_test_png(path);
    test_load_and_binary_alpha(path);
    test_draw_clipping_stride_and_reuse(path);
    for (int argument_index = 1; argument_index < argc; ++argument_index) {
        test_load_real_icon(argv[argument_index]);
    }
    TEST_CHECK(remove(path) == 0);
    puts("ui_image tests: PASS");
    return EXIT_SUCCESS;
}
