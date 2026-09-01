#define _POSIX_C_SOURCE 200809L

#include "hud_text.h"

#include "hud_blend.h"
#include "hud_renderer.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HUD_TEXT_FIXED_POINT_SHIFT 6
#define HUD_TEXT_FIXED_POINT_ONE (1LL << HUD_TEXT_FIXED_POINT_SHIFT)

static int hud_text_floor_26_6(int64_t value)
{
    if (value >= 0) return (int)(value / HUD_TEXT_FIXED_POINT_ONE);
    return (int)(-((-value + HUD_TEXT_FIXED_POINT_ONE - 1) /
                   HUD_TEXT_FIXED_POINT_ONE));
}

static uint32_t hud_text_ceil_positive_26_6(int64_t value)
{
    if (value <= 0) return 0U;
    if ((uint64_t)value >
        (uint64_t)UINT32_MAX * HUD_TEXT_FIXED_POINT_ONE) {
        return UINT32_MAX;
    }
    return (uint32_t)((value + HUD_TEXT_FIXED_POINT_ONE - 1) /
                      HUD_TEXT_FIXED_POINT_ONE);
}

static bool hud_text_decode_utf8(const char *text, uint32_t *codepoint,
                                 size_t *consumed)
{
    const unsigned char *bytes = (const unsigned char *)text;
    uint32_t value;
    size_t length;

    if (text == NULL || codepoint == NULL || consumed == NULL) {
        return false;
    }
    length = strlen(text);
    if (length == 0U) return false;
    if (bytes[0] < 0x80U) {
        *codepoint = bytes[0];
        *consumed = 1U;
        return true;
    }
    if (length >= 2U && bytes[0] >= 0xc2U && bytes[0] <= 0xdfU &&
        (bytes[1] & 0xc0U) == 0x80U) {
        *codepoint = ((uint32_t)(bytes[0] & 0x1fU) << 6U) |
                     (uint32_t)(bytes[1] & 0x3fU);
        *consumed = 2U;
        return true;
    }
    if (length >= 3U && bytes[0] >= 0xe0U && bytes[0] <= 0xefU &&
        (bytes[1] & 0xc0U) == 0x80U &&
        (bytes[2] & 0xc0U) == 0x80U &&
        !(bytes[0] == 0xe0U && bytes[1] < 0xa0U) &&
        !(bytes[0] == 0xedU && bytes[1] >= 0xa0U)) {
        value = ((uint32_t)(bytes[0] & 0x0fU) << 12U) |
                ((uint32_t)(bytes[1] & 0x3fU) << 6U) |
                (uint32_t)(bytes[2] & 0x3fU);
        *codepoint = value;
        *consumed = 3U;
        return true;
    }
    if (length >= 4U && bytes[0] >= 0xf0U && bytes[0] <= 0xf4U &&
        (bytes[1] & 0xc0U) == 0x80U &&
        (bytes[2] & 0xc0U) == 0x80U &&
        (bytes[3] & 0xc0U) == 0x80U &&
        !(bytes[0] == 0xf0U && bytes[1] < 0x90U) &&
        !(bytes[0] == 0xf4U && bytes[1] >= 0x90U)) {
        value = ((uint32_t)(bytes[0] & 0x07U) << 18U) |
                ((uint32_t)(bytes[1] & 0x3fU) << 12U) |
                ((uint32_t)(bytes[2] & 0x3fU) << 6U) |
                (uint32_t)(bytes[3] & 0x3fU);
        *codepoint = value;
        *consumed = 4U;
        return true;
    }
    /* Invalid UTF-8 is not allowed to make the render loop run past the
     * string.  Keep one byte of progress and use the font's replacement
     * glyph, if present. */
    *codepoint = 0xfffdU;
    *consumed = 1U;
    return true;
}

static FT_Library hud_text_library(const HudTextAtlas *atlas)
{
    return (FT_Library)atlas->library;
}

static FT_Face hud_text_face(const HudTextAtlas *atlas)
{
    return (FT_Face)atlas->face;
}

