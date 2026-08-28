#include "ui_draw.h"

#include "hud_renderer.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 800
#define TEST_HEIGHT 600
#define TEST_GUARD_SIZE 4096U
#define TEST_GUARD_BYTE 0xCDU

typedef struct {
    uint8_t *allocation;
    uint8_t *pixels;
    size_t allocation_size;
    UiCanvas canvas;
} GuardedCanvas;

static uint32_t test_pixel_color(const UiCanvas *canvas, int x, int y)
{
    const uint8_t *pixel = canvas->pixels + (size_t)y * canvas->stride +
                           (size_t)x * 4U;

    return UI_ARGB(pixel[3], pixel[2], pixel[1], pixel[0]);
}

static void test_guarded_canvas_init(GuardedCanvas *guarded, int width,
                                     int height, int stride)
{
    guarded->allocation_size = TEST_GUARD_SIZE * 2U +
                               (size_t)stride * (size_t)height;
    guarded->allocation = malloc(guarded->allocation_size);
    assert(guarded->allocation != NULL);
    memset(guarded->allocation, TEST_GUARD_BYTE, guarded->allocation_size);
    guarded->pixels = guarded->allocation + TEST_GUARD_SIZE;
    guarded->canvas = (UiCanvas){ guarded->pixels, width, height, stride };
}

static void test_guarded_canvas_close(const GuardedCanvas *guarded)
{
    size_t index;

    for (index = 0U; index < TEST_GUARD_SIZE; ++index) {
        assert(guarded->allocation[index] == TEST_GUARD_BYTE);
        assert(guarded->allocation[TEST_GUARD_SIZE +
                                   (size_t)guarded->canvas.stride *
                                       (size_t)guarded->canvas.height + index] ==
               TEST_GUARD_BYTE);
    }
    free(guarded->allocation);
}

static void test_color_and_stride(void)
{
    GuardedCanvas guarded;
    const uint32_t color = UI_ARGB(0x12U, 0x34U, 0x56U, 0x78U);
    int row;
    int column;

    test_guarded_canvas_init(&guarded, 7, 5, 7 * 4 + 64);
    ui_clear(&guarded.canvas, color);
    for (row = 0; row < guarded.canvas.height; ++row) {
        const uint8_t *padding = guarded.canvas.pixels +
                                 (size_t)row * guarded.canvas.stride +
                                 guarded.canvas.width * 4;

        for (column = 0; column < guarded.canvas.width; ++column) {
            assert(test_pixel_color(&guarded.canvas, column, row) == color);
        }
        for (column = 0; column < 64; ++column) {
            assert(padding[column] == TEST_GUARD_BYTE);
        }
    }
    test_guarded_canvas_close(&guarded);
}

static void test_rectangles(void)
{
    uint8_t pixels[16 * 12 * 4];
    UiCanvas canvas = { pixels, 16, 12, 16 * 4 };
    const uint32_t color = UI_COLOR_WHITE;
    int row;
    int column;

    memset(pixels, 0, sizeof(pixels));
    ui_fill_rect(&canvas, -3, -2, 8, 6, color);
    for (row = 0; row < canvas.height; ++row) {
        for (column = 0; column < canvas.width; ++column) {
            bool expected = row < 4 && column < 5;

            assert((test_pixel_color(&canvas, column, row) == color) ==
                   expected);
        }
    }

    memset(pixels, 0, sizeof(pixels));
    ui_rect(&canvas, -1, 2, 12, 7, 2, color);
    assert(test_pixel_color(&canvas, 0, 2) == color);
    assert(test_pixel_color(&canvas, 5, 3) == color);
    assert(test_pixel_color(&canvas, 0, 5) == color);
    assert(test_pixel_color(&canvas, 5, 5) == 0U);
    assert(test_pixel_color(&canvas, 9, 8) == color);

    ui_fill_rect(&canvas, INT_MIN, INT_MIN, INT_MAX, INT_MAX, color);
    ui_rect(&canvas, INT_MAX, INT_MAX, INT_MAX, INT_MAX, 4, color);
}

