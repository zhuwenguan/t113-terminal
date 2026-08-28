#define _POSIX_C_SOURCE 200809L
#include "console_log.h"
#include "video_capture.h"
#include "video_pipeline.h"
#include <race/shared_state.h>
#include <race/time_utils.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop_requested;
static void handle_signal(int signal_number) { (void)signal_number; stop_requested = 1; }

typedef struct { RaceSharedStateHandle shared; bool stale_reported; } VideoStateSource;

static bool shared_state_getter(VehicleState *state, void *context) {
    VideoStateSource *source = context;
    uint64_t now = 0U, published = 0U;
    if (race_shared_state_read(&source->shared, state, &published, NULL) < 0 ||
        race_monotonic_us(&now) < 0) {
        memset(state, 0, sizeof(*state)); return false;
    }
    if (race_shared_state_is_stale(now, published, RACE_STATE_STALE_US)) {
        if (!source->stale_reported) LOG_WARN("CORE STATE STALE");
        source->stale_reported = true;
        memset(state, 0, sizeof(*state)); return false;
    }
    if (source->stale_reported) LOG_INFO("CORE STATE LIVE");
    source->stale_reported = false; return true;
}

static int install_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action)); action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) < 0 ||
           sigaction(SIGTERM, &action, NULL) < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    VideoPipeline pipeline;
    VideoPipelineConfig config;
    VideoStateSource source;
    const char *camera = VIDEO_CAPTURE_DEFAULT_DEVICE;
    const char *record = NULL;
    const char *hud_assets = HUD_DEFAULT_ASSET_DIR;
    const char *hud_font = HUD_DEFAULT_FONT_PATH;
    HudStyle hud_style = HUD_STYLE_PNG;
    uint32_t seconds = 0U;
    int i;
    memset(&pipeline, 0, sizeof(pipeline)); memset(&config, 0, sizeof(config));
    memset(&source, 0, sizeof(source)); source.shared.fd = -1;
    console_log_init();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) console_log_set_level(CONSOLE_LOG_DEBUG);
        else if (!strcmp(argv[i], "--camera") && i + 1 < argc) camera = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--hud-style") && i + 1 < argc &&
                 hud_style_parse(argv[++i], &hud_style)) { }
        else if (!strcmp(argv[i], "--hud-assets") && i + 1 < argc) hud_assets = argv[++i];
        else if (!strcmp(argv[i], "--hud-font") && i + 1 < argc) hud_font = argv[++i];
        else if (!strcmp(argv[i], "--no-hud")) hud_style = HUD_STYLE_NONE;
        else { LOG_ERROR("Usage: %s [-v] [--camera PATH] [--record PATH] [--seconds N] [--hud-style png|legacy|none] [--hud-assets PATH] [--hud-font PATH] [--no-hud]", argv[0]); return EXIT_FAILURE; }
    }
    if (install_signals() < 0) { LOG_ERROR("sigaction: %s", strerror(errno)); return EXIT_FAILURE; }
    if (race_shared_state_reader_open(&source.shared, NULL) < 0) {
        LOG_ERROR("Cannot open %s: %s", RACE_SHARED_STATE_NAME, strerror(errno)); return EXIT_FAILURE;
    }
    config.device_path = camera; config.record_path = record;
    config.width = VIDEO_CAPTURE_WIDTH; config.height = VIDEO_CAPTURE_HEIGHT;
    config.fps = VIDEO_CAPTURE_FPS; config.run_seconds = seconds;
    config.compositor_mode = VIDEO_COMPOSITOR_MODE_AUTO;
    config.hud_style = hud_style;
    config.hud_asset_path = hud_assets;
    config.hud_font_path = hud_font;
    config.get_state = shared_state_getter; config.state_context = &source;
    if (video_pipeline_init(&pipeline, &config) < 0 || video_pipeline_start(&pipeline) < 0) {
        video_pipeline_stop(&pipeline); race_shared_state_close(&source.shared); return EXIT_FAILURE;
    }
    while (!stop_requested) { struct timespec delay = {0, 100000000L}; nanosleep(&delay, NULL); }
    video_pipeline_stop(&pipeline); video_pipeline_print_stats(&pipeline);
    race_shared_state_close(&source.shared);
    return EXIT_SUCCESS;
}
