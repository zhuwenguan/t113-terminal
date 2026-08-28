#ifndef T113_UI_FONT_H
#define T113_UI_FONT_H

#include "ui_draw.h"

#include <stdint.h>

typedef struct UiFont UiFont;

/* Loads one persistent font face at one pixel size. */
UiFont *ui_font_load(const char *path, int pixel_size);
void ui_font_free(UiFont *font);

/* x is the left edge of the logical text run; y is the text baseline. */
/* Returns 0 on success and -1 if the font, canvas, or text is invalid. */
int ui_text(UiCanvas *canvas, UiFont *font, int x, int baseline_y,
            uint32_t color, const char *text);

/* Returns the logical line width and height in pixels. */
int ui_text_measure(UiFont *font, const char *text, int *width, int *height);

#endif
