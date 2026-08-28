#ifndef T113_UI_DRAW_H
#define T113_UI_DRAW_H

#include <stdint.h>

/*
 * A directly writable T113 ARGB8888 framebuffer.
 *
 * width and height are pixels.  stride is the number of bytes between the
 * starts of two adjacent rows; it may be larger than width * 4.
 */
typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    int stride;
} UiCanvas;

/* Colors are represented as 0xAARRGGBB.  Pixels are stored as B,G,R,A. */
#define UI_ARGB(a, r, g, b) \
    ((uint32_t)((((uint32_t)(a) & UINT32_C(0xff)) << 24) | \
                (((uint32_t)(r) & UINT32_C(0xff)) << 16) | \
                (((uint32_t)(g) & UINT32_C(0xff)) << 8) | \
                ((uint32_t)(b) & UINT32_C(0xff))))

#define UI_COLOR_TRANSPARENT UI_ARGB(0U, 0U, 0U, 0U)
#define UI_COLOR_BLACK       UI_ARGB(255U, 0U, 0U, 0U)
#define UI_COLOR_WHITE       UI_ARGB(255U, 255U, 255U, 255U)

void ui_clear(UiCanvas *canvas, uint32_t color);

void ui_line(UiCanvas *canvas, int x0, int y0, int x1, int y1,
             int thickness, uint32_t color);

void ui_rect(UiCanvas *canvas, int x, int y, int width, int height,
             int thickness, uint32_t color);

void ui_fill_rect(UiCanvas *canvas, int x, int y, int width, int height,
                  uint32_t color);

void ui_circle(UiCanvas *canvas, int cx, int cy, int radius, int thickness,
               uint32_t color);

void ui_fill_circle(UiCanvas *canvas, int cx, int cy, int radius,
                    uint32_t color);

void ui_round_rect(UiCanvas *canvas, int x, int y, int width, int height,
                   int radius, int thickness, uint32_t color);

void ui_fill_round_rect(UiCanvas *canvas, int x, int y, int width, int height,
                        int radius, uint32_t color);

#endif