static HudTextGlyph *hud_text_find_cached(HudTextAtlas *atlas,
                                          uint32_t pixel_size,
                                          uint32_t codepoint)
{
    uint32_t index;

    for (index = 0U; index < HUD_TEXT_CACHE_CAPACITY; ++index) {
        HudTextGlyph *glyph = &atlas->cache[index];

        if (glyph->occupied && glyph->pixel_size == pixel_size &&
            glyph->codepoint == codepoint) {
            glyph->last_used = ++atlas->use_counter;
            atlas->cache_hits++;
            return glyph;
        }
    }
    return NULL;
}

static HudTextGlyph *hud_text_select_cache_slot(HudTextAtlas *atlas)
{
    HudTextGlyph *selected = NULL;
    uint32_t index;

    for (index = 0U; index < HUD_TEXT_CACHE_CAPACITY; ++index) {
        HudTextGlyph *glyph = &atlas->cache[index];

        if (!glyph->occupied) return glyph;
        if (selected == NULL || glyph->last_used < selected->last_used) {
            selected = glyph;
        }
    }
    if (selected != NULL) {
        free(selected->bitmap);
        if (atlas->bytes >= (size_t)selected->height * selected->pitch) {
            atlas->bytes -= (size_t)selected->height * selected->pitch;
        } else {
            atlas->bytes = 0U;
        }
        memset(selected, 0, sizeof(*selected));
        atlas->cached_glyphs--;
    }
    return selected;
}

static HudTextGlyph *hud_text_load_glyph(HudTextAtlas *atlas,
                                         uint32_t pixel_size,
                                         uint32_t codepoint)
{
    FT_Face face = hud_text_face(atlas);
    FT_GlyphSlot slot;
    HudTextGlyph *glyph;
    int bitmap_pitch;
    uint32_t pitch;
    size_t bytes;
    uint32_t row;

    if (FT_Set_Pixel_Sizes(face, 0U, pixel_size) != 0 ||
        FT_Load_Char(face, (FT_ULong)codepoint,
                     FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
        fprintf(stderr, "[HUD][FONT] Failed to rasterize U+%04X at %u px from %s\n",
                codepoint, pixel_size, atlas->font_path);
        return NULL;
    }
    slot = face->glyph;
    if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
        fprintf(stderr, "[HUD][FONT] Non-gray glyph U+%04X from %s\n",
                codepoint, atlas->font_path);
        return NULL;
    }
    bitmap_pitch = slot->bitmap.pitch;
    pitch = bitmap_pitch < 0 ? (uint32_t)(-bitmap_pitch) :
            (uint32_t)bitmap_pitch;
    if ((size_t)slot->bitmap.rows > SIZE_MAX / (pitch != 0U ? pitch : 1U)) {
        errno = EOVERFLOW;
        return NULL;
    }
    bytes = (size_t)slot->bitmap.rows * pitch;
    glyph = hud_text_select_cache_slot(atlas);
    if (glyph == NULL) return NULL;
    glyph->pixel_size = pixel_size;
    glyph->codepoint = codepoint;
    glyph->width = slot->bitmap.width;
    glyph->height = slot->bitmap.rows;
    glyph->pitch = pitch;
    glyph->bitmap_left = slot->bitmap_left;
    glyph->bitmap_top = slot->bitmap_top;
    glyph->advance_x_26_6 = (int32_t)slot->advance.x;
    glyph->last_used = ++atlas->use_counter;
    if (bytes != 0U) {
        if (slot->bitmap.buffer == NULL) {
            memset(glyph, 0, sizeof(*glyph));
            return NULL;
        }
        glyph->bitmap = malloc(bytes);
        if (glyph->bitmap == NULL) {
            memset(glyph, 0, sizeof(*glyph));
            return NULL;
        }
        for (row = 0U; row < glyph->height; ++row) {
            const uint8_t *source = slot->bitmap.buffer +
                (bitmap_pitch >= 0 ? (size_t)row * pitch :
                 (size_t)(glyph->height - 1U - row) * pitch);
            memcpy(glyph->bitmap + (size_t)row * pitch, source, pitch);
        }
        atlas->bytes += bytes;
    }
    glyph->occupied = true;
    atlas->cached_glyphs++;
    return glyph;
}

