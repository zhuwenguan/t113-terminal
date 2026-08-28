#include "hud_widget.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GUARD_SIZE 4096U
#define TEST_GUARD_BYTE 0xCDU

/* A deterministic text adapter lets widget layout be tested without making
 * the geometry test depend on a host FreeType development installation. */
struct UiFont {
    int char_width;
    int line_height;
};

typedef struct {
    int x;
    int baseline_y;
    uint32_t color;
    char text[32];
} TextCall;

static TextCall text_calls[16];
static size_t text_call_count;

int ui_text_measure(UiFont *font, const char *text, int *width, int *height)
{
    size_t length;

    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (font == NULL || text == NULL || width == NULL || height == NULL) {
        return -1;
    }
    length = strlen(text);
    if (font->char_width <= 0 || font->line_height <= 0 ||
        length > (size_t)INT_MAX / (size_t)font->char_width) {
        return -1;
    }
    *width = (int)length * font->char_width;
    *height = font->line_height;
    return 0;
}

int ui_text(UiCanvas *canvas, UiFont *font, int x, int baseline_y,
            uint32_t color, const char *text)
{
    int width;
    int height;

    if (ui_text_measure(font, text, &width, &height) < 0) return -1;
    if (text_call_count < sizeof(text_calls) / sizeof(text_calls[0])) {
        TextCall *call = &text_calls[text_call_count++];

        call->x = x;
        call->baseline_y = baseline_y;
        call->color = color;
        (void)snprintf(call->text, sizeof(call->text), "%s", text);
    }
    /* The stub still uses the production canvas API, so clipping and stride
     * checks cover the text widget's placement as well. */
    ui_fill_rect(canvas, x, baseline_y - height, width, height, color);
    return 0;
}

typedef struct {
    uint8_t *allocation;
    uint8_t *pixels;
    size_t framebuffer_size;
    UiCanvas canvas;
} GuardedCanvas;

static void guarded_canvas_init(GuardedCanvas *guarded, int width, int height,
                                int stride)
{
    size_t row;

    assert(stride >= width * 4);
    guarded->framebuffer_size = (size_t)stride * (size_t)height;
    guarded->allocation = malloc(TEST_GUARD_SIZE * 2U +
                                  guarded->framebuffer_size);
    assert(guarded->allocation != NULL);
    memset(guarded->allocation, TEST_GUARD_BYTE,
           TEST_GUARD_SIZE * 2U + guarded->framebuffer_size);
    guarded->pixels = guarded->allocation + TEST_GUARD_SIZE;
    guarded->canvas = (UiCanvas){ guarded->pixels, width, height, stride };
    for (row = 0U; row < (size_t)height; ++row) {
        memset(guarded->pixels + row * (size_t)stride, 0,
               (size_t)width * 4U);
    }
}

static uint32_t pixel_color(const UiCanvas *canvas, int x, int y)
{
    const uint8_t *pixel = canvas->pixels + (size_t)y *
                           (size_t)canvas->stride + (size_t)x * 4U;

    return UI_ARGB(pixel[3], pixel[2], pixel[1], pixel[0]);
}

static void guarded_canvas_close(const GuardedCanvas *guarded)
{
    size_t row;
    size_t index;

    for (row = 0U; row < (size_t)guarded->canvas.height; ++row) {
        const uint8_t *padding = guarded->pixels +
            row * (size_t)guarded->canvas.stride +
            (size_t)guarded->canvas.width * 4U;

        for (index = (size_t)guarded->canvas.width * 4U;
             index < (size_t)guarded->canvas.stride; ++index) {
            assert(padding[index - (size_t)guarded->canvas.width * 4U] ==
                   TEST_GUARD_BYTE);
        }
    }
    for (index = 0U; index < TEST_GUARD_SIZE; ++index) {
        assert(guarded->allocation[index] == TEST_GUARD_BYTE);
        assert(guarded->allocation[TEST_GUARD_SIZE +
                                   guarded->framebuffer_size + index] ==
               TEST_GUARD_BYTE);
    }
    free(guarded->allocation);
}

static void reset_text_calls(void)
{
    memset(text_calls, 0, sizeof(text_calls));
    text_call_count = 0U;
}

