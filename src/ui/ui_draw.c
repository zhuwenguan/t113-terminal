#include "ui_draw.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_BYTES_PER_PIXEL 4

static bool ui_canvas_valid(const UiCanvas *canvas)
{
    size_t stride;

    if (canvas == NULL || canvas->pixels == NULL || canvas->width <= 0 ||
        canvas->height <= 0 || canvas->stride <= 0 ||
        canvas->width > INT_MAX / UI_BYTES_PER_PIXEL ||
        canvas->stride < canvas->width * UI_BYTES_PER_PIXEL) {
        return false;
    }
    stride = (size_t)canvas->stride;
    return (size_t)canvas->height <= SIZE_MAX / stride;
}

static inline void ui_store_color(uint8_t *pixel, uint32_t color)
{
    /* T113 ARGB8888 is little-endian B,G,R,A in CPU-visible memory. */
    pixel[0] = (uint8_t)(color & UINT32_C(0xff));
    pixel[1] = (uint8_t)((color >> 8U) & UINT32_C(0xff));
    pixel[2] = (uint8_t)((color >> 16U) & UINT32_C(0xff));
    pixel[3] = (uint8_t)((color >> 24U) & UINT32_C(0xff));
}

static inline void ui_put_pixel(const UiCanvas *canvas, int64_t x,
                                int64_t y, uint32_t color)
{
    uint8_t *pixel;

    if (!ui_canvas_valid(canvas) || x < 0 || y < 0 ||
        x >= (int64_t)canvas->width || y >= (int64_t)canvas->height) {
        return;
    }
    pixel = canvas->pixels + (size_t)y * (size_t)canvas->stride +
            (size_t)x * UI_BYTES_PER_PIXEL;
    ui_store_color(pixel, color);
}

static void ui_fill_span(const UiCanvas *canvas, int64_t y, int64_t left,
                         int64_t right, uint32_t color)
{
    int64_t x;

    if (!ui_canvas_valid(canvas) || y < 0 ||
        y >= (int64_t)canvas->height || left >= right) {
        return;
    }
    if (left < 0) left = 0;
    if (right > (int64_t)canvas->width) right = canvas->width;
    if (left >= right) return;

    for (x = left; x < right; ++x) {
        ui_put_pixel(canvas, x, y, color);
    }
}

static void ui_fill_rect_i64(const UiCanvas *canvas, int64_t x, int64_t y,
                             int64_t width, int64_t height, uint32_t color)
{
    int64_t left;
    int64_t right;
    int64_t top;
    int64_t bottom;
    int64_t row;

    if (!ui_canvas_valid(canvas) || width <= 0 || height <= 0) return;

    left = x > 0 ? x : 0;
    right = x + width < (int64_t)canvas->width ? x + width : canvas->width;
    top = y > 0 ? y : 0;
    bottom = y + height < (int64_t)canvas->height ?
             y + height : canvas->height;
    if (left >= right || top >= bottom) return;

    for (row = top; row < bottom; ++row) {
        ui_fill_span(canvas, row, left, right, color);
    }
}

static void ui_rect_i64(const UiCanvas *canvas, int64_t x, int64_t y,
                        int64_t width, int64_t height, int64_t thickness,
                        uint32_t color)
{
    int64_t max_thickness;

    if (!ui_canvas_valid(canvas) || width <= 0 || height <= 0 ||
        thickness <= 0) {
        return;
    }
    max_thickness = width < height ? width : height;
    if (thickness > max_thickness) thickness = max_thickness;

    ui_fill_rect_i64(canvas, x, y, width, thickness, color);
    ui_fill_rect_i64(canvas, x, y + height - thickness, width, thickness,
                     color);
    if (height > 2 * thickness) {
        ui_fill_rect_i64(canvas, x, y + thickness, thickness,
                         height - 2 * thickness, color);
        ui_fill_rect_i64(canvas, x + width - thickness, y + thickness,
                         thickness, height - 2 * thickness, color);
    }
}

static uint64_t ui_isqrt(uint64_t value, uint64_t upper_bound)
{
    uint64_t low = 0U;
    uint64_t high = upper_bound;

    while (low <= high) {
        uint64_t middle = low + (high - low) / 2U;

        if (middle * middle <= value) {
            low = middle + 1U;
        } else {
            high = middle - 1U;
        }
    }
    return high;
}