static HudTextGlyph *hud_text_get_glyph(HudTextAtlas *atlas,
                                        uint32_t pixel_size,
                                        uint32_t codepoint)
{
    HudTextGlyph *glyph;

    glyph = hud_text_find_cached(atlas, pixel_size, codepoint);
    if (glyph != NULL) return glyph;
    atlas->cache_misses++;
    return hud_text_load_glyph(atlas, pixel_size, codepoint);
}

static bool hud_text_set_size(HudTextAtlas *atlas, uint32_t pixel_size)
{
    if (pixel_size == 0U || pixel_size > UINT16_MAX ||
        FT_Set_Pixel_Sizes(hud_text_face(atlas), 0U, pixel_size) != 0) {
        return false;
    }
    return true;
}

static void hud_text_get_line_metrics(HudTextAtlas *atlas,
                                      uint32_t pixel_size,
                                      int *ascent, int *descent)
{
    FT_Size_Metrics metrics;

    *ascent = 0;
    *descent = 0;
    if (!hud_text_set_size(atlas, pixel_size) || hud_text_face(atlas)->size == NULL) {
        return;
    }
    metrics = hud_text_face(atlas)->size->metrics;
    *ascent = (int)hud_text_ceil_positive_26_6(metrics.ascender);
    *descent = (int)hud_text_ceil_positive_26_6(-metrics.descender);
}

int hud_text_atlas_init(HudTextAtlas *atlas, const char *font_path)
{
    FT_Library library = NULL;
    FT_Face face = NULL;
    FT_Error error;

    if (atlas == NULL || font_path == NULL || font_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    memset(atlas, 0, sizeof(*atlas));
    if (snprintf(atlas->font_path, sizeof(atlas->font_path), "%s", font_path) >=
        (int)sizeof(atlas->font_path)) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "[HUD][FONT] Font path is too long: %s\n", font_path);
        return -1;
    }
    error = FT_Init_FreeType(&library);
    if (error != 0) {
        fprintf(stderr, "[HUD][FONT] FT_Init_FreeType failed (%d) for %s\n",
                error, font_path);
        return -1;
    }
    error = FT_New_Face(library, font_path, 0, &face);
    if (error != 0) {
        fprintf(stderr, "[HUD][FONT] Failed to load %s (FreeType error %d)\n",
                font_path, error);
        FT_Done_FreeType(library);
        return -1;
    }
    error = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (error != 0) {
        fprintf(stderr, "[HUD][FONT] Unicode charmap unavailable in %s (%d)\n",
                font_path, error);
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return -1;
    }
    atlas->library = library;
    atlas->face = face;
    atlas->initialized = true;
    fprintf(stderr, "[HUD][FONT] Loaded %s (%s)\n", font_path,
            face->family_name != NULL ? face->family_name : "unknown family");
    return 0;
}

void hud_text_atlas_close(HudTextAtlas *atlas)
{
    uint32_t index;

    if (atlas == NULL) return;
    for (index = 0U; index < HUD_TEXT_CACHE_CAPACITY; ++index) {
        free(atlas->cache[index].bitmap);
    }
    if (atlas->face != NULL) FT_Done_Face(hud_text_face(atlas));
    if (atlas->library != NULL) FT_Done_FreeType(hud_text_library(atlas));
    memset(atlas, 0, sizeof(*atlas));
}

bool hud_text_atlas_warm(HudTextAtlas *atlas,
                         const uint32_t *pixel_sizes, size_t size_count)
{
    static const char common_characters[] =
        " 0123456789+-.:/%ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz\xc2\xb0";
    size_t size_index;

    if (atlas == NULL || !atlas->initialized || pixel_sizes == NULL ||
        size_count == 0U) return false;
    for (size_index = 0U; size_index < size_count; ++size_index) {
        const char *cursor = common_characters;

        if (!hud_text_set_size(atlas, pixel_sizes[size_index])) return false;
        while (*cursor != '\0') {
            uint32_t codepoint;
            size_t consumed;

            if (!hud_text_decode_utf8(cursor, &codepoint, &consumed)) break;
            if (hud_text_get_glyph(atlas, pixel_sizes[size_index], codepoint) == NULL) {
                return false;
            }
            cursor += consumed;
        }
    }
    return true;
}