static void test_panel(void)
{
    GuardedCanvas guarded;
    HudPanelStyle style = {
        UI_ARGB(255U, 10U, 20U, 30U),
        UI_ARGB(255U, 200U, 210U, 220U),
        2,
        0
    };

    guarded_canvas_init(&guarded, 64, 48, 64 * 4 + 64);
    hud_panel(&guarded.canvas, (HudRect){ 4, 4, 20, 12 }, &style);
    assert(pixel_color(&guarded.canvas, 8, 8) == style.background_color);
    assert(pixel_color(&guarded.canvas, 4, 4) == style.border_color);

    style.radius = 4;
    style.border_width = 1;
    hud_panel(&guarded.canvas, (HudRect){ 30, 4, 24, 16 }, &style);
    assert(pixel_color(&guarded.canvas, 42, 12) == style.background_color);
    hud_panel(&guarded.canvas, (HudRect){ -10, -6, 24, 18 }, &style);
    hud_panel(&guarded.canvas, (HudRect){ INT_MAX, INT_MAX, 8, 8 }, &style);
    hud_panel(&guarded.canvas, (HudRect){ 1, 1, 0, 8 }, &style);
    guarded_canvas_close(&guarded);
}

static void test_text_value(void)
{
    GuardedCanvas guarded;
    struct UiFont label_font = { 8, 10 };
    struct UiFont value_font = { 20, 24 };
    struct UiFont unit_font = { 6, 10 };
    HudTextValueStyle style = {
        &label_font,
        &value_font,
        &unit_font,
        UI_COLOR_WHITE,
        UI_ARGB(255U, 20U, 200U, 240U),
        UI_ARGB(255U, 200U, 200U, 200U),
        2,
        3,
        HUD_WIDGET_ALIGN_CENTER
    };

    guarded_canvas_init(&guarded, 200, 150, 200 * 4 + 64);
    reset_text_calls();
    hud_text_value(&guarded.canvas, (HudRect){ 10, 50, 180, 80 }, &style,
                   "SPEED", "86", "km/h");
    assert(text_call_count == 3U);
    assert(strcmp(text_calls[0].text, "SPEED") == 0);
    assert(text_calls[0].x == 80);
    assert(text_calls[0].baseline_y == 60);
    assert(strcmp(text_calls[1].text, "86") == 0);
    assert(text_calls[1].x == 66);
    assert(text_calls[1].baseline_y == 86);
    assert(strcmp(text_calls[2].text, "km/h") == 0);
    assert(text_calls[2].x == 109);
    assert(text_calls[2].baseline_y == 86);

    style.align = HUD_WIDGET_ALIGN_LEFT;
    reset_text_calls();
    hud_text_value(&guarded.canvas, (HudRect){ 10, 50, 180, 80 }, &style,
                   "A", "12", "V");
    assert(text_calls[0].x == 10);
    assert(text_calls[1].x == 10);
    assert(text_calls[2].x == 53);

    style.align = HUD_WIDGET_ALIGN_RIGHT;
    reset_text_calls();
    hud_text_value(&guarded.canvas, (HudRect){ 10, 50, 180, 80 }, &style,
                   "A", "12", "V");
    assert(text_calls[0].x == 182);
    assert(text_calls[1].x == 141);
    assert(text_calls[2].x == 184);

    reset_text_calls();
    hud_text_value(&guarded.canvas, (HudRect){ -50, -40, 80, 50 }, &style,
                   "CLIPPED", "9", "V");
    guarded_canvas_close(&guarded);
}

static void test_segment_bar(void)
{
    GuardedCanvas guarded;
    HudSegmentBarStyle style = {
        UI_ARGB(255U, 40U, 220U, 120U),
        UI_ARGB(255U, 50U, 50U, 60U),
        UI_ARGB(255U, 255U, 255U, 255U),
        10,
        2,
        0,
        0
    };

    guarded_canvas_init(&guarded, 160, 80, 160 * 4 + 64);
    hud_segment_bar(&guarded.canvas, (HudRect){ 4, 4, 100, 8 }, &style,
                    -1.0f);
    assert(pixel_color(&guarded.canvas, 5, 5) == style.inactive_color);
    hud_segment_bar(&guarded.canvas, (HudRect){ 4, 20, 100, 8 }, &style,
                    0.5f);
    assert(pixel_color(&guarded.canvas, 5, 21) == style.active_color);
    assert(pixel_color(&guarded.canvas, 97, 21) == style.inactive_color);
    hud_segment_bar(&guarded.canvas, (HudRect){ 4, 36, 100, 8 }, &style,
                    0.99f);
    assert(pixel_color(&guarded.canvas, 97, 37) == style.inactive_color);
    hud_segment_bar(&guarded.canvas, (HudRect){ 4, 52, 100, 8 }, &style,
                    2.0f);
    assert(pixel_color(&guarded.canvas, 97, 53) == style.active_color);

    style.segment_count = 1;
    style.gap = 1000;
    hud_segment_bar(&guarded.canvas, (HudRect){ 4, 64, 20, 8 }, &style,
                    1.0f);
    assert(pixel_color(&guarded.canvas, 5, 65) == style.active_color);
    style.segment_count = 20;
    style.gap = 0;
    hud_segment_bar(&guarded.canvas, (HudRect){ 40, 64, 5, 8 }, &style,
                    1.0f);
    style.segment_count = 4;
    style.radius = 3;
    style.border_width = 1;
    hud_segment_bar(&guarded.canvas, (HudRect){ -8, 10, 32, 10 }, &style,
                    0.5f);
    hud_segment_bar(&guarded.canvas, (HudRect){ 150, 70, 32, 10 }, &style,
                    0.5f);
    guarded_canvas_close(&guarded);
}