static bool ui_circle_span(int64_t cx, int64_t cy, int64_t radius,
                           int64_t row, int64_t *left, int64_t *right)
{
    int64_t dy;
    uint64_t radius_squared;
    uint64_t dy_squared;
    int64_t span;

    dy = row - cy;
    if (dy < -radius || dy > radius) return false;
    radius_squared = (uint64_t)radius * (uint64_t)radius;
    dy_squared = (uint64_t)(dy < 0 ? -dy : dy) *
                 (uint64_t)(dy < 0 ? -dy : dy);
    span = (int64_t)ui_isqrt(radius_squared - dy_squared,
                             (uint64_t)radius);
    *left = cx - span;
    *right = cx + span + 1;
    return true;
}

static void ui_circle_band(const UiCanvas *canvas, int64_t cx, int64_t cy,
                           int64_t outer_radius, int64_t inner_radius,
                           uint32_t color)
{
    int64_t top;
    int64_t bottom;
    int64_t row;

    if (!ui_canvas_valid(canvas) || outer_radius <= 0) return;
    top = cy - outer_radius > 0 ? cy - outer_radius : 0;
    bottom = cy + outer_radius + 1 < (int64_t)canvas->height ?
             cy + outer_radius + 1 : canvas->height;
    if (top >= bottom) return;

    for (row = top; row < bottom; ++row) {
        int64_t outer_left;
        int64_t outer_right;
        int64_t inner_left;
        int64_t inner_right;

        if (!ui_circle_span(cx, cy, outer_radius, row, &outer_left,
                            &outer_right)) {
            continue;
        }
        if (inner_radius > 0 &&
            ui_circle_span(cx, cy, inner_radius, row, &inner_left,
                           &inner_right)) {
            ui_fill_span(canvas, row, outer_left, inner_left, color);
            ui_fill_span(canvas, row, inner_right, outer_right, color);
        } else {
            ui_fill_span(canvas, row, outer_left, outer_right, color);
        }
    }
}

static bool ui_clip_line(long double min_x, long double max_x,
                         long double min_y, long double max_y,
                         int64_t *x0, int64_t *y0, int64_t *x1,
                         int64_t *y1)
{
    long double start = 0.0L;
    long double end = 1.0L;
    long double dx = (long double)*x1 - (long double)*x0;
    long double dy = (long double)*y1 - (long double)*y0;
    const long double p[4] = { -dx, dx, -dy, dy };
    const long double q[4] = {
        (long double)*x0 - min_x,
        max_x - (long double)*x0,
        (long double)*y0 - min_y,
        max_y - (long double)*y0
    };
    unsigned int index;

    for (index = 0U; index < 4U; ++index) {
        long double ratio;

        if (p[index] == 0.0L) {
            if (q[index] < 0.0L) return false;
            continue;
        }
        ratio = q[index] / p[index];
        if (p[index] < 0.0L) {
            if (ratio > end) return false;
            if (ratio > start) start = ratio;
        } else {
            if (ratio < start) return false;
            if (ratio < end) end = ratio;
        }
    }
    if (start > end) return false;

    *x0 = (int64_t)((long double)*x0 + start * dx +
                    ((long double)*x0 + start * dx >= 0.0L ? 0.5L : -0.5L));
    *y0 = (int64_t)((long double)*y0 + start * dy +
                    ((long double)*y0 + start * dy >= 0.0L ? 0.5L : -0.5L));
    *x1 = (int64_t)((long double)*x1 + end * dx +
                    ((long double)*x1 + end * dx >= 0.0L ? 0.5L : -0.5L));
    *y1 = (int64_t)((long double)*y1 + end * dy +
                    ((long double)*y1 + end * dy >= 0.0L ? 0.5L : -0.5L));
    if (*x0 < (int64_t)min_x) *x0 = (int64_t)min_x;
    if (*x0 > (int64_t)max_x) *x0 = (int64_t)max_x;
    if (*x1 < (int64_t)min_x) *x1 = (int64_t)min_x;
    if (*x1 > (int64_t)max_x) *x1 = (int64_t)max_x;
    if (*y0 < (int64_t)min_y) *y0 = (int64_t)min_y;
    if (*y0 > (int64_t)max_y) *y0 = (int64_t)max_y;
    if (*y1 < (int64_t)min_y) *y1 = (int64_t)min_y;
    if (*y1 > (int64_t)max_y) *y1 = (int64_t)max_y;
    return true;
}

