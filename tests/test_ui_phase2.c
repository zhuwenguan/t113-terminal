#include "ui_draw.h"
#include "ui_font.h"
#include "ui_image.h"
#include "hud_renderer.h"

#include "test_check.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_WIDTH 800
#define TEST_HEIGHT 600
#define TEST_GUARD_SIZE 4096U
#define TEST_GUARD_BYTE 0xCDU

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

static void write_test_png(const char *path)
{
    FILE *file = fopen(path, "wb");

    TEST_CHECK(file != NULL);
    TEST_CHECK(fwrite(test_png, sizeof(test_png), 1U, file) == 1U);
    TEST_CHECK(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    uint8_t *allocation;
    uint8_t *pixels;
    HudSurface surface;
    UiCanvas canvas;
    UiFont *font;
    UiImage *image;
    char image_path[64];
    size_t framebuffer_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 4U;
    size_t index;

    TEST_CHECK(argc >= 2);
    TEST_CHECK(snprintf(image_path, sizeof(image_path),
                    "/tmp/t113_ui_phase2_%ld.png", (long)getpid()) > 0);
    write_test_png(image_path);
    allocation = malloc(TEST_GUARD_SIZE * 2U + framebuffer_size);
    TEST_CHECK(allocation != NULL);
    memset(allocation, TEST_GUARD_BYTE,
           TEST_GUARD_SIZE * 2U + framebuffer_size);
    pixels = allocation + TEST_GUARD_SIZE;
    surface = (HudSurface){ pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4,
                            HUD_PIXEL_FORMAT_ARGB8888 };
    canvas = (UiCanvas){ surface.pixels, (int)surface.width,
                         (int)surface.height, (int)surface.stride_bytes };
    memset(pixels, 0, framebuffer_size);

    font = ui_font_load(argv[1], 24);
    image = ui_image_load_png(image_path);
    TEST_CHECK(font != NULL && image != NULL);
    ui_fill_round_rect(&canvas, 140, 180, 520, 220, 20,
                       UI_ARGB(255U, 12U, 20U, 32U));
    ui_round_rect(&canvas, 140, 180, 520, 220, 20, 3,
                  UI_ARGB(255U, 44U, 210U, 232U));
    TEST_CHECK(ui_text(&canvas, font, 170, 230,
                   UI_ARGB(255U, 255U, 255U, 255U), "SPEED") == 0);
    TEST_CHECK(ui_text(&canvas, font, 400, 320,
                   UI_ARGB(255U, 255U, 210U, 64U), "86") == 0);
    TEST_CHECK(ui_text(&canvas, font, 390, 360,
                   UI_ARGB(255U, 64U, 230U, 122U), "READY") == 0);
    ui_image_draw(&canvas, image, 760, 20);

    for (index = 3U; index < framebuffer_size; index += 4U) {
        if (pixels[index] != 0U) break;
    }
    TEST_CHECK(index < framebuffer_size);
    for (index = 0U; index < TEST_GUARD_SIZE; ++index) {
        TEST_CHECK(allocation[index] == TEST_GUARD_BYTE);
        TEST_CHECK(allocation[TEST_GUARD_SIZE + framebuffer_size + index] ==
               TEST_GUARD_BYTE);
    }
    TEST_CHECK(surface.pixels == canvas.pixels);
    TEST_CHECK(surface.stride_bytes == (uint32_t)canvas.stride);
    TEST_CHECK(surface.pixel_format == HUD_PIXEL_FORMAT_ARGB8888);
    ui_image_free(image);
    ui_font_free(font);
    TEST_CHECK(remove(image_path) == 0);
    free(allocation);
    puts("ui phase2 smoke: PASS");
    return EXIT_SUCCESS;
}