bool hud_text_measure(HudTextAtlas *atlas, const char *text,
                      uint32_t pixel_size, HudTextMetrics *metrics)
{
    const char *cursor;
    int ascent;
    int descent;
    int64_t pen = 0;

    if (metrics != NULL) memset(metrics, 0, sizeof(*metrics));
    if (atlas == NULL || !atlas->initialized || text == NULL ||
        metrics == NULL || !hud_text_set_size(atlas, pixel_size)) {
        return false;
    }
    hud_text_get_line_metrics(atlas, pixel_size, &ascent, &descent);
    cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint;
        size_t consumed;
        HudTextGlyph *glyph;

        if (!hud_text_decode_utf8(cursor, &codepoint, &consumed)) break;
        glyph = hud_text_get_glyph(atlas, pixel_size, codepoint);
        if (glyph == NULL) return false;
        if ((int64_t)glyph->advance_x_26_6 > INT64_MAX - pen) return false;
        pen += glyph->advance_x_26_6;
        cursor += consumed;
    }
    metrics->width = hud_text_ceil_positive_26_6(pen);
    metrics->ascent = ascent;
    metrics->descent = descent;
    metrics->height = (uint32_t)(ascent + descent);
    return true;
}

uint32_t hud_text_width(HudTextAtlas *atlas, const char *text,
                        uint32_t pixel_size)
{
    HudTextMetrics metrics;

    return hud_text_measure(atlas, text, pixel_size, &metrics) ?
           metrics.width : 0U;
}

size_t hud_text_cache_bytes(const HudTextAtlas *atlas)
{
    return atlas != NULL ? atlas->bytes : 0U;
}

uint32_t hud_text_cached_glyph_count(const HudTextAtlas *atlas)
{
    return atlas != NULL ? atlas->cached_glyphs : 0U;
}

bool hud_text_draw(HudTextAtlas *atlas, HudLayer *layer, int x, int baseline_y,
                   uint32_t pixel_size, HudTextColor color,
                   HudTextAlign align, const char *text)
{
    HudTextMetrics metrics;
    const char *cursor;
    int64_t pen = 0;
    int alignment_offset = 0;

    if (atlas == NULL || !atlas->initialized || layer == NULL ||
        layer->pixels == NULL || text == NULL ||
        !hud_text_measure(atlas, text, pixel_size, &metrics)) {
        return false;
    }
    if (align == HUD_ALIGN_CENTER) alignment_offset = (int)metrics.width / 2;
    else if (align == HUD_ALIGN_RIGHT) alignment_offset = (int)metrics.width;
    cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint;
        size_t consumed;
        HudTextGlyph *glyph;
        int64_t glyph_x_26_6;
        int64_t glyph_x_64;
        int64_t glyph_y_64;
        int glyph_x;
        int glyph_y;

        if (!hud_text_decode_utf8(cursor, &codepoint, &consumed)) break;
        glyph = hud_text_get_glyph(atlas, pixel_size, codepoint);
        if (glyph == NULL) return false;
        glyph_x_26_6 = pen + (int64_t)glyph->bitmap_left *
                       HUD_TEXT_FIXED_POINT_ONE;
        glyph_x_64 = (int64_t)x + hud_text_floor_26_6(glyph_x_26_6) -
                     (int64_t)alignment_offset;
        glyph_y_64 = (int64_t)baseline_y - (int64_t)glyph->bitmap_top;
        glyph_x = glyph_x_64 < INT_MIN ? INT_MIN :
                  (glyph_x_64 > INT_MAX ? INT_MAX : (int)glyph_x_64);
        glyph_y = glyph_y_64 < INT_MIN ? INT_MIN :
                  (glyph_y_64 > INT_MAX ? INT_MAX : (int)glyph_y_64);
        if (glyph->bitmap != NULL && glyph->width != 0U &&
            glyph->height != 0U) {
            if (!hud_blend_coverage(layer, glyph_x, glyph_y, glyph->width,
                                    glyph->height, glyph->bitmap, glyph->pitch,
                                    color.red, color.green, color.blue,
                                    color.alpha)) {
                return false;
            }
        }
        pen += glyph->advance_x_26_6;
        cursor += consumed;
    }
    return true;
}
