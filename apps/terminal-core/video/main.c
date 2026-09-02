#define _POSIX_C_SOURCE 200809L
#include "console_log.h"
#include "hud_resources.h"
#include "video_capture.h"
#include "video_pipeline.h"
#include <race/core_status.h>
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

typedef struct {
    RaceSharedStateHandle shared;
    RaceCoreStatusHandle core_status;
    bool stale_reported;
} VideoStateSource;

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
    char record_directory[PATH_MAX];
    const char *hud_font = HUD_DEFAULT_FONT_PATH;
    const char *video_ipc_name = RACE_VIDEO_IPC_NAME;
    HudStyle hud_style = HUD_STYLE_UI;
    uint32_t seconds = 0U;
    uint32_t video_publish_fps = VIDEO_PIPELINE_DEFAULT_WEB_PUBLISH_FPS;
    uint32_t record_segment_seconds =
        VIDEO_PIPELINE_DEFAULT_RECORD_SEGMENT_SECONDS;
    double record_start_hv_voltage =
        VIDEO_PIPELINE_DEFAULT_HV_RECORD_THRESHOLD_V;
    int i;
    memset(&pipeline, 0, sizeof(pipeline)); memset(&config, 0, sizeof(config));
    memset(&source, 0, sizeof(source));
    source.shared.fd = -1; source.core_status.fd = -1;
    record_directory[0] = '\0';
    console_log_init();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) console_log_set_level(CONSOLE_LOG_DEBUG);
        else if (!strcmp(argv[i], "--camera") && i + 1 < argc) camera = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!strcmp(argv[i], "--record-dir") && i + 1 < argc) {
            if (snprintf(record_directory, sizeof(record_directory), "%s",
                         argv[++i]) >= (int)sizeof(record_directory)) {
                LOG_ERROR("Recording directory is too long");
                return EXIT_FAILURE;
            }
        }
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--record-segment-seconds") && i + 1 < argc) {
            unsigned long parsed = strtoul(argv[++i], NULL, 10);
            if (parsed == 0U || parsed > VIDEO_RECORDER_MAX_DURATION_SECONDS) {
                LOG_ERROR("Invalid --record-segment-seconds; use 1..%u",
                          VIDEO_RECORDER_MAX_DURATION_SECONDS);
                return EXIT_FAILURE;
            }
            record_segment_seconds = (uint32_t)parsed;
        }
        else if (!strcmp(argv[i], "--record-hv-voltage") && i + 1 < argc) {
            char *end = NULL;
            record_start_hv_voltage = strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || record_start_hv_voltage < 0.0) {
                LOG_ERROR("Invalid --record-hv-voltage");
                return EXIT_FAILURE;
            }
        }
        else if (!strcmp(argv[i], "--hud-style") && i + 1 < argc &&
                 hud_style_parse(argv[++i], &hud_style)) { }
        else if (!strcmp(argv[i], "--hud-font") && i + 1 < argc) hud_font = argv[++i];
        else if (!strcmp(argv[i], "--video-ipc-name") && i + 1 < argc) video_ipc_name = argv[++i];
        else if (!strcmp(argv[i], "--video-publish-fps") && i + 1 < argc) {
            unsigned long parsed = strtoul(argv[++i], NULL, 10);
            if (parsed == 0U || parsed > 100U) {
                LOG_ERROR("Invalid --video-publish-fps; use 1..100");
                return EXIT_FAILURE;
            }
            video_publish_fps = (uint32_t)parsed;
        }
        else if (!strcmp(argv[i], "--no-hud")) hud_style = HUD_STYLE_NONE;
        else { LOG_ERROR("Usage: %s [-v] [--camera PATH] [--record PATH] [--record-dir DIR] [--record-hv-voltage V] [--record-segment-seconds N] [--seconds N] [--video-ipc-name NAME] [--video-publish-fps N] [--hud-style ui|none] [--hud-font PATH] [--no-hud]", argv[0]); return EXIT_FAILURE; }
    }
    if (install_signals() < 0) { LOG_ERROR("sigaction: %s", strerror(errno)); return EXIT_FAILURE; }
    if (race_shared_state_reader_open(&source.shared, NULL) < 0) {
        LOG_ERROR("Cannot open %s: %s", RACE_SHARED_STATE_NAME, strerror(errno)); return EXIT_FAILURE;
    }
    if (record != NULL && record_directory[0] != '\0') {
        LOG_ERROR("Use either --record or --record-dir, not both");
        race_shared_state_close(&source.shared);
        return EXIT_FAILURE;
    }
    if (record == NULL && record_directory[0] == '\0' &&
        race_core_status_reader_open(&source.core_status, NULL) == 0) {
        RaceCoreStatusData status;

        if (race_core_status_read(&source.core_status, &status) == 0 &&
            status.state == RACE_CORE_LIFECYCLE_RUNNING &&
            (status.flags & RACE_CORE_STATUS_LOGGER_READY) != 0U) {
            (void)snprintf(record_directory, sizeof(record_directory), "%s",
                           status.session_directory);
        }
    }
    config.device_path = camera; config.record_path = record;
    config.record_directory = record_directory[0] != '\0' ? record_directory : NULL;
    config.width = VIDEO_CAPTURE_WIDTH; config.height = VIDEO_CAPTURE_HEIGHT;
    config.video_ipc_name = video_ipc_name;
    config.video_publish_fps = video_publish_fps;
    config.fps = VIDEO_CAPTURE_FPS; config.run_seconds = seconds;
    config.record_segment_seconds = record_segment_seconds;
    config.record_start_hv_voltage = record_start_hv_voltage;
    config.compositor_mode = VIDEO_COMPOSITOR_MODE_AUTO;
    config.hud_style = hud_style;
    config.hud_font_path = hud_font;
    config.get_state = shared_state_getter; config.state_context = &source;
    if (video_pipeline_init(&pipeline, &config) < 0 || video_pipeline_start(&pipeline) < 0) {
        video_pipeline_stop(&pipeline); race_shared_state_close(&source.shared); return EXIT_FAILURE;
    }
    while (!stop_requested) { struct timespec delay = {0, 100000000L}; nanosleep(&delay, NULL); }
    video_pipeline_stop(&pipeline); video_pipeline_print_stats(&pipeline);
    race_core_status_close(&source.core_status);
    race_shared_state_close(&source.shared);
    return EXIT_SUCCESS;
}
