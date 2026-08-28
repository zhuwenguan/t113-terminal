#define _POSIX_C_SOURCE 200809L

#include "hud_assets.h"

#include "tiny_png.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

typedef struct {
    const char *name;
    uint32_t width;
    uint32_t height;
} HudAssetSpec;

static const HudAssetSpec hud_asset_specs[HUD_ASSET_COUNT] = {
    [HUD_ASSET_STATIC_FRAME] = { "hud_static_frame.png", 800U, 600U },
    [HUD_ASSET_LEFT_TOP_PANEL] = { "left_top_panel.png", 220U, 120U },
    [HUD_ASSET_RIGHT_TOP_PANEL] = { "right_top_panel.png", 220U, 120U },
    [HUD_ASSET_RPM_ARC_BG] = { "rpm_arc_bg.png", 520U, 170U },
    [HUD_ASSET_RPM_ARC_FILL] = { "rpm_arc_fill.png", 520U, 170U },
    [HUD_ASSET_CENTER_GEAR_FRAME] = { "center_gear_frame.png", 160U, 110U },
    [HUD_ASSET_G_METER_BG] = { "g_meter_bg.png", 160U, 160U },
    [HUD_ASSET_G_METER_DOT] = { "g_meter_dot.png", 32U, 32U },
    [HUD_ASSET_PEDAL_BAR_BG] = { "pedal_bar_bg.png", 120U, 180U },
    [HUD_ASSET_PEDAL_BAR_FILL_BLUE] = {
        "pedal_bar_fill_blue.png", 120U, 180U
    },
    [HUD_ASSET_PEDAL_BAR_FILL_RED] = {
        "pedal_bar_fill_red.png", 120U, 180U
    },
    [HUD_ASSET_WARNING_ICON_SET] = {
        "warning_icon_set.png", 384U, 64U
    },
    [HUD_ASSET_SMALL_LABEL_PLATE] = {
        "small_label_plate.png", 120U, 32U
    },
    [HUD_ASSET_NUMBER_HIGHLIGHT_PLATE] = {
        "number_highlight_plate.png", 160U, 60U
    },
    [HUD_ASSET_STATUS_PILL] = { "status_pill.png", 100U, 28U }
};

const char *hud_asset_name(HudAssetId id)
{
    return id < HUD_ASSET_COUNT ? hud_asset_specs[id].name : "unknown.png";
}

static bool hud_assets_join_path(char *destination, size_t destination_size,
                                 const char *directory, const char *name)
{
    size_t directory_length;
    const char *separator = "";

    if (destination == NULL || destination_size == 0U || directory == NULL ||
        name == NULL || directory[0] == '\0') {
        return false;
    }
    directory_length = strlen(directory);
    if (directory[directory_length - 1U] != '/') {
        separator = "/";
    }
    if (snprintf(destination, destination_size, "%s%s%s", directory,
                 separator, name) >= (int)destination_size) {
        return false;
    }
    return true;
}

static uint8_t hud_assets_premultiply(uint8_t value, uint8_t alpha)
{
    return (uint8_t)(((unsigned int)value * alpha + 127U) / 255U);
}

static int hud_assets_load_one(HudAssets *assets, HudAssetId id)
{
    const HudAssetSpec *spec = &hud_asset_specs[id];
    TinyPngImage decoded;
    HudImage *image = &assets->image[id];
    char path[HUD_ASSET_PATH_MAX];
    char error_message[160];
    size_t pixel_count;
    size_t pixel_index;

    if (!hud_assets_join_path(path, sizeof(path), assets->asset_dir,
                              spec->name)) {
        fprintf(stderr, "HUD PNG: %s: asset path is too long\n", spec->name);
        assets->failed_count++;
        return -1;
    }
    memset(&decoded, 0, sizeof(decoded));
    memset(error_message, 0, sizeof(error_message));
    if (tiny_png_load_rgba(path, &decoded, error_message,
                           sizeof(error_message)) < 0) {
        fprintf(stderr, "HUD PNG: %s: %s (%s)\n", spec->name,
                error_message[0] != '\0' ? error_message : "decode failed",
                path);
        assets->failed_count++;
        return -1;
    }
    if (decoded.width != spec->width || decoded.height != spec->height) {
        fprintf(stderr, "HUD PNG: %s: expected %ux%u, got %ux%u\n",
                spec->name, spec->width, spec->height, decoded.width,
                decoded.height);
        tiny_png_image_close(&decoded);
        assets->failed_count++;
        return -1;
    }
    pixel_count = (size_t)decoded.width * decoded.height;
    if (pixel_count > SIZE_MAX / 4U) {
        fprintf(stderr, "HUD PNG: %s: decoded size overflow\n", spec->name);
        tiny_png_image_close(&decoded);
        assets->failed_count++;
        return -1;
    }
    image->pixels = malloc(pixel_count * 4U);
    if (image->pixels == NULL) {
        fprintf(stderr, "HUD PNG: %s: out of memory (%zu bytes)\n",
                spec->name, pixel_count * 4U);
        tiny_png_image_close(&decoded);
        assets->failed_count++;
        return -1;
    }
    image->width = decoded.width;
    image->height = decoded.height;
    image->stride = decoded.width * 4U;
    image->bytes = pixel_count * 4U;
    image->pixel_format = assets->target_pixel_format;
    image->alpha_min = decoded.alpha_min;
    image->alpha_max = decoded.alpha_max;
    image->alpha_x_min = decoded.alpha_x_min;
    image->alpha_y_min = decoded.alpha_y_min;
    image->alpha_x_max = decoded.alpha_x_max;
    image->alpha_y_max = decoded.alpha_y_max;
    image->premultiplied = assets->target_premultiplied;
    for (pixel_index = 0U; pixel_index < pixel_count; ++pixel_index) {
        const uint8_t *source = decoded.rgba + pixel_index * 4U;
        uint8_t *destination = image->pixels + pixel_index * 4U;
        uint8_t red = source[0];
        uint8_t green = source[1];
        uint8_t blue = source[2];
        uint8_t alpha = source[3];

        if (assets->target_premultiplied) {
            red = hud_assets_premultiply(red, alpha);
            green = hud_assets_premultiply(green, alpha);
            blue = hud_assets_premultiply(blue, alpha);
        }
        if (assets->target_pixel_format == 2 /* HUD_PIXEL_FORMAT_ARGB8888 */) {
            /* On T113 little-endian ARGB8888 is stored B,G,R,A. */
            destination[0] = blue;
            destination[1] = green;
            destination[2] = red;
        } else {
            destination[0] = red;
            destination[1] = green;
            destination[2] = blue;
        }
        destination[3] = alpha;
    }
    image->loaded = true;
    assets->resident_bytes += image->bytes;
    assets->loaded_count++;
    fprintf(stderr, "HUD PNG: loaded %s %ux%u alpha=%u..%u bbox=(%u,%u)-(%u,%u) bytes=%zu\n",
            spec->name, image->width, image->height, image->alpha_min,
            image->alpha_max, image->alpha_x_min, image->alpha_y_min,
            image->alpha_x_max, image->alpha_y_max, image->bytes);
    tiny_png_image_close(&decoded);
    return 0;
}