static void ui_draw_thick_point(const UiCanvas *canvas, int64_t x,
                                int64_t y, int64_t thickness, uint32_t color)
{
    int64_t half = thickness / 2;
    int64_t left = x - half;
    int64_t right = x + thickness - half;
    int64_t top = y - half;
    int64_t bottom = y + thickness - half;
    int64_t row;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)canvas->width) right = canvas->width;
    if (bottom > (int64_t)canvas->height) bottom = canvas->height;
    for (row = top; row < bottom; ++row) {
        ui_fill_span(canvas, row, left, right, color);
    }
}

static int64_t ui_round_radius(int64_t width, int64_t height,
                               int64_t radius)
{
    int64_t limit;

    if (radius <= 0) return 0;
    limit = (width / 2) < (height / 2) ? width / 2 : height / 2;
    return radius < limit ? radius : limit;
}

static bool ui_round_rect_span(int64_t x, int64_t y, int64_t width,
                               int64_t height, int64_t radius, int64_t row,
                               int64_t *left, int64_t *right)
{
    int64_t inset = 0;

    if (width <= 0 || height <= 0 || row < y || row >= y + height) {
        return false;
    }
    radius = ui_round_radius(width, height, radius);
    if (radius > 0 && row < y + radius) {
        int64_t dy = y + radius - row;
        uint64_t inside = (uint64_t)radius * (uint64_t)radius -
                          (uint64_t)dy * (uint64_t)dy;

        inset = radius - (int64_t)ui_isqrt(inside, (uint64_t)radius);
    } else if (radius > 0 && row >= y + height - radius) {
        int64_t dy = row - (y + height - radius - 1);
        uint64_t inside = (uint64_t)radius * (uint64_t)radius -
                          (uint64_t)dy * (uint64_t)dy;

        inset = radius - (int64_t)ui_isqrt(inside, (uint64_t)radius);
    }
    /* Keep every scanline non-empty, including a rectangle whose minimum
     * dimension is exactly twice the requested radius. */
    if (inset * 2 >= width) inset = (width - 1) / 2;
    *left = x + inset;
    *right = x + width - inset;
    return *left < *right;
}

static void ui_fill_round_rect_i64(const UiCanvas *canvas, int64_t x,
                                   int64_t y, int64_t width, int64_t height,
                                   int64_t radius, uint32_t color)
{
    int64_t top;
    int64_t bottom;
    int64_t row;

    if (!ui_canvas_valid(canvas) || width <= 0 || height <= 0) return;
    radius = ui_round_radius(width, height, radius);
    if (radius <= 0) {
        ui_fill_rect_i64(canvas, x, y, width, height, color);
        return;
    }
    top = y > 0 ? y : 0;
    bottom = y + height < (int64_t)canvas->height ?
             y + height : canvas->height;
    for (row = top; row < bottom; ++row) {
        int64_t left;
        int64_t right;

        if (ui_round_rect_span(x, y, width, height, radius, row, &left,
                               &right)) {
            ui_fill_span(canvas, row, left, right, color);
        }
    }
}

static void ui_round_rect_i64(const UiCanvas *canvas, int64_t x, int64_t y,
                              int64_t width, int64_t height, int64_t radius,
                              int64_t thickness, uint32_t color)
{
    int64_t outer_radius;
    int64_t top;
    int64_t bottom;
    int64_t row;

    if (!ui_canvas_valid(canvas) || width <= 0 || height <= 0 ||
        thickness <= 0) {
        return;
    }
    outer_radius = ui_round_radius(width, height, radius);
    if (outer_radius <= 0) {
        ui_rect_i64(canvas, x, y, width, height, thickness, color);
        return;
    }
    if (thickness > width) thickness = width;
    if (thickness > height) thickness = height;

    top = y > 0 ? y : 0;
    bottom = y + height < (int64_t)canvas->height ?
             y + height : canvas->height;
    for (row = top; row < bottom; ++row) {
        int64_t outer_left;
        int64_t outer_right;
        bool has_inner = width > 2 * thickness && height > 2 * thickness;

        if (!ui_round_rect_span(x, y, width, height, outer_radius, row,
                                &outer_left, &outer_right)) {
            continue;
        }
        if (has_inner) {
            int64_t inner_left;
            int64_t inner_right;
            int64_t inner_radius = outer_radius > thickness ?
                                   outer_radius - thickness : 0;

            if (ui_round_rect_span(x + thickness, y + thickness,
                                   width - 2 * thickness,
                                   height - 2 * thickness, inner_radius, row,
                                   &inner_left, &inner_right)) {
                ui_fill_span(canvas, row, outer_left, inner_left, color);
                ui_fill_span(canvas, row, inner_right, outer_right, color);
                continue;
            }
        }
        ui_fill_span(canvas, row, outer_left, outer_right, color);
    }
}