static void test_lines_and_circles(void)
{
    uint8_t pixels[40 * 30 * 4];
    UiCanvas canvas = { pixels, 40, 30, 40 * 4 };
    int colored_pixels = 0;
    int row;
    int column;

    memset(pixels, 0, sizeof(pixels));
    ui_line(&canvas, -20, 4, 35, 4, 1, UI_COLOR_WHITE);
    ui_line(&canvas, 5, -20, 5, 25, 2, UI_COLOR_WHITE);
    ui_line(&canvas, 0, 0, 30, 20, 4, UI_COLOR_WHITE);
    ui_line(&canvas, 30, 0, 0, 20, 1, UI_COLOR_WHITE);
    ui_line(&canvas, 0, 20, 30, 0, 2, UI_COLOR_WHITE);
    ui_line(&canvas, INT_MIN, INT_MIN, INT_MAX, INT_MAX, 1, UI_COLOR_WHITE);
    for (row = 0; row < canvas.height; ++row) {
        for (column = 0; column < canvas.width; ++column) {
            if (test_pixel_color(&canvas, column, row) != 0U) {
                ++colored_pixels;
            }
        }
    }
    assert(colored_pixels > 100);

    memset(pixels, 0, sizeof(pixels));
    ui_fill_circle(&canvas, 0, 0, 8, UI_COLOR_WHITE);
    ui_fill_circle(&canvas, 39, 29, 12, UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 0, 0) == UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 39, 29) == UI_COLOR_WHITE);

    memset(pixels, 0, sizeof(pixels));
    ui_circle(&canvas, 20, 15, 8, 2, UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 20, 15) == 0U);
    assert(test_pixel_color(&canvas, 20, 7) == UI_COLOR_WHITE);
}

static void test_round_rectangles(void)
{
    uint8_t pixels[32 * 24 * 4];
    UiCanvas canvas = { pixels, 32, 24, 32 * 4 };
    int colored_pixels = 0;
    int row;
    int column;

    memset(pixels, 0, sizeof(pixels));
    ui_fill_round_rect(&canvas, -4, -3, 20, 14, 8, UI_COLOR_WHITE);
    ui_fill_round_rect(&canvas, 20, 12, 30, 20, 100, UI_COLOR_WHITE);
    for (row = 0; row < canvas.height; ++row) {
        for (column = 0; column < canvas.width; ++column) {
            if (test_pixel_color(&canvas, column, row) != 0U) {
                ++colored_pixels;
            }
        }
    }
    assert(colored_pixels > 100);

    memset(pixels, 0, sizeof(pixels));
    ui_round_rect(&canvas, 4, 4, 20, 14, 4, 2, UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 4, 8) == UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 14, 10) == 0U);
    assert(test_pixel_color(&canvas, 14, 4) == UI_COLOR_WHITE);

    memset(pixels, 0, sizeof(pixels));
    ui_round_rect(&canvas, 4, 4, 20, 14, 0, 2, UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 4, 4) == UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 14, 10) == 0U);
}

static void test_guard_memory_and_clipping(void)
{
    GuardedCanvas guarded;

    test_guarded_canvas_init(&guarded, TEST_WIDTH, TEST_HEIGHT,
                             TEST_WIDTH * 4 + 64);
    ui_clear(&guarded.canvas, UI_COLOR_TRANSPARENT);
    ui_fill_rect(&guarded.canvas, -1000, -1000, 2000, 2000,
                 UI_COLOR_WHITE);
    ui_line(&guarded.canvas, -500, -500, 1500, 1000, 4, UI_COLOR_WHITE);
    ui_circle(&guarded.canvas, -20, 50, 100, 4, UI_COLOR_WHITE);
    ui_fill_circle(&guarded.canvas, 799, 599, 100, UI_COLOR_WHITE);
    ui_round_rect(&guarded.canvas, -50, -50, 200, 100, 20, 3,
                  UI_COLOR_WHITE);
    ui_fill_round_rect(&guarded.canvas, 750, 550, 300, 200, 40,
                       UI_COLOR_WHITE);
    ui_fill_rect(&guarded.canvas, INT_MIN, INT_MIN, INT_MAX, INT_MAX,
                 UI_COLOR_WHITE);
    ui_line(&guarded.canvas, INT_MIN, INT_MAX, INT_MAX, INT_MIN, 4,
            UI_COLOR_WHITE);
    ui_circle(&guarded.canvas, INT_MIN, INT_MIN, INT_MAX, 4,
              UI_COLOR_WHITE);
    ui_fill_circle(&guarded.canvas, INT_MAX, INT_MAX, INT_MAX,
                   UI_COLOR_WHITE);
    ui_round_rect(&guarded.canvas, INT_MIN, INT_MIN, INT_MAX, INT_MAX,
                  INT_MAX, 4, UI_COLOR_WHITE);
    ui_fill_round_rect(&guarded.canvas, INT_MAX, INT_MAX, INT_MAX, INT_MAX,
                       INT_MAX, UI_COLOR_WHITE);
    test_guarded_canvas_close(&guarded);
}

static void test_hud_surface_mapping(void)
{
    uint8_t pixels[16 * 12 * 4];
    HudSurface surface = {
        pixels, 16U, 12U, 16U * 4U, HUD_PIXEL_FORMAT_ARGB8888
    };
    UiCanvas canvas = {
        surface.pixels, (int)surface.width, (int)surface.height,
        (int)surface.stride_bytes
    };

    memset(pixels, 0, sizeof(pixels));
    ui_fill_rect(&canvas, 2, 3, 4, 5, UI_COLOR_WHITE);
    assert(test_pixel_color(&canvas, 2, 3) == UI_COLOR_WHITE);
    assert(surface.pixels == canvas.pixels);
    assert(surface.stride_bytes == (uint32_t)canvas.stride);
    assert(surface.pixel_format == HUD_PIXEL_FORMAT_ARGB8888);
}