static void test_status_indicator(void)
{
    GuardedCanvas guarded;
    struct UiFont font = { 8, 12 };
    HudStatusStyle style = {
        &font,
        UI_ARGB(255U, 50U, 220U, 100U),
        UI_ARGB(255U, 220U, 60U, 70U),
        UI_COLOR_WHITE,
        4,
        2
    };

    guarded_canvas_init(&guarded, 160, 80, 160 * 4 + 64);
    reset_text_calls();
    hud_status_indicator(&guarded.canvas, 20, 20, &style, true, "READY");
    assert(pixel_color(&guarded.canvas, 24, 26) == style.active_color);
    assert(text_call_count == 1U);
    assert(text_calls[0].x == 30);
    assert(text_calls[0].baseline_y == 32);

    hud_status_indicator(&guarded.canvas, 60, 20, &style, false, "REC");
    assert(pixel_color(&guarded.canvas, 64, 26) == style.inactive_color);
    hud_status_indicator(&guarded.canvas, -5, 74, &style, true, "GPS");
    style.dot_radius = 0;
    hud_status_indicator(&guarded.canvas, 100, 20, &style, true, "CAN");
    guarded_canvas_close(&guarded);
}

static void test_composite_and_memory_safety(void)
{
    GuardedCanvas guarded;
    struct UiFont font = { 8, 12 };
    HudPanelStyle panel_style = {
        UI_ARGB(255U, 12U, 18U, 28U),
        UI_ARGB(255U, 80U, 180U, 220U),
        2,
        8
    };
    HudTextValueStyle text_style = {
        &font, &font, &font,
        UI_COLOR_WHITE,
        UI_COLOR_WHITE,
        UI_ARGB(255U, 180U, 190U, 200U),
        2, 2, HUD_WIDGET_ALIGN_CENTER
    };
    HudSegmentBarStyle bar_style = {
        UI_ARGB(255U, 50U, 220U, 120U),
        UI_ARGB(255U, 40U, 50U, 60U),
        UI_COLOR_WHITE,
        20, 3, 1, 2
    };
    HudStatusStyle status_style = {
        &font,
        UI_ARGB(255U, 50U, 220U, 120U),
        UI_ARGB(255U, 220U, 60U, 70U),
        UI_COLOR_WHITE,
        4, 2
    };
    int iteration;

    guarded_canvas_init(&guarded, 800, 600, 800 * 4 + 64);
    ui_clear(&guarded.canvas, UI_COLOR_TRANSPARENT);
    hud_panel(&guarded.canvas, (HudRect){ 30, 30, 400, 220 }, &panel_style);
    hud_text_value(&guarded.canvas, (HudRect){ 40, 45, 380, 100 },
                   &text_style, "SPEED", "86", "km/h");
    hud_segment_bar(&guarded.canvas, (HudRect){ 50, 150, 350, 16 },
                    &bar_style, 0.63f);
    hud_status_indicator(&guarded.canvas, 50, 185, &status_style, true,
                         "READY");

    /* Deliberately pathological placement is part of the guard test. */
    hud_panel(&guarded.canvas, (HudRect){ -100, -100, 300, 180 },
              &panel_style);
    hud_text_value(&guarded.canvas, (HudRect){ -100, 560, 150, 100 },
                   &text_style, "EDGE", "1", "V");
    hud_segment_bar(&guarded.canvas, (HudRect){ 790, 580, 300, 30 },
                    &bar_style, 0.5f);
    hud_status_indicator(&guarded.canvas, 790, 590, &status_style, false,
                         "BOTTOM");
    hud_panel(&guarded.canvas,
              (HudRect){ INT_MIN, INT_MIN, INT_MAX, INT_MAX }, &panel_style);
    for (iteration = 0; iteration < 1000; ++iteration) {
        hud_segment_bar(&guarded.canvas, (HudRect){ 760, 590, 200, 20 },
                        &bar_style, (float)(iteration % 101) / 100.0f);
    }
    guarded_canvas_close(&guarded);
}

int main(void)
{
    test_panel();
    test_text_value();
    test_segment_bar();
    test_status_indicator();
    test_composite_and_memory_safety();
    puts("hud_widget tests: PASS");
    return 0;
}
