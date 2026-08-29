#include "hud_layout.h"
#include "hud_render.h"

#include "test_check.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH HUD_UI_WIDTH
#define TEST_HEIGHT HUD_UI_HEIGHT
#define TEST_GUARD_SIZE 4096U
#define TEST_GUARD_BYTE 0xCDU

struct UiFont {
    int pixel_size;
};

static unsigned int font_load_calls;
static char rendered_text[128][48];
static int rendered_text_x[128];
static int rendered_text_baseline_y[128];
static size_t rendered_text_count;

UiFont *ui_font_load(const char *path, int pixel_size)
{
    UiFont *font;

    TEST_CHECK(path != NULL && path[0] != '\0');
    TEST_CHECK(pixel_size > 0);
    font = malloc(sizeof(*font));
    TEST_CHECK(font != NULL);
    font->pixel_size = pixel_size;
    font_load_calls++;
    return font;
}

void ui_font_free(UiFont *font)
{
    free(font);
}

int ui_text_measure(UiFont *font, const char *text, int *width, int *height)
{
    size_t length;
    int character_width;

    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (font == NULL || text == NULL || width == NULL || height == NULL) {
        return -1;
    }
    length = strlen(text);
    character_width = font->pixel_size / 2;
    if (character_width <= 0 || length > (size_t)INT_MAX /
        (size_t)character_width) {
        return -1;
    }
    *width = (int)length * character_width;
    *height = font->pixel_size;
    return 0;
}

int ui_text(UiCanvas *canvas, UiFont *font, int x, int baseline_y,
            uint32_t color, const char *text)
{
    int width;
    int height;
    int64_t top;

    if (ui_text_measure(font, text, &width, &height) < 0) return -1;
    if (rendered_text_count < sizeof(rendered_text) /
                                sizeof(rendered_text[0])) {
        rendered_text_x[rendered_text_count] = x;
        rendered_text_baseline_y[rendered_text_count] = baseline_y;
        (void)snprintf(rendered_text[rendered_text_count],
                       sizeof(rendered_text[rendered_text_count]), "%s", text);
        rendered_text_count++;
    }
    top = (int64_t)baseline_y - height;
    if (top < INT_MIN) top = INT_MIN;
    if (top > INT_MAX) top = INT_MAX;
    ui_fill_rect(canvas, x, (int)top, width, height, color);
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

    guarded->framebuffer_size = (size_t)stride * (size_t)height;
    guarded->allocation = malloc(TEST_GUARD_SIZE * 2U +
                                  guarded->framebuffer_size);
    TEST_CHECK(guarded->allocation != NULL);
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
            TEST_CHECK(padding[index - (size_t)guarded->canvas.width * 4U] ==
                   TEST_GUARD_BYTE);
        }
    }
    for (index = 0U; index < TEST_GUARD_SIZE; ++index) {
        TEST_CHECK(guarded->allocation[index] == TEST_GUARD_BYTE);
        TEST_CHECK(guarded->allocation[TEST_GUARD_SIZE +
                                   guarded->framebuffer_size + index] ==
               TEST_GUARD_BYTE);
    }
    free(guarded->allocation);
}

static VehicleState make_state(void)
{
    VehicleState state;

    memset(&state, 0, sizeof(state));
    state.valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                       VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_HV_CURRENT_A |
                       VEHICLE_STATE_VALID_HV_POWER_W |
                       VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                       VEHICLE_STATE_VALID_ACC_X_MPS2 |
                       VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT |
                       VEHICLE_STATE_VALID_DRIVE_STATE;
    state.speed_kmh = 86.0;
    state.motor_rpm = 8200.0;
    state.throttle_percent = 63.0;
    state.brake_percent = 18.0;
    state.hv_voltage_v = 398.0;
    state.hv_current_a = 81.0;
    state.hv_power_w = 32200.0;
    state.hv_soc_percent = 72.0;
    state.acc_x = 1.2 * 9.80665;
    state.acc_y = -0.4 * 9.80665;
    state.drive_state = VEHICLE_DRIVE_STATE_READY;
    state.can_valid = true;
    state.gps_valid = true;
    state.gps_fix_type = 4U;
    state.lap_valid = true;
    state.lap_number = 3U;
    state.lap_time_s = 62.314;
    state.best_lap_time_s = 61.900;
    state.delta_time_s = 0.214;
    state.accel_50m_time_s = 3.82;
    state.accel_time_s = 5.41;
    return state;
}

static void assert_text_has_no_invalid_tokens(void)
{
    size_t index;

    for (index = 0U; index < rendered_text_count; ++index) {
        TEST_CHECK(strstr(rendered_text[index], "nan") == NULL);
        TEST_CHECK(strstr(rendered_text[index], "inf") == NULL);
        TEST_CHECK(strstr(rendered_text[index], "-2147483648") == NULL);
    }
}

static bool rendered_text_call_found(const char *text, int x, int baseline_y)
{
    size_t index;

    for (index = 0U; index < rendered_text_count; ++index) {
        if (strcmp(rendered_text[index], text) == 0 &&
            rendered_text_x[index] == x &&
            rendered_text_baseline_y[index] == baseline_y) {
            return true;
        }
    }
    return false;
}

