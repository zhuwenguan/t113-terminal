#ifndef T113_HUD_ASSETS_H
#define T113_HUD_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HUD_ASSET_PATH_MAX 512U

typedef enum {
    HUD_ASSET_STATIC_FRAME = 0,
    HUD_ASSET_LEFT_TOP_PANEL,
    HUD_ASSET_RIGHT_TOP_PANEL,
    HUD_ASSET_RPM_ARC_BG,
    HUD_ASSET_RPM_ARC_FILL,
    HUD_ASSET_CENTER_GEAR_FRAME,
    HUD_ASSET_G_METER_BG,
    HUD_ASSET_G_METER_DOT,
    HUD_ASSET_PEDAL_BAR_BG,
    HUD_ASSET_PEDAL_BAR_FILL_BLUE,
    HUD_ASSET_PEDAL_BAR_FILL_RED,
    HUD_ASSET_WARNING_ICON_SET,
    HUD_ASSET_SMALL_LABEL_PLATE,
    HUD_ASSET_NUMBER_HIGHLIGHT_PLATE,
    HUD_ASSET_STATUS_PILL,
    HUD_ASSET_COUNT
} HudAssetId;

typedef struct {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t bytes;
    int pixel_format;
    uint8_t alpha_min;
    uint8_t alpha_max;
    uint32_t alpha_x_min;
    uint32_t alpha_y_min;
    uint32_t alpha_x_max;
    uint32_t alpha_y_max;
    bool loaded;
    bool premultiplied;
} HudImage;

typedef struct {
    char asset_dir[HUD_ASSET_PATH_MAX];
    int target_pixel_format;
    bool target_premultiplied;
    HudImage image[HUD_ASSET_COUNT];
    size_t resident_bytes;
    uint32_t loaded_count;
    uint32_t failed_count;
    bool core_ready;
    bool initialized;
} HudAssets;

int hud_assets_init(HudAssets *assets, const char *asset_dir,
                    int target_pixel_format, bool target_premultiplied);
void hud_assets_close(HudAssets *assets);
const HudImage *hud_assets_get(const HudAssets *assets, HudAssetId id);
size_t hud_assets_resident_bytes(const HudAssets *assets);
bool hud_assets_core_ready(const HudAssets *assets);
const char *hud_asset_name(HudAssetId id);

#endif
