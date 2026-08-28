#include "hud_renderer.h"
#include "video_frame.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static VehicleState snapshot_state(unsigned int index)
{
    VehicleState state;

    memset(&state, 0, sizeof(state));
    state.valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                       VEHICLE_STATE_VALID_ACC_X_MPS2 |
                       VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                       VEHICLE_STATE_VALID_MOTOR_RPM |
                       VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_LV_VOLTAGE_V |
                       VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                       VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                       VEHICLE_STATE_VALID_BRAKE_PERCENT |
                       VEHICLE_STATE_VALID_DRIVE_STATE;
    state.speed_kmh = 72.0;
    state.motor_rpm = 4280.0;
    state.hv_voltage_v = 356.0;
    state.lv_voltage_v = 13.6;
    state.hv_soc_percent = 78.0;
    state.throttle_percent = 68.0;
    state.brake_percent = 18.0;
    state.acc_x = -2.06;
    state.acc_y = 8.43;
    state.drive_state = VEHICLE_DRIVE_STATE_DRIVE;
    state.lap_valid = true;
    state.lap_number = 2U;
    state.lap_time_s = 83.486;
    state.best_lap_time_s = 82.910;
    state.delta_time_s = -0.42;
    state.gps_valid = true;
    state.gps_fix_type = 4U;
    state.can_valid = true;
    switch (index) {
    case 1U: state.motor_rpm = 300.0; state.throttle_percent = 0.0; state.brake_percent = 0.0; break;
    case 2U: state.motor_rpm = 9800.0; break;
    case 3U: state.motor_rpm = 1200.0; state.throttle_percent = 100.0; state.brake_percent = 0.0; break;
    case 4U: state.motor_rpm = 1200.0; state.throttle_percent = 0.0; state.brake_percent = 90.0; break;
    case 5U: state.acc_x = 1.5 * 9.80665; state.acc_y = 1.5 * 9.80665; break;
    case 6U: state.acc_x = -1.5 * 9.80665; state.acc_y = -1.5 * 9.80665; break;
    case 7U:
        state.valid_mask &= ~(VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                              VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                              VEHICLE_STATE_VALID_LV_VOLTAGE_V |
                              VEHICLE_STATE_VALID_ACC_X_MPS2 |
                              VEHICLE_STATE_VALID_ACC_Y_MPS2);
        state.gps_valid = false;
        state.can_valid = false;
        state.lap_valid = false;
        break;
    default: break;
    }
    return state;
}

int main(int argc, char **argv)
{
    const char *asset_dir = argc > 1 ? argv[1] : "assets/hud";
    const char *output_dir = argc > 2 ? argv[2] : "docs/validation/hud_png_snapshots";
    static const char *const names[] = {
        "normal", "low_rpm_zero_pedal", "high_rpm_red", "full_throttle",
        "high_brake", "g_positive", "g_negative", "invalid_gps_bms"
    };
    HudRenderer renderer;
    VideoFrame frame;
    HudSurface surface;
    VehicleState state;
    unsigned int index;

    if (mkdir(output_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", output_dir, strerror(errno));
        return EXIT_FAILURE;
    }
    memset(&renderer, 0, sizeof(renderer));
    memset(&frame, 0, sizeof(frame));
    if (hud_renderer_init_style(&renderer, 800U, 600U, HUD_STYLE_PNG,
                                asset_dir, HUD_DEFAULT_FONT_PATH,
                                HUD_PIXEL_FORMAT_RGBA8888) < 0 ||
        video_frame_init(&frame, 800U, 600U) < 0) {
        hud_renderer_close(&renderer);
        return EXIT_FAILURE;
    }
    surface.pixels = frame.pixels;
    surface.width = frame.width;
    surface.height = frame.height;
    surface.stride_bytes = frame.stride;
    surface.pixel_format = HUD_PIXEL_FORMAT_RGBA8888;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        char path[512];

        memset(frame.pixels, 0, frame.capacity);
        state = snapshot_state(index);
        if (hud_renderer_render_frame_to_surface(
                &renderer, &state, index == 2U, &surface,
                NULL) == NULL ||
            snprintf(path, sizeof(path), "%s/%s.bmp", output_dir,
                     names[index]) >= (int)sizeof(path) ||
            !video_frame_write_bmp(&frame, path)) {
            fprintf(stderr, "snapshot %s failed: %s\n", names[index],
                    strerror(errno));
            video_frame_close(&frame);
            hud_renderer_close(&renderer);
            return EXIT_FAILURE;
        }
    }
    video_frame_close(&frame);
    hud_renderer_close(&renderer);
    return EXIT_SUCCESS;
}
