#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "can_decoder.h"
#include "can_receiver.h"
#include "console_log.h"
#include "hmi.h"
#include "lappro.h"
#include "logger.h"
#include "race_event.h"
#include "replay.h"
#include "vehicle_state_snapshot.h"
#include "vehicle_state_clock.h"
#ifndef RACE_CORE_DAEMON
#include "hud_resources.h"
#include "video_capture.h"
#include "video_pipeline.h"
#include <signal.h>
#endif
#ifdef RACE_CORE_DAEMON
#include <race/shared_state.h>
#endif

typedef struct {
    pthread_mutex_t mutex;
    Logger *logger;
    CanDecoder decoder;
    RaceEvent race_event;
    VehicleState vehicle_state;
    VehicleStateClock clock;
    Hmi *hmi;
#ifdef RACE_CORE_DAEMON
    RaceSharedStateHandle shared_state;
#endif
} MainCanContext;

typedef struct {
    bool replay;
    char replay_directory[PATH_MAX];
    double replay_speed;
#ifndef RACE_CORE_DAEMON
    bool video;
    bool video_only;
    bool hud_demo;
    bool video_record;
    char camera_path[PATH_MAX];
    char video_output[PATH_MAX];
    char video_frame[PATH_MAX];
    char hud_font_path[PATH_MAX];
    uint32_t video_seconds;
    VideoCompositorMode video_compositor_mode;
    HudStyle hud_style;
#endif
} MainOptions;

#ifndef RACE_CORE_DAEMON
static volatile sig_atomic_t main_stop_requested;

static void main_handle_signal(int signal_number)
{
    (void)signal_number;
    main_stop_requested = 1;
}

static int main_install_signals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = main_handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }
    return 0;
}
#endif

static uint64_t main_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static void main_vehicle_state_snapshot_locked(MainCanContext *can_context,
                                               uint64_t timestamp_us,
                                               VehicleState *state)
{
    can_decoder_expire_stale(&can_context->decoder, timestamp_us,
                             &can_context->vehicle_state);
    vehicle_state_clock_expire_lappro(&can_context->clock, timestamp_us,
                                      &can_context->vehicle_state);
    can_context->vehicle_state.can_valid =
        (can_context->vehicle_state.valid_mask &
         VEHICLE_STATE_CAN_REQUIRED_MASK) == VEHICLE_STATE_CAN_REQUIRED_MASK;
    can_context->vehicle_state.gps_valid =
        (can_context->vehicle_state.valid_mask &
         VEHICLE_STATE_LAPPRO_REQUIRED_MASK) ==
        VEHICLE_STATE_LAPPRO_REQUIRED_MASK;
    *state = can_context->vehicle_state;
    state->timestamp_us = timestamp_us;
}

static bool main_vehicle_state_snapshot_provider(uint64_t timestamp_us,
                                                 VehicleState *state,
                                                 void *context)
{
    MainCanContext *can_context = context;

    if (can_context == NULL || state == NULL) {
        return false;
    }

    pthread_mutex_lock(&can_context->mutex);
    main_vehicle_state_snapshot_locked(can_context, timestamp_us, state);
    pthread_mutex_unlock(&can_context->mutex);

    /* HMI can display partial state; the logger applies its own completeness
     * policy in the sink below. */
    return true;
}

static void main_vehicle_state_snapshot_sink(uint64_t timestamp_us,
                                             const VehicleState *state,
                                             void *context)
{
    MainCanContext *can_context = context;
    const uint32_t required_mask = VEHICLE_STATE_CAN_REQUIRED_MASK |
                                   VEHICLE_STATE_LAPPRO_REQUIRED_MASK;

    if (can_context == NULL || state == NULL) {
        return;
    }

    if (can_context->logger != NULL &&
        (state->valid_mask & required_mask) == required_mask) {
        logger_vehicle_state_sink(timestamp_us, state, can_context->logger);
    }
    if (can_context->hmi != NULL) {
        (void)hmi_update(can_context->hmi, timestamp_us, state);
    }
#ifdef RACE_CORE_DAEMON
    if (can_context->shared_state.mapping != NULL &&
        race_shared_state_publish(&can_context->shared_state, state,
                                  main_monotonic_us()) < 0) {
        LOG_WARN("Shared VehicleState publish failed: %s", strerror(errno));
    }
#endif
}