int hud_assets_init(HudAssets *assets, const char *asset_dir,
                    int target_pixel_format, bool target_premultiplied)
{
    HudAssetId id;

    if (assets == NULL || asset_dir == NULL || asset_dir[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    memset(assets, 0, sizeof(*assets));
    if (snprintf(assets->asset_dir, sizeof(assets->asset_dir), "%s",
                 asset_dir) >= (int)sizeof(assets->asset_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    assets->target_pixel_format = target_pixel_format;
    assets->target_premultiplied = target_premultiplied;
    assets->initialized = true;
    for (id = HUD_ASSET_STATIC_FRAME; id < HUD_ASSET_COUNT; ++id) {
        (void)hud_assets_load_one(assets, id);
    }
    assets->core_ready = assets->image[HUD_ASSET_STATIC_FRAME].loaded &&
        assets->image[HUD_ASSET_LEFT_TOP_PANEL].loaded &&
        assets->image[HUD_ASSET_RIGHT_TOP_PANEL].loaded &&
        assets->image[HUD_ASSET_RPM_ARC_BG].loaded &&
        assets->image[HUD_ASSET_RPM_ARC_FILL].loaded &&
        assets->image[HUD_ASSET_CENTER_GEAR_FRAME].loaded &&
        assets->image[HUD_ASSET_G_METER_BG].loaded &&
        assets->image[HUD_ASSET_G_METER_DOT].loaded &&
        assets->image[HUD_ASSET_PEDAL_BAR_BG].loaded &&
        assets->image[HUD_ASSET_PEDAL_BAR_FILL_BLUE].loaded &&
        assets->image[HUD_ASSET_PEDAL_BAR_FILL_RED].loaded;
    fprintf(stderr, "HUD PNG: asset memory=%zu bytes, loaded=%u/%u, failed=%u, core=%s, format=%s alpha=%s\n",
            assets->resident_bytes, assets->loaded_count, HUD_ASSET_COUNT,
            assets->failed_count, assets->core_ready ? "yes" : "no",
            target_pixel_format == 2 ? "BGRA/ARGB8888" : "RGBA8888",
            target_premultiplied ? "premultiplied" : "straight");
    return 0;
}

void hud_assets_close(HudAssets *assets)
{
    HudAssetId id;

    if (assets == NULL) {
        return;
    }
    for (id = HUD_ASSET_STATIC_FRAME; id < HUD_ASSET_COUNT; ++id) {
        free(assets->image[id].pixels);
        assets->image[id].pixels = NULL;
        assets->image[id].loaded = false;
    }
    memset(assets, 0, sizeof(*assets));
}

const HudImage *hud_assets_get(const HudAssets *assets, HudAssetId id)
{
    if (assets == NULL || id >= HUD_ASSET_COUNT ||
        !assets->image[id].loaded) {
        return NULL;
    }
    return &assets->image[id];
}

size_t hud_assets_resident_bytes(const HudAssets *assets)
{
    return assets != NULL ? assets->resident_bytes : 0U;
}

bool hud_assets_core_ready(const HudAssets *assets)
{
    return assets != NULL && assets->initialized && assets->core_ready;
}
