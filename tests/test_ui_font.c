#include "ui_font.h"

#include "test_check.h"
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
    UiCanvas canvas;
} GuardedCanvas;

static void guarded_canvas_init(GuardedCanvas *guarded)
{
    size_t framebuffer_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 4U;

    guarded->allocation = malloc(TEST_GUARD_SIZE * 2U + framebuffer_size);
    TEST_CHECK(guarded->allocation != NULL);
    memset(guarded->allocation, TEST_GUARD_BYTE,
           TEST_GUARD_SIZE * 2U + framebuffer_size);
    guarded->pixels = guarded->allocation + TEST_GUARD_SIZE;
    guarded->canvas = (UiCanvas){ guarded->pixels, TEST_WIDTH, TEST_HEIGHT,
                                  TEST_WIDTH * 4 };
    memset(guarded->pixels, 0, framebuffer_size);
}

static void guarded_canvas_close(const GuardedCanvas *guarded)
{
    size_t framebuffer_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 4U;
    size_t index;

    for (index = 0U; index < TEST_GUARD_SIZE; ++index) {
        TEST_CHECK(guarded->allocation[index] == TEST_GUARD_BYTE);
        TEST_CHECK(guarded->allocation[TEST_GUARD_SIZE + framebuffer_size + index] ==
               TEST_GUARD_BYTE);
    }
    free(guarded->allocation);
}

static bool canvas_has_pixels(const UiCanvas *canvas, bool *has_antialias)
{
    size_t index;
    size_t bytes = (size_t)canvas->width * (size_t)canvas->height * 4U;
    bool has_pixels = false;

    *has_antialias = false;
    for (index = 3U; index < bytes; index += 4U) {
        if (canvas->pixels[index] != 0U) has_pixels = true;
        if (canvas->pixels[index] > 0U && canvas->pixels[index] < 255U) {
            *has_antialias = true;
        }
    }
    return has_pixels;
}

static void test_text_measure(UiFont *font)
{
    static const char *const texts[] = {
        "SPEED", "LAP", "DELTA", "BATTERY", "POWER", "READY",
        "0123456789", "86", "398 V", "32.5 kW", "+0.214"
    };
    size_t index;

    for (index = 0U; index < sizeof(texts) / sizeof(texts[0]); ++index) {
        int width;
        int height;

        TEST_CHECK(ui_text_measure(font, texts[index], &width, &height) == 0);
        TEST_CHECK(width > 0);
        TEST_CHECK(height > 0);
    }
}

int main(int argc, char **argv)
{
    GuardedCanvas guarded;
    UiFont *small_font;
    UiFont *large_font;
    bool has_antialias;
    int large_width;
    int large_height;

    TEST_CHECK(argc >= 2);
    small_font = ui_font_load(argv[1], 24);
    large_font = ui_font_load(argv[1], 48);
    TEST_CHECK(small_font != NULL && large_font != NULL);
    test_text_measure(small_font);
    TEST_CHECK(ui_text_measure(large_font, "398 V", &large_width,
                           &large_height) == 0);
    TEST_CHECK(large_width > 0 && large_height > 0);

    guarded_canvas_init(&guarded);
    TEST_CHECK(ui_text(&guarded.canvas, small_font, 24, 48,
                   UI_ARGB(255U, 255U, 255U, 255U), "SPEED") == 0);
    TEST_CHECK(ui_text(&guarded.canvas, small_font, -5, -5,
                   UI_ARGB(255U, 44U, 210U, 232U), "LAP") == 0);
    TEST_CHECK(ui_text(&guarded.canvas, small_font, 799, 120,
                   UI_ARGB(255U, 255U, 66U, 72U), "DELTA") == 0);
    TEST_CHECK(ui_text(&guarded.canvas, small_font, 100, 595,
                   UI_ARGB(255U, 64U, 230U, 122U), "READY") == 0);
    TEST_CHECK(ui_text(&guarded.canvas, small_font, INT_MIN, INT_MIN,
                   UI_ARGB(255U, 255U, 255U, 255U), "POWER") == 0);
    TEST_CHECK(ui_text(&guarded.canvas, small_font, INT_MAX, INT_MAX,
                   UI_ARGB(255U, 255U, 255U, 255U), "BATTERY") == 0);
    TEST_CHECK(ui_text(&guarded.canvas, large_font, 360, 280,
                   UI_ARGB(255U, 255U, 255U, 255U), "86") == 0);
    TEST_CHECK(canvas_has_pixels(&guarded.canvas, &has_antialias));
    TEST_CHECK(has_antialias);
    guarded_canvas_close(&guarded);

    ui_font_free(small_font);
    ui_font_free(large_font);
    ui_font_free(NULL);
    puts("ui_font tests: PASS");
    return EXIT_SUCCESS;
}
