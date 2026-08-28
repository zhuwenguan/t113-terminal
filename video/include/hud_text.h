#ifndef T113_HUD_TEXT_H
#define T113_HUD_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HudLayer HudLayer;

/* The cache is deliberately fixed-size so normal HUD frames do not grow a
 * container or allocate a new entry.  It is large enough for the common HUD
 * character set at the small, value, speed, RPM and gear production sizes. */
#define HUD_TEXT_CACHE_CAPACITY 384U
#define HUD_TEXT_PATH_MAX 512U

typedef struct {
    uint32_t pixel_size;
    uint32_t codepoint;
    uint8_t *bitmap;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    int bitmap_left;
    int bitmap_top;
    int32_t advance_x_26_6;
    uint64_t last_used;
    bool occupied;
} HudTextGlyph;

typedef struct {
    /* Stored as opaque pointers here so users of the HUD header do not need
     * to include FreeType headers.  hud_text.c owns the FT lifecycle. */
    void *library;
    void *face;
    HudTextGlyph cache[HUD_TEXT_CACHE_CAPACITY];
    uint64_t use_counter;
    size_t bytes;
    uint32_t cached_glyphs;
    uint64_t cache_hits;
    uint64_t cache_misses;
    char font_path[HUD_TEXT_PATH_MAX];
    bool initialized;
} HudTextAtlas;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} HudTextColor;

typedef enum {
    HUD_ALIGN_LEFT = 0,
    HUD_ALIGN_CENTER = 1,
    HUD_ALIGN_RIGHT = 2
} HudTextAlign;

typedef struct {
    uint32_t width;
    uint32_t height;
    int ascent;
    int descent;
} HudTextMetrics;

/* y passed to hud_text_draw() is a baseline, not a bitmap top edge. */
int hud_text_atlas_init(HudTextAtlas *atlas, const char *font_path);
void hud_text_atlas_close(HudTextAtlas *atlas);

/* Warm the cache during renderer initialization.  A cache miss during
 * drawing remains safe and is retained for future frames. */
bool hud_text_atlas_warm(HudTextAtlas *atlas,
                         const uint32_t *pixel_sizes, size_t size_count);

bool hud_text_measure(HudTextAtlas *atlas, const char *text,
                      uint32_t pixel_size, HudTextMetrics *metrics);
uint32_t hud_text_width(HudTextAtlas *atlas, const char *text,
                        uint32_t pixel_size);
size_t hud_text_cache_bytes(const HudTextAtlas *atlas);
uint32_t hud_text_cached_glyph_count(const HudTextAtlas *atlas);

bool hud_text_draw(HudTextAtlas *atlas, HudLayer *layer, int x, int baseline_y,
                   uint32_t pixel_size, HudTextColor color,
                   HudTextAlign align, const char *text);

#endif
