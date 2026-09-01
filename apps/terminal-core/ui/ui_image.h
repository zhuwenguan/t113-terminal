#ifndef T113_UI_IMAGE_H
#define T113_UI_IMAGE_H

#include "ui_draw.h"

#include <stdint.h>

typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    int stride;
} UiImage;

/* Loads once and stores pixels as T113 ARGB8888 B,G,R,A. */
UiImage *ui_image_load_png(const char *path);

/* Draws at native size.  Transparent source pixels are skipped. */
void ui_image_draw(UiCanvas *canvas, const UiImage *image, int x, int y);

void ui_image_free(UiImage *image);

#endif
