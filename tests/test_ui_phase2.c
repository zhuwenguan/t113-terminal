#include "ui_draw.h"
#include "ui_font.h"
#include "ui_image.h"
#include "hud_renderer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 800
#define TEST_HEIGHT 600
#define TEST_GUARD_SIZE 4096U
#define TEST_GUARD_BYTE 0xCDU

int main(int argc, char **argv)
{
    uint8_t *allocation;
    uint8_t *pixels;
    HudSurface surface;
    UiCanvas canvas;
    UiFont *font;
    UiImage *image;
    size_t framebuffer_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 4U;
    size_t index;

    assert(argc >= 3);
    allocation = malloc(TEST_GUARD_SIZE * 2U + framebuffer_size);
    assert(allocation != NULL);
    memset(allocation, TEST_GUARD_BYTE,
           TEST_GUARD_SIZE * 2U + framebuffer_size);
    pixels = allocation + TEST_GUARD_SIZE;
    surface = (HudSurface){ pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4,
                            HUD_PIXEL_FORMAT_ARGB8888 };
    canvas = (UiCanvas){ surface.pixels, (int)surface.width,
                         (int)surface.height, (int)surface.stride_bytes };
    memset(pixels, 0, framebuffer_size);

    font = ui_font_load(argv[1], 24);
    image = ui_image_load_png(argv[2]);
    assert(font != NULL && image != NULL);
    ui_fill_round_rect(&canvas, 140, 180, 520, 220, 20,
                       UI_ARGB(255U, 12U, 20U, 32U));
    ui_round_rect(&canvas, 140, 180, 520, 220, 20, 3,
                  UI_ARGB(255U, 44U, 210U, 232U));
    assert(ui_text(&canvas, font, 170, 230,
                   UI_ARGB(255U, 255U, 255U, 255U), "SPEED") == 0);
    assert(ui_text(&canvas, font, 400, 320,
                   UI_ARGB(255U, 255U, 210U, 64U), "86") == 0);
    assert(ui_text(&canvas, font, 390, 360,
                   UI_ARGB(255U, 64U, 230U, 122U), "READY") == 0);
    ui_image_draw(&canvas, image, 760, 20);

    for (index = 3U; index < framebuffer_size; index += 4U) {
        if (pixels[index] != 0U) break;
    }
    assert(index < framebuffer_size);
    for (index = 0U; index < TEST_GUARD_SIZE; ++index) {
        assert(allocation[index] == TEST_GUARD_BYTE);
        assert(allocation[TEST_GUARD_SIZE + framebuffer_size + index] ==
               TEST_GUARD_BYTE);
    }
    assert(surface.pixels == canvas.pixels);
    assert(surface.stride_bytes == (uint32_t)canvas.stride);
    assert(surface.pixel_format == HUD_PIXEL_FORMAT_ARGB8888);
    ui_image_free(image);
    ui_font_free(font);
    free(allocation);
    puts("ui phase2 smoke: PASS");
    return EXIT_SUCCESS;
}