static bool rendered_text_found(const char *text)
{
    size_t index;

    for (index = 0U; index < rendered_text_count; ++index) {
        if (strcmp(rendered_text[index], text) == 0) return true;
    }
    return false;
}

static void test_layout_constants(void)
{
    HudLayout layout;
    const HudRect *rects[] = {
        &layout.lap, &layout.accel_event, &layout.main, &layout.speed,
        &layout.g_meter, &layout.vehicle_info
    };
    size_t index;

    hud_ui_layout_default(&layout);
    TEST_CHECK(layout.lap.x == 10 && layout.lap.y == 25 &&
           layout.lap.w == 160 && layout.lap.h == 170);
    TEST_CHECK(layout.accel_event.x == 10 && layout.accel_event.y == 205 &&
           layout.accel_event.w == 160 && layout.accel_event.h == 80);
    TEST_CHECK(layout.main.x == 115 && layout.main.y == 450 &&
           layout.main.w == 640 && layout.main.h == 140);
    TEST_CHECK(layout.main_div_1_x == 220 && layout.main_div_2_x == 435 &&
           layout.main_div_3_x == 570);
    TEST_CHECK(layout.main_div_y1 == 460 && layout.main_div_y2 == 580);
    TEST_CHECK(layout.pedal_bar_x == 240 && layout.throttle_bar_y == 485 &&
           layout.brake_bar_y == 525);
    TEST_CHECK(layout.rpm_bar_x == 240 && layout.rpm_bar_y == 565);
    TEST_CHECK(layout.pedal_segment_count == 16 &&
           layout.pedal_segment_width == 6 &&
           layout.pedal_segment_gap == 2 && layout.pedal_segment_height == 10);
    TEST_CHECK(layout.rpm_segment_count == 20 && layout.rpm_segment_width == 4 &&
           layout.rpm_segment_gap == 2 && layout.rpm_segment_height == 9);
    TEST_CHECK(layout.g_center_x == 502 && layout.g_center_y == 528 &&
           layout.g_radius == 32);
    TEST_CHECK(layout.rpm_max == 10000);
    for (index = 0U; index < sizeof(rects) / sizeof(rects[0]); ++index) {
        TEST_CHECK(rects[index]->x >= 0 && rects[index]->y >= 0);
        TEST_CHECK(rects[index]->w > 0 && rects[index]->h > 0);
        TEST_CHECK(rects[index]->x + rects[index]->w <= TEST_WIDTH);
        TEST_CHECK(rects[index]->y + rects[index]->h <= TEST_HEIGHT);
    }
}

static void test_render_values_and_stride(HudContext *context)
{
    GuardedCanvas guarded;
    VehicleState state = make_state();
    static const double speeds[] = { 0.0, 8.0, 86.0, 128.0, 999.0 };
    static const double rpms[] = { 0.0, 800.0, 8200.0, 10000.0, 12000.0 };
    static const double voltages[] = { 0.0, 398.0, 450.0, 12.0, 999.0 };
    static const double powers_kw[] = { 0.0, 9.5, 32.5, 100.0, 1.0 };
    static const double deltas[] = { -9.999, -0.214, 0.0, 0.214, 9.999 };
    int iteration;

    guarded_canvas_init(&guarded, TEST_WIDTH, TEST_HEIGHT,
                        TEST_WIDTH * 4 + 64);
    rendered_text_count = 0U;
    TEST_CHECK(hud_render(context, &guarded.canvas, &state, false) == 0);
    TEST_CHECK(pixel_color(&guarded.canvas, 40, 40) != UI_COLOR_TRANSPARENT);
    TEST_CHECK(pixel_color(&guarded.canvas, 130, 500) != UI_COLOR_TRANSPARENT);
    TEST_CHECK(rendered_text_count > 0U);
    TEST_CHECK(rendered_text_call_found("3", 20, 96));
    TEST_CHECK(rendered_text_call_found("3.82 s", 102, 242));
    TEST_CHECK(rendered_text_call_found("86", 152, 521));
    TEST_CHECK(rendered_text_call_found("63%", 396, 492));
    TEST_CHECK(rendered_text_call_found("18%", 396, 532));
    TEST_CHECK(rendered_text_call_found("8200", 388, 572));
    assert_text_has_no_invalid_tokens();

    for (iteration = 0; iteration < 5; ++iteration) {
        state.speed_kmh = speeds[iteration];
        state.motor_rpm = rpms[iteration];
        state.hv_voltage_v = voltages[iteration];
        state.hv_power_w = powers_kw[iteration] * 1000.0;
        state.delta_time_s = deltas[iteration];
        state.throttle_percent = speeds[iteration] / 10.0;
        state.brake_percent = 100.0 - state.throttle_percent;
        TEST_CHECK(hud_render(context, &guarded.canvas, &state,
                          iteration == 4) == 0);
    }
    assert_text_has_no_invalid_tokens();
    guarded_canvas_close(&guarded);
}