static void main_replay_vehicle_state_snapshot_sink(uint64_t timestamp_us,
                                                    void *context)
{
    VehicleState state;

    if (main_vehicle_state_snapshot_provider(timestamp_us, &state, context)) {
        main_vehicle_state_snapshot_sink(timestamp_us, &state, context);
    }
}

static bool main_vehicle_state_getter(VehicleState *state, void *context)
{
    MainCanContext *can_context = context;
    uint64_t timestamp_us;

    if (can_context == NULL || state == NULL) {
        return false;
    }
    pthread_mutex_lock(&can_context->mutex);
    timestamp_us = vehicle_state_clock_snapshot_timestamp(
        &can_context->clock, main_monotonic_us());
    main_vehicle_state_snapshot_locked(can_context, timestamp_us, state);
    pthread_mutex_unlock(&can_context->mutex);
    return true;
}

#ifndef RACE_CORE_DAEMON
static bool main_hud_demo_state_getter(VehicleState *state, void *context)
{
    (void)context;
    if (state == NULL) {
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->timestamp_us = main_monotonic_us();
    state->valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                        VEHICLE_STATE_VALID_ACC_X_MPS2 |
                        VEHICLE_STATE_VALID_ACC_Y_MPS2 |
                        VEHICLE_STATE_VALID_MOTOR_RPM |
                        VEHICLE_STATE_VALID_TORQUE_NM |
                        VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                        VEHICLE_STATE_VALID_HV_CURRENT_A |
                        VEHICLE_STATE_VALID_HV_POWER_W |
                        VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                        VEHICLE_STATE_VALID_LV_VOLTAGE_V |
                        VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                        VEHICLE_STATE_VALID_BRAKE_PERCENT |
                        VEHICLE_STATE_VALID_DRIVE_STATE;
    state->lap_valid = true;
    state->lap_number = 2U;
    state->lap_time_s = 83.486;
    state->best_lap_time_s = 82.910;
    state->delta_time_s = -0.42;
    state->speed_kmh = 72.0;
    state->acc_x = -2.06;
    state->acc_y = 8.43;
    state->motor_rpm = 4280.0;
    state->torque_nm = 96.0;
    state->hv_voltage_v = 356.0;
    state->hv_current_a = 118.0;
    state->hv_power_w = 42000.0;
    state->hv_soc_percent = 78.0;
    state->lv_voltage_v = 13.6;
    state->throttle_percent = 68.0;
    state->brake_percent = 18.0;
    state->drive_state = VEHICLE_DRIVE_STATE_DRIVE;
    return true;
}
#endif

#ifndef RACE_CORE_DAEMON
static bool main_parse_uint32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool main_copy_option_path(char *destination, size_t destination_size,
                                  const char *source)
{
    if (destination == NULL || destination_size == 0U || source == NULL ||
        source[0] == '\0' || strlen(source) >= destination_size) {
        return false;
    }
    (void)snprintf(destination, destination_size, "%s", source);
    return true;
}
#endif

static void main_can_message_sink(const CanMessage *message, void *context)
{
    MainCanContext *can_context = context;

    if (can_context == NULL || message == NULL) {
        return;
    }

    /* Keep the raw path independent from decoder mappings and failures. */
    if (can_context->logger != NULL) {
        logger_message_sink(message, can_context->logger);
    }

    pthread_mutex_lock(&can_context->mutex);
    vehicle_state_clock_note_event(&can_context->clock,
                                   message->timestamp_us);
    (void)can_decoder_process(&can_context->decoder, message,
                              &can_context->vehicle_state);
    pthread_mutex_unlock(&can_context->mutex);
}

static void main_lappro_sample_sink(uint64_t timestamp_us,
                                    const LapProState *sample,
                                    void *context)
{
    MainCanContext *can_context = context;
    const uint32_t lappro_valid_mask = VEHICLE_STATE_LAPPRO_REQUIRED_MASK;

    if (can_context == NULL || sample == NULL) {
        return;
    }

    if (can_context->logger != NULL) {
        logger_lappro_sink(timestamp_us, sample, can_context->logger);
    }

    pthread_mutex_lock(&can_context->mutex);
    vehicle_state_clock_note_lappro(&can_context->clock, timestamp_us,
                                    sample->fix_type >= 3U);
    can_decoder_expire_stale(&can_context->decoder, timestamp_us,
                             &can_context->vehicle_state);
    can_context->vehicle_state.timestamp_us = timestamp_us;
    can_context->vehicle_state.gps_fix_type = sample->fix_type;
    can_context->vehicle_state.gps_num_sv = sample->num_sv;
    if (sample->fix_type >= 3U) {
        can_context->vehicle_state.speed_kmh = sample->speed_kmh;
        can_context->vehicle_state.heading_deg = sample->heading_deg;
        can_context->vehicle_state.longitude = sample->longitude;
        can_context->vehicle_state.latitude = sample->latitude;
        can_context->vehicle_state.acc_x = sample->acc_x;
        can_context->vehicle_state.acc_y = sample->acc_y;
        can_context->vehicle_state.acc_z = sample->acc_z;
        can_context->vehicle_state.valid_mask |= lappro_valid_mask;
    } else {
        can_context->vehicle_state.valid_mask &= ~lappro_valid_mask;
    }
    race_event_update(&can_context->race_event, timestamp_us, sample,
                      &can_context->vehicle_state);
    pthread_mutex_unlock(&can_context->mutex);
}

static bool main_parse_speed(const char *text, double *speed)
{
    char *end;
    double parsed;

    if (text == NULL || speed == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        !isfinite(parsed) || parsed < 0.0) {
        return false;
    }
    *speed = parsed;
    return true;
}

static int main_parse_arguments(int argc, char *argv[], MainOptions *options)
{
    int argument_index;

    if (options == NULL) {
        return -1;
    }
    memset(options, 0, sizeof(*options));
    options->replay_speed = 1.0;
#ifndef RACE_CORE_DAEMON
    options->video_compositor_mode = VIDEO_COMPOSITOR_MODE_AUTO;
    options->hud_style = HUD_STYLE_UI;
    (void)snprintf(options->camera_path, sizeof(options->camera_path), "%s",
                   VIDEO_CAPTURE_DEFAULT_DEVICE);
    (void)snprintf(options->hud_font_path, sizeof(options->hud_font_path),
                   "%s", HUD_DEFAULT_FONT_PATH);
#endif
    for (argument_index = 1; argument_index < argc; ++argument_index) {
        if (strcmp(argv[argument_index], "-v") == 0 ||
            strcmp(argv[argument_index], "--verbose") == 0) {
            console_log_set_level(CONSOLE_LOG_DEBUG);
            continue;
        }
        if (strcmp(argv[argument_index], "--replay") == 0) {
            if (argument_index + 1 >= argc ||
                argv[argument_index + 1][0] == '\0' ||
                snprintf(options->replay_directory,
                         sizeof(options->replay_directory), "%s",
                         argv[++argument_index]) >=
                    (int)sizeof(options->replay_directory)) {
                LOG_ERROR("Invalid --replay directory");
                return -1;
            }
            options->replay = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--replay-speed") == 0) {
            if (argument_index + 1 >= argc ||
                !main_parse_speed(argv[++argument_index],
                                  &options->replay_speed)) {
                LOG_ERROR("Invalid --replay-speed; use 0 or a positive number");
                return -1;
            }
            continue;
        }
        if (strncmp(argv[argument_index], "--replay-speed=", 15U) == 0) {
            if (!main_parse_speed(argv[argument_index] + 15U,
                                  &options->replay_speed)) {
                LOG_ERROR("Invalid --replay-speed; use 0 or a positive number");
                return -1;
            }
            continue;
        }
#ifndef RACE_CORE_DAEMON
        if (strcmp(argv[argument_index], "--video") == 0) {
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--video-only") == 0) {
            options->video = true;
            options->video_only = true;
            if (options->video_seconds == 0U) {
                options->video_seconds = 3U;
            }
            continue;
        }
        if (strcmp(argv[argument_index], "--camera") == 0) {
            if (argument_index + 1 >= argc ||
                !main_copy_option_path(options->camera_path,
                                       sizeof(options->camera_path),
                                       argv[++argument_index])) {
                LOG_ERROR("Invalid --camera path");
                return -1;
            }
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--video-record") == 0) {
            options->video = true;
            options->video_record = true;
            if (argument_index + 1 < argc &&
                argv[argument_index + 1][0] != '-') {
                if (!main_copy_option_path(options->video_output,
                                           sizeof(options->video_output),
                                           argv[++argument_index])) {
                    LOG_ERROR("Invalid --video-record output path");
                    return -1;
                }
            }
            continue;
        }
        if (strcmp(argv[argument_index], "--video-frame") == 0) {
            if (argument_index + 1 >= argc ||
                !main_copy_option_path(options->video_frame,
                                       sizeof(options->video_frame),
                                       argv[++argument_index])) {
                LOG_ERROR("Invalid --video-frame output path");
                return -1;
            }
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--video-seconds") == 0) {
            if (argument_index + 1 >= argc ||
                !main_parse_uint32(argv[++argument_index],
                                   &options->video_seconds)) {
                LOG_ERROR("Invalid --video-seconds value");
                return -1;
            }
            if (options->video_seconds >
                VIDEO_RECORDER_MAX_DURATION_SECONDS) {
                LOG_ERROR("Invalid --video-seconds value; maximum is %u",
                          VIDEO_RECORDER_MAX_DURATION_SECONDS);
                return -1;
            }
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--video-compositor") == 0) {
            if (argument_index + 1 >= argc ||
                !video_compositor_parse_mode(argv[++argument_index],
                                             &options->video_compositor_mode)) {
                LOG_ERROR("Invalid --video-compositor; use auto, g2d, or cpu");
                return -1;
            }
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--hud-style") == 0) {
            if (argument_index + 1 >= argc ||
                !hud_style_parse(argv[++argument_index], &options->hud_style)) {
                LOG_ERROR("Invalid --hud-style; use ui or none");
                return -1;
            }
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--hud-font") == 0) {
            if (argument_index + 1 >= argc ||
                !main_copy_option_path(options->hud_font_path,
                                       sizeof(options->hud_font_path),
                                       argv[++argument_index])) {
                LOG_ERROR("Invalid --hud-font path");
                return -1;
            }
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--no-hud") == 0) {
            options->hud_style = HUD_STYLE_NONE;
            options->video = true;
            continue;
        }
        if (strcmp(argv[argument_index], "--hud-demo") == 0) {
            options->hud_demo = true;
            options->video = true;
            options->video_seconds = 1U;
            if (options->video_frame[0] == '\0') {
                (void)snprintf(options->video_frame,
                               sizeof(options->video_frame),
                               "hud_demo.bmp");
            }
            continue;
        }
        if (strcmp(argv[argument_index], "--hud-demo-no-frame") == 0) {
            options->hud_demo = true;
            options->video = true;
            options->video_seconds = 1U;
            options->video_frame[0] = '\0';
            continue;
        }
#endif

#ifdef RACE_CORE_DAEMON
        LOG_ERROR("Usage: %s [-v|--verbose] [--replay <directory>] "
                  "[--replay-speed <number>]", argv[0]);
#else
        LOG_ERROR("Usage: %s [-v|--verbose] [--replay <directory>] "
                  "[--replay-speed <number>] [--video] [--camera <path>] "
                  "[--video-only] "
                  "[--video-record [path]] [--video-frame <path>] "
                  "[--video-seconds <number>] "
                  "[--video-compositor auto|g2d|cpu] "
                  "[--hud-style ui|none] "
                  "[--hud-font PATH] "
                  "[--no-hud] "
                  "[--hud-demo|--hud-demo-no-frame]",
                  argv[0]);
#endif
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    static const char *const interfaces[CAN_RECEIVER_BUS_COUNT] = {
        "awlink0",
        "awlink1"
    };
    CanReceiver receiver;
    Logger logger;
    Hmi hmi;
    LapProConfig lappro_config;
    LapPro lappro;
    VehicleStateSnapshotter snapshotter;
#ifndef RACE_CORE_DAEMON
    VideoPipeline video_pipeline;
#endif
    static MainCanContext can_context;
    MainOptions options;
    bool lappro_ready = false;
    bool logger_ready = false;
    bool hmi_ready = false;
    bool snapshotter_ready = false;
#ifndef RACE_CORE_DAEMON
    bool video_ready = false;
#endif
    int context_mutex_result;
    int result;

    memset(&can_context, 0, sizeof(can_context));
    memset(&snapshotter, 0, sizeof(snapshotter));
#ifndef RACE_CORE_DAEMON
    memset(&video_pipeline, 0, sizeof(video_pipeline));
#endif
    can_decoder_init(&can_context.decoder);
    console_log_init();
    if (main_parse_arguments(argc, argv, &options) < 0) {
        return EXIT_FAILURE;
    }
#ifndef RACE_CORE_DAEMON
    if (main_install_signals() < 0) {
        LOG_ERROR("Cannot install video termination handlers: %s",
                  strerror(errno));
        return EXIT_FAILURE;
    }
#endif
#ifdef RACE_CORE_DAEMON
    if (race_shared_state_writer_open(&can_context.shared_state, NULL) < 0) {
        LOG_ERROR("Cannot create %s: %s", RACE_SHARED_STATE_NAME,
                  strerror(errno));
        return EXIT_FAILURE;
    }
    LOG_INFO("Shared VehicleState ready: name=%s version=%u size=%zu",
             RACE_SHARED_STATE_NAME, RACE_SHM_VERSION,
             sizeof(RaceSharedState));
#endif
    vehicle_state_clock_init(&can_context.clock, options.replay);
    context_mutex_result = pthread_mutex_init(&can_context.mutex, NULL);
    if (context_mutex_result != 0) {
        LOG_ERROR("Cannot initialize VehicleState mutex: %s",
                  strerror(context_mutex_result));
#ifdef RACE_CORE_DAEMON
        race_shared_state_close(&can_context.shared_state);
#endif
        return EXIT_FAILURE;
    }
    if (logger_init(&logger) < 0) {
        LOG_WARN("Data logger unavailable; continuing without file logging");
    } else if (logger_start(&logger) < 0) {
        logger_stop(&logger);
        LOG_WARN("Data logger did not start; continuing without file logging");
    } else {
        logger_ready = true;
    }
    can_context.logger = logger_ready ? &logger : NULL;
    if (hmi_init(&hmi, HMI_DEFAULT_DEVICE_PATH) < 0) {
        LOG_WARN("HMI unavailable; continuing without display output");
    } else {
        hmi_ready = true;
    }
    can_context.hmi = hmi_ready ? &hmi : NULL;
    race_event_init(&can_context.race_event,
                    logger_ready ? logger_get_session_directory(&logger)
                                 : NULL);

#ifndef RACE_CORE_DAEMON
    if (options.video_record && options.video_output[0] == '\0') {
        const char *session_directory = logger_ready ?
            logger_get_session_directory(&logger) : NULL;
        if (session_directory == NULL ||
            snprintf(options.video_output, sizeof(options.video_output),
                     "%s/onboard.mp4", session_directory) >=
                (int)sizeof(options.video_output)) {
            LOG_WARN("Video recording disabled: no logger session directory");
            options.video_record = false;
        }
    }
    if (options.video) {
        VideoPipelineConfig video_config;

        memset(&video_config, 0, sizeof(video_config));
        video_config.device_path = options.camera_path;
        video_config.record_path = options.video_record ?
                                   options.video_output : NULL;
        video_config.single_frame_path = options.video_frame[0] != '\0' ?
                                         options.video_frame : NULL;
        video_config.width = VIDEO_CAPTURE_WIDTH;
        video_config.height = VIDEO_CAPTURE_HEIGHT;
        video_config.fps = VIDEO_CAPTURE_FPS;
        video_config.run_seconds = options.video_seconds;
        video_config.mock_camera = options.hud_demo;
        video_config.compositor_mode = options.hud_demo ?
            VIDEO_COMPOSITOR_MODE_CPU : options.video_compositor_mode;
        video_config.hud_style = options.hud_style;
        video_config.hud_font_path = options.hud_font_path;
        video_config.get_state = options.hud_demo ?
                                 main_hud_demo_state_getter :
                                 main_vehicle_state_getter;
        video_config.state_context = &can_context;
        if (video_pipeline_init(&video_pipeline, &video_config) < 0 ||
            video_pipeline_start(&video_pipeline) < 0) {
            video_pipeline_stop(&video_pipeline);
            LOG_WARN("Video pipeline did not start; continuing vehicle data");
        } else {
            video_ready = true;
        }
    }

    if (options.hud_demo || options.video_only) {
        struct timespec wait_time = { 0, 100000000L };
        uint32_t wait_iterations = options.video_seconds * 10U + 10U;

        while (!main_stop_requested && wait_iterations-- > 0U) {
            (void)nanosleep(&wait_time, NULL);
        }
        if (video_ready) {
            video_pipeline_stop(&video_pipeline);
            video_pipeline_print_stats(&video_pipeline);
        }
        race_event_close(&can_context.race_event);
        if (logger_ready) {
            logger_stop(&logger);
            logger_print_stats(&logger);
        }
        if (hmi_ready) {
            hmi_close(&hmi);
        }
        sync();
        pthread_mutex_destroy(&can_context.mutex);
        return video_ready ? EXIT_SUCCESS : EXIT_FAILURE;
    }
#endif

    if (options.replay) {
        ReplayConfig replay_config;

        memset(&replay_config, 0, sizeof(replay_config));
        replay_config.directory = options.replay_directory;
        replay_config.speed = options.replay_speed;
        replay_config.can_sink = main_can_message_sink;
        replay_config.lappro_sink = main_lappro_sample_sink;
        replay_config.sink_context = &can_context;
        replay_config.vehicle_state_getter = main_vehicle_state_getter;
        replay_config.vehicle_state_snapshot_sink =
            main_replay_vehicle_state_snapshot_sink;
        result = replay_run(&replay_config);
        #ifndef RACE_CORE_DAEMON
        if (video_ready) {
            video_pipeline_stop(&video_pipeline);
            video_pipeline_print_stats(&video_pipeline);
        }
        #endif
        race_event_close(&can_context.race_event);
        if (logger_ready) {
            logger_stop(&logger);
            logger_print_stats(&logger);
        }
        if (hmi_ready) {
            hmi_close(&hmi);
        }
        sync();
        pthread_mutex_destroy(&can_context.mutex);
#ifdef RACE_CORE_DAEMON
        race_shared_state_close(&can_context.shared_state);
#endif
        if (result < 0) {
            LOG_ERROR("Replay stopped with an error");
            return EXIT_FAILURE;
        }
        LOG_INFO("Replay stopped cleanly");
        return EXIT_SUCCESS;
    }

    lappro_config_default(&lappro_config);
    if (lappro_init(&lappro, &lappro_config) == 0) {
        lappro_ready = true;
        lappro_set_sample_sink(&lappro, main_lappro_sample_sink,
                               &can_context);
        if (lappro_start(&lappro) < 0) {
            LOG_WARN("LapPro worker did not start");
        }
    } else {
        LOG_WARN("LapPro initialization failed; continuing CAN");
    }

    if (can_receiver_init(&receiver, interfaces) < 0) {
#ifndef RACE_CORE_DAEMON
        if (video_ready) {
            video_pipeline_stop(&video_pipeline);
            video_pipeline_print_stats(&video_pipeline);
        }
#endif
        if (lappro_ready) {
            lappro_stop(&lappro);
        }
        race_event_close(&can_context.race_event);
        if (logger_ready) {
            logger_stop(&logger);
        }
        if (hmi_ready) {
            hmi_close(&hmi);
        }
        pthread_mutex_destroy(&can_context.mutex);
#ifdef RACE_CORE_DAEMON
        race_shared_state_close(&can_context.shared_state);
#endif
        return EXIT_FAILURE;
    }

    can_receiver_set_message_sink(&receiver, main_can_message_sink,
                                  &can_context);
    if (vehicle_state_snapshotter_init(
            &snapshotter, main_vehicle_state_snapshot_provider,
            main_vehicle_state_snapshot_sink, &can_context) < 0 ||
        vehicle_state_snapshotter_start(&snapshotter) < 0) {
        vehicle_state_snapshotter_stop(&snapshotter);
        LOG_WARN("VehicleState snapshotter did not start");
    } else {
        snapshotter_ready = true;
    }
    result = can_receiver_run(&receiver);
    can_receiver_print_stats(&receiver);
    can_receiver_close(&receiver);
    if (snapshotter_ready) {
        vehicle_state_snapshotter_stop(&snapshotter);
    }
    if (lappro_ready) {
        lappro_stop(&lappro);
    }
    #ifndef RACE_CORE_DAEMON
    if (video_ready) {
        video_pipeline_stop(&video_pipeline);
        video_pipeline_print_stats(&video_pipeline);
    }
    #endif
    race_event_close(&can_context.race_event);
    if (logger_ready) {
        logger_stop(&logger);
        logger_print_stats(&logger);
    }
    if (hmi_ready) {
        hmi_close(&hmi);
    }
    sync();
    pthread_mutex_destroy(&can_context.mutex);
#ifdef RACE_CORE_DAEMON
    race_shared_state_close(&can_context.shared_state);
#endif

    if (result < 0) {
        LOG_ERROR("CAN receiver stopped with an error");
        return EXIT_FAILURE;
    }

    LOG_INFO("CAN receiver stopped cleanly");
    return EXIT_SUCCESS;
}