static void test_store_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xffU);
    destination[1] = (uint8_t)(value >> 8U);
}

static void test_store_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xffU);
    destination[1] = (uint8_t)((value >> 8U) & 0xffU);
    destination[2] = (uint8_t)((value >> 16U) & 0xffU);
    destination[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static void write_bmp(const UiCanvas *canvas, const char *path)
{
    FILE *file = fopen(path, "wb");
    uint8_t header[54] = { 0U };
    size_t row_stride = ((size_t)canvas->width * 3U + 3U) & ~3U;
    uint8_t *row_pixels = malloc(row_stride);
    int row;
    int column;

    assert(file != NULL);
    assert(row_pixels != NULL);
    header[0] = 'B';
    header[1] = 'M';
    test_store_le32(header + 2, (uint32_t)(54U + row_stride *
                                           (size_t)canvas->height));
    test_store_le32(header + 10, 54U);
    test_store_le32(header + 14, 40U);
    test_store_le32(header + 18, (uint32_t)canvas->width);
    test_store_le32(header + 22, (uint32_t)canvas->height);
    test_store_le16(header + 26, 1U);
    test_store_le16(header + 28, 24U);
    test_store_le32(header + 34, (uint32_t)(row_stride *
                                           (size_t)canvas->height));
    assert(fwrite(header, sizeof(header), 1U, file) == 1U);
    for (row = canvas->height - 1; row >= 0; --row) {
        memset(row_pixels, 0, row_stride);
        for (column = 0; column < canvas->width; ++column) {
            const uint8_t *pixel = canvas->pixels +
                                   (size_t)row * canvas->stride +
                                   (size_t)column * 4U;

            row_pixels[(size_t)column * 3U] = pixel[0];
            row_pixels[(size_t)column * 3U + 1U] = pixel[1];
            row_pixels[(size_t)column * 3U + 2U] = pixel[2];
        }
        assert(fwrite(row_pixels, row_stride, 1U, file) == 1U);
    }
    free(row_pixels);
    assert(fclose(file) == 0);
}

static void draw_visual_sample(UiCanvas *canvas)
{
    ui_clear(canvas, UI_ARGB(255U, 8U, 12U, 18U));
    ui_rect(canvas, 16, 16, 220, 110, 3, UI_ARGB(255U, 255U, 255U, 255U));
    ui_fill_rect(canvas, 28, 32, 80, 36, UI_ARGB(255U, 36U, 180U, 220U));
    ui_line(canvas, 260, 30, 390, 30, 1, UI_ARGB(255U, 255U, 255U, 255U));
    ui_line(canvas, 260, 40, 390, 100, 2, UI_ARGB(255U, 255U, 210U, 64U));
    ui_line(canvas, 390, 100, 260, 40, 4, UI_ARGB(255U, 255U, 66U, 72U));
    ui_fill_circle(canvas, 500, 72, 28, UI_ARGB(255U, 64U, 230U, 122U));
    ui_circle(canvas, 580, 72, 30, 3, UI_ARGB(255U, 44U, 210U, 232U));
    ui_fill_round_rect(canvas, 30, 190, 260, 120, 16,
                       UI_ARGB(255U, 25U, 36U, 52U));
    ui_round_rect(canvas, 30, 190, 260, 120, 16, 3,
                  UI_ARGB(255U, 44U, 210U, 232U));
    ui_fill_round_rect(canvas, 330, 190, 260, 120, 8,
                       UI_ARGB(255U, 25U, 36U, 52U));
    ui_round_rect(canvas, 330, 190, 260, 120, 8, 2,
                  UI_ARGB(255U, 255U, 210U, 64U));
    ui_rect(canvas, -30, 350, 180, 100, 5, UI_ARGB(255U, 255U, 66U, 72U));
    ui_fill_circle(canvas, 799, 599, 100, UI_ARGB(255U, 64U, 230U, 122U));
}

int main(int argc, char **argv)
{
    GuardedCanvas visual;

    test_color_and_stride();
    test_rectangles();
    test_lines_and_circles();
    test_round_rectangles();
    test_guard_memory_and_clipping();
    test_hud_surface_mapping();

    if (argc > 1) {
        test_guarded_canvas_init(&visual, TEST_WIDTH, TEST_HEIGHT,
                                 TEST_WIDTH * 4);
        draw_visual_sample(&visual.canvas);
        write_bmp(&visual.canvas, argv[1]);
        test_guarded_canvas_close(&visual);
    }
    puts("ui_draw tests: PASS");
    return EXIT_SUCCESS;
}