static void test_g_direction_history(HudContext *context)
{
    GuardedCanvas guarded;
    VehicleState state = make_state();

    guarded_canvas_init(&guarded, TEST_WIDTH, TEST_HEIGHT,
                        TEST_WIDTH * 4 + 64);
    state.acc_x = 1.25 * 9.80665;
    state.acc_y = -0.35 * 9.80665;
    rendered_text_count = 0U;
    TEST_CHECK(hud_render(context, &guarded.canvas, &state, false) == 0);
    TEST_CHECK(rendered_text_found("+1.25"));
    TEST_CHECK(rendered_text_found("-0.35"));
    TEST_CHECK(rendered_text_found("--"));
    TEST_CHECK(rendered_text_call_found("+1.25", 482, 490));
    TEST_CHECK(rendered_text_call_found("-0.35", 431, 532));

    state.acc_x = -0.75 * 9.80665;
    state.acc_y = 0.65 * 9.80665;
    rendered_text_count = 0U;
    TEST_CHECK(hud_render(context, &guarded.canvas, &state, false) == 0);
    TEST_CHECK(rendered_text_found("+1.25"));
    TEST_CHECK(rendered_text_found("-0.75"));
    TEST_CHECK(rendered_text_found("-0.35"));
    TEST_CHECK(rendered_text_found("+0.65"));
    TEST_CHECK(rendered_text_call_found("+1.25", 482, 490));
    TEST_CHECK(rendered_text_call_found("-0.75", 482, 576));
    TEST_CHECK(rendered_text_call_found("-0.35", 431, 532));
    TEST_CHECK(rendered_text_call_found("+0.65", 533, 532));

    state.acc_x = 0.0;
    state.acc_y = 0.0;
    rendered_text_count = 0U;
    TEST_CHECK(hud_render(context, &guarded.canvas, &state, false) == 0);
    TEST_CHECK(rendered_text_found("+1.25"));
    TEST_CHECK(rendered_text_found("-0.75"));
    TEST_CHECK(rendered_text_found("-0.35"));
    TEST_CHECK(rendered_text_found("+0.65"));
    guarded_canvas_close(&guarded);
}

static void test_extreme_geometry_clipping(void)
{
    GuardedCanvas guarded;

    guarded_canvas_init(&guarded, TEST_WIDTH, TEST_HEIGHT,
                        TEST_WIDTH * 4 + 64);
    ui_fill_rect(&guarded.canvas, -1000, -1000, 2000, 2000, UI_COLOR_WHITE);
    ui_line(&guarded.canvas, -500, -500, 1500, 1000, 4, UI_COLOR_BLACK);
    ui_circle(&guarded.canvas, -20, 50, 100, 4, UI_COLOR_WHITE);
    ui_fill_circle(&guarded.canvas, 799, 599, 100, UI_COLOR_BLACK);
    ui_round_rect(&guarded.canvas, -50, -50, 200, 100, 20, 3,
                  UI_COLOR_WHITE);
    ui_fill_round_rect(&guarded.canvas, 750, 550, 300, 200, 40,
                       UI_COLOR_BLACK);
    guarded_canvas_close(&guarded);
}

static void test_invalid_values_and_accel_visibility(HudContext *context)
{
    GuardedCanvas guarded;
    VehicleState invalid;

    memset(&invalid, 0, sizeof(invalid));
    invalid.valid_mask = UINT32_MAX;
    invalid.lap_valid = true;
    invalid.speed_kmh = NAN;
    invalid.motor_rpm = INFINITY;
    invalid.hv_voltage_v = -INFINITY;
    invalid.hv_current_a = NAN;
    invalid.hv_power_w = NAN;
    invalid.hv_soc_percent = INFINITY;
    invalid.throttle_percent = -100.0;
    invalid.brake_percent = 10000.0;
    invalid.lap_time_s = NAN;
    invalid.delta_time_s = INFINITY;
    invalid.acc_x = NAN;
    invalid.acc_y = NAN;
    invalid.accel_50m_time_s = NAN;
    invalid.accel_time_s = INFINITY;

    guarded_canvas_init(&guarded, TEST_WIDTH, TEST_HEIGHT,
                        TEST_WIDTH * 4 + 64);
    rendered_text_count = 0U;
    TEST_CHECK(hud_render(context, &guarded.canvas, &invalid, false) == 0);
    assert_text_has_no_invalid_tokens();

    invalid = make_state();
    invalid.accel_50m_time_s = 0.0;
    invalid.accel_time_s = 0.0;
    TEST_CHECK(hud_render(context, &guarded.canvas, &invalid, false) == 0);
    TEST_CHECK(pixel_color(&guarded.canvas, 20, 215) != UI_COLOR_TRANSPARENT);
    guarded_canvas_close(&guarded);
}

int main(void)
{
    HudContext *context;

    test_layout_constants();
    test_extreme_geometry_clipping();
    TEST_CHECK(hud_init(&context, "stub-font.ttf") == 0);
    TEST_CHECK(font_load_calls == 3U);
    test_render_values_and_stride(context);
    test_g_direction_history(context);
    test_invalid_values_and_accel_visibility(context);
    hud_shutdown(context);
    puts("hud_render tests: PASS");
    return 0;
}