void ui_clear(UiCanvas *canvas, uint32_t color)
{
    int row;

    if (!ui_canvas_valid(canvas)) return;
    for (row = 0; row < canvas->height; ++row) {
        ui_fill_span(canvas, row, 0, canvas->width, color);
    }
}

void ui_fill_rect(UiCanvas *canvas, int x, int y, int width, int height,
                  uint32_t color)
{
    ui_fill_rect_i64(canvas, (int64_t)x, (int64_t)y, (int64_t)width,
                     (int64_t)height, color);
}

void ui_rect(UiCanvas *canvas, int x, int y, int width, int height,
             int thickness, uint32_t color)
{
    ui_rect_i64(canvas, (int64_t)x, (int64_t)y, (int64_t)width,
                (int64_t)height, (int64_t)thickness, color);
}

void ui_line(UiCanvas *canvas, int x0, int y0, int x1, int y1,
             int thickness, uint32_t color)
{
    int64_t start_x = x0;
    int64_t start_y = y0;
    int64_t end_x = x1;
    int64_t end_y = y1;
    int64_t margin;
    int64_t dx;
    int64_t dy;
    int64_t step_x;
    int64_t step_y;
    int64_t error;
    int64_t thick;

    if (!ui_canvas_valid(canvas) || thickness <= 0) return;
    thick = thickness;
    if (thick > canvas->width && thick > canvas->height) {
        thick = canvas->width > canvas->height ? canvas->width : canvas->height;
    }
    margin = thick;
    if (!ui_clip_line(-((long double)margin),
                      (long double)canvas->width - 1.0L + margin,
                      -((long double)margin),
                      (long double)canvas->height - 1.0L + margin,
                      &start_x, &start_y, &end_x, &end_y)) {
        return;
    }

    dx = end_x >= start_x ? end_x - start_x : start_x - end_x;
    dy = end_y >= start_y ? end_y - start_y : start_y - end_y;
    step_x = start_x < end_x ? 1 : -1;
    step_y = start_y < end_y ? 1 : -1;
    error = dx - dy;
    for (;;) {
        int64_t twice_error;

        ui_draw_thick_point(canvas, start_x, start_y, thick, color);
        if (start_x == end_x && start_y == end_y) break;
        twice_error = 2 * error;
        if (twice_error > -dy) {
            error -= dy;
            start_x += step_x;
        }
        if (twice_error < dx) {
            error += dx;
            start_y += step_y;
        }
    }
}

void ui_fill_circle(UiCanvas *canvas, int cx, int cy, int radius,
                    uint32_t color)
{
    if (radius <= 0) return;
    ui_circle_band(canvas, (int64_t)cx, (int64_t)cy, (int64_t)radius, 0,
                   color);
}

void ui_circle(UiCanvas *canvas, int cx, int cy, int radius, int thickness,
               uint32_t color)
{
    int64_t inner_radius;

    if (radius <= 0 || thickness <= 0) return;
    inner_radius = (int64_t)radius - (int64_t)thickness;
    ui_circle_band(canvas, (int64_t)cx, (int64_t)cy, (int64_t)radius,
                   inner_radius, color);
}

void ui_fill_round_rect(UiCanvas *canvas, int x, int y, int width, int height,
                        int radius, uint32_t color)
{
    ui_fill_round_rect_i64(canvas, (int64_t)x, (int64_t)y, (int64_t)width,
                           (int64_t)height, (int64_t)radius, color);
}

void ui_round_rect(UiCanvas *canvas, int x, int y, int width, int height,
                   int radius, int thickness, uint32_t color)
{
    ui_round_rect_i64(canvas, (int64_t)x, (int64_t)y, (int64_t)width,
                      (int64_t)height, (int64_t)radius, (int64_t)thickness,
                      color);
}
