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
#include "bms_parser.h"
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
#include <signal.h>
#include <race/core_status.h>
#include <race/shared_state.h>
#include <race/bms_shared_state.h>
#endif

typedef struct {
    pthread_mutex_t mutex;
    Logger *logger;
    CanDecoder decoder;
    BmsParser bms_parser;
    RaceEvent race_event;
    VehicleState vehicle_state;
    VehicleStateClock clock;
    Hmi *hmi;
#ifdef RACE_CORE_DAEMON
    pthread_mutex_t logger_mutex;
    pthread_mutex_t status_mutex;
    pthread_t logger_thread;
    Logger *logger_instance;
    bool logger_ready;
    bool logger_ever_ready;
    bool logger_thread_stop_requested;
    bool logger_thread_started;
    char logger_session_directory[RACE_CORE_STATUS_PATH_MAX];
    uint32_t can_ready_mask;
    LapPro *lappro;
    LapProConfig *lappro_config;
    bool lappro_ready;
    RaceCoreStatusHandle core_status;
    RaceSharedStateHandle shared_state;
    RaceSharedBmsStateHandle bms_shared_state;
#endif
} MainCanContext;

typedef struct {
    bool replay;
    char replay_directory[PATH_MAX];
    double replay_speed;
    double soc_initial_percent;
    double battery_usable_capacity_ah;
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

#ifdef RACE_CORE_DAEMON
static volatile sig_atomic_t main_stop_requested;
static CanReceiver *main_active_receiver;

static void main_core_handle_signal(int signal_number)
{
    (void)signal_number;
    main_stop_requested = 1;
    if (main_active_receiver != NULL) {
        can_receiver_stop(main_active_receiver);
    }
}

static int main_core_install_signals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = main_core_handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }
    return 0;
}
#endif

#ifdef RACE_CORE_DAEMON
static void main_lappro_sample_sink(uint64_t timestamp_us,
                                    const LapProState *sample,
                                    void *context);
#endif

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

#ifdef RACE_CORE_DAEMON
static bool main_logger_is_ready(MainCanContext *can_context)
{
    bool ready;

    if (can_context == NULL ||
        pthread_mutex_lock(&can_context->logger_mutex) != 0) {
        return false;
    }
    ready = can_context->logger_ready;
    pthread_mutex_unlock(&can_context->logger_mutex);
    return ready;
}

static void main_logger_message_sink(const CanMessage *message,
                                     MainCanContext *can_context)
{
    if (can_context == NULL || message == NULL ||
        pthread_mutex_lock(&can_context->logger_mutex) != 0) {
        return;
    }
    if (can_context->logger_ready && can_context->logger_instance != NULL) {
        logger_message_sink(message, can_context->logger_instance);
    }
    pthread_mutex_unlock(&can_context->logger_mutex);
}

static void main_logger_lappro_sink(uint64_t timestamp_us,
                                    const LapProState *sample,
                                    MainCanContext *can_context)
{
    if (can_context == NULL || sample == NULL ||
        pthread_mutex_lock(&can_context->logger_mutex) != 0) {
        return;
    }
    if (can_context->logger_ready && can_context->logger_instance != NULL) {
        logger_lappro_sink(timestamp_us, sample, can_context->logger_instance);
    }
    pthread_mutex_unlock(&can_context->logger_mutex);
}

static void main_logger_vehicle_state_sink(uint64_t timestamp_us,
                                           const VehicleState *state,
                                           MainCanContext *can_context)
{
    if (can_context == NULL || state == NULL ||
        pthread_mutex_lock(&can_context->logger_mutex) != 0) {
        return;
    }
    if (can_context->logger_ready && can_context->logger_instance != NULL) {
        logger_vehicle_state_sink(timestamp_us, state,
                                   can_context->logger_instance);
    }
    pthread_mutex_unlock(&can_context->logger_mutex);
}

static void main_update_core_status(MainCanContext *can_context,
                                    uint32_t can_ready_mask,
                                    bool update_can,
                                    bool stopping)
{
    RaceCoreStatusData data;
    bool logger_ready;
    bool lappro_ready;

    if (can_context == NULL ||
        pthread_mutex_lock(&can_context->logger_mutex) != 0) {
        return;
    }
    logger_ready = can_context->logger_ready;
    pthread_mutex_unlock(&can_context->logger_mutex);

    if (pthread_mutex_lock(&can_context->status_mutex) != 0) {
        return;
    }
    if (update_can) {
        can_context->can_ready_mask = can_ready_mask;
    }
    lappro_ready = can_context->lappro_ready;
    if (logger_ready) {
        can_context->logger_ever_ready = true;
    }
    memset(&data, 0, sizeof(data));
    data.can_ready_mask = can_context->can_ready_mask;
    data.flags = (logger_ready ? RACE_CORE_STATUS_LOGGER_READY : 0U) |
                 (can_context->can_ready_mask & 1U ?
                      RACE_CORE_STATUS_CAN0_READY : 0U) |
                 (can_context->can_ready_mask & 2U ?
                      RACE_CORE_STATUS_CAN1_READY : 0U) |
                 (can_context->hmi != NULL ? RACE_CORE_STATUS_HMI_READY : 0U) |
                 (lappro_ready ? RACE_CORE_STATUS_LAPPRO_READY : 0U);
    if (stopping) {
        data.state = RACE_CORE_LIFECYCLE_STOPPING;
    } else if (!logger_ready && can_context->logger_ever_ready) {
        data.state = RACE_CORE_LIFECYCLE_DEGRADED;
    } else if (logger_ready && can_context->can_ready_mask == 3U) {
        data.state = RACE_CORE_LIFECYCLE_RUNNING;
    } else {
        data.state = RACE_CORE_LIFECYCLE_PRECHECK;
    }
    data.heartbeat_mono_us = main_monotonic_us();
    if (logger_ready) {
        (void)snprintf(data.session_directory,
                       sizeof(data.session_directory), "%s",
                       can_context->logger_session_directory);
    }
    (void)race_core_status_publish(&can_context->core_status, &data);
    pthread_mutex_unlock(&can_context->status_mutex);
}

static void main_lappro_set_ready(MainCanContext *can_context, bool ready)
{
    if (can_context == NULL || pthread_mutex_lock(&can_context->status_mutex) != 0) {
        return;
    }
    can_context->lappro_ready = ready;
    pthread_mutex_unlock(&can_context->status_mutex);
}

static bool main_lappro_get_ready(MainCanContext *can_context)
{
    bool ready;

    if (can_context == NULL ||
        pthread_mutex_lock(&can_context->status_mutex) != 0) {
        return false;
    }
    ready = can_context->lappro_ready;
    pthread_mutex_unlock(&can_context->status_mutex);
    return ready;
}

static void main_logger_sleep(MainCanContext *can_context)
{
    struct timespec delay = { 0, 100000000L };
    unsigned int iteration;

    for (iteration = 0U; iteration < 10U; ++iteration) {
        bool stop;

        if (pthread_mutex_lock(&can_context->logger_mutex) != 0) {
            return;
        }
        stop = can_context->logger_thread_stop_requested;
        pthread_mutex_unlock(&can_context->logger_mutex);
        if (stop || main_stop_requested) {
            return;
        }
        (void)nanosleep(&delay, NULL);
    }
}

static void *main_logger_thread_main(void *opaque)
{
    MainCanContext *can_context = opaque;

    while (!main_stop_requested) {
        bool logger_ready;
        bool stop;

        if (pthread_mutex_lock(&can_context->logger_mutex) != 0) {
            break;
        }
        stop = can_context->logger_thread_stop_requested;
        logger_ready = can_context->logger_ready;
        if (!stop && !logger_ready) {
            Logger *logger = can_context->logger_instance;

            if (logger != NULL && logger_init(logger) == 0 &&
                logger_start(logger) == 0) {
                const char *session = logger_get_session_directory(logger);

                can_context->logger_ready = true;
                if (session != NULL) {
                    (void)snprintf(can_context->logger_session_directory,
                                   sizeof(can_context->logger_session_directory),
                                   "%s", session);
                }
                LOG_INFO("Logger ready: session=%s",
                         can_context->logger_session_directory);
            } else if (logger != NULL) {
                logger_stop(logger);
            }
        } else if (!stop && logger_ready &&
                   !logger_is_healthy(can_context->logger_instance)) {
            LOG_ERROR("Logger became unhealthy; entering degraded mode");
            logger_stop(can_context->logger_instance);
            can_context->logger_ready = false;
            can_context->logger_session_directory[0] = '\0';
        }
        pthread_mutex_unlock(&can_context->logger_mutex);
        if (stop) {
            break;
        }
        main_update_core_status(can_context, 0U, false, false);
        main_logger_sleep(can_context);
    }
    return NULL;
}

static void main_stop_logger_supervisor(MainCanContext *can_context)
{
    if (can_context == NULL) {
        return;
    }
    if (pthread_mutex_lock(&can_context->logger_mutex) == 0) {
        can_context->logger_thread_stop_requested = true;
        pthread_mutex_unlock(&can_context->logger_mutex);
    }
    if (can_context->logger_thread_started) {
        pthread_join(can_context->logger_thread, NULL);
        can_context->logger_thread_started = false;
    }
    if (pthread_mutex_lock(&can_context->logger_mutex) == 0) {
        if (can_context->logger_ready && can_context->logger_instance != NULL) {
            logger_stop(can_context->logger_instance);
            logger_print_stats(can_context->logger_instance);
        }
        can_context->logger_ready = false;
        pthread_mutex_unlock(&can_context->logger_mutex);
    }
}

static void main_can_tick(void *context)
{
    MainCanContext *can_context = context;
    uint32_t can_ready_mask = 0U;
    bool logger_ready;
    bool lappro_ready;

    if (can_context == NULL) {
        return;
    }
    logger_ready = main_logger_is_ready(can_context);
    lappro_ready = main_lappro_get_ready(can_context);
    if (logger_ready && !lappro_ready && can_context->lappro != NULL &&
        can_context->lappro_config != NULL) {
        char session_directory[RACE_CORE_STATUS_PATH_MAX];

        memset(session_directory, 0, sizeof(session_directory));
        if (pthread_mutex_lock(&can_context->logger_mutex) == 0) {
            (void)snprintf(session_directory, sizeof(session_directory), "%s",
                           can_context->logger_session_directory);
            pthread_mutex_unlock(&can_context->logger_mutex);
        }
        if (session_directory[0] != '\0') {
            race_event_close(&can_context->race_event);
            race_event_init(&can_context->race_event, session_directory);
        }
        if (lappro_init(can_context->lappro, can_context->lappro_config) == 0) {
            lappro_set_sample_sink(can_context->lappro,
                                   main_lappro_sample_sink, can_context);
            if (lappro_start(can_context->lappro) == 0) {
                main_lappro_set_ready(can_context, true);
                LOG_INFO("LapPro started after logger became ready");
            } else {
                lappro_stop(can_context->lappro);
            }
        }
    } else if (!logger_ready && lappro_ready) {
        lappro_stop(can_context->lappro);
        main_lappro_set_ready(can_context, false);
        LOG_WARN("LapPro stopped because logger is unavailable");
    }
    if (main_active_receiver != NULL) {
        can_ready_mask = can_receiver_active_mask(main_active_receiver);
    }
    main_update_core_status(can_context, can_ready_mask, true, false);
}
#endif

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

#ifdef RACE_CORE_DAEMON
static void main_bms_state_snapshot_locked(MainCanContext *can_context,
                                           uint64_t timestamp_us,
                                           BmsState *state)
{
    bms_parser_snapshot(&can_context->bms_parser, timestamp_us, state);
}
#endif

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

    if ((state->valid_mask & required_mask) == required_mask) {
#ifdef RACE_CORE_DAEMON
        main_logger_vehicle_state_sink(timestamp_us, state, can_context);
#else
        if (can_context->logger != NULL) {
            logger_vehicle_state_sink(timestamp_us, state, can_context->logger);
        }
#endif
    }
    if (can_context->hmi != NULL) {
        (void)hmi_update(can_context->hmi, timestamp_us, state);
    }
#ifdef RACE_CORE_DAEMON
    {
        BmsState bms_state;

        pthread_mutex_lock(&can_context->mutex);
        main_bms_state_snapshot_locked(can_context, timestamp_us, &bms_state);
        pthread_mutex_unlock(&can_context->mutex);
        if (can_context->bms_shared_state.mapping != NULL &&
            race_shared_bms_state_publish(&can_context->bms_shared_state,
                                           &bms_state, main_monotonic_us()) < 0) {
            LOG_WARN("BMS shared-state publish failed: %s", strerror(errno));
        }
    }
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
                        VEHICLE_STATE_VALID_DRIVE_STATE |
                        VEHICLE_STATE_VALID_MOTOR_TEMPERATURE_C |
                        VEHICLE_STATE_VALID_CONTROLLER_TEMPERATURE_C |
                        VEHICLE_STATE_VALID_BATTERY_TEMPERATURE_C |
                        VEHICLE_STATE_VALID_MODE;
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
    state->motor_temperature_c = 72.0;
    state->controller_temperature_c = 64.0;
    state->battery_temperature_c = 31.0;
    state->hv_voltage_v = 356.0;
    state->hv_current_a = 118.0;
    state->hv_power_w = 42000.0;
    state->hv_soc_percent = 78.0;
    state->lv_voltage_v = 13.6;
    state->throttle_percent = 68.0;
    state->brake_percent = 18.0;
    state->mode = 1U;
    state->drive_state = VEHICLE_DRIVE_STATE_READY;
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
#ifdef RACE_CORE_DAEMON
    main_logger_message_sink(message, can_context);
#else
    if (can_context->logger != NULL) {
        logger_message_sink(message, can_context->logger);
    }
#endif

    pthread_mutex_lock(&can_context->mutex);
    vehicle_state_clock_note_event(&can_context->clock,
                                   message->timestamp_us);
    (void)can_decoder_process(&can_context->decoder, message,
                              &can_context->vehicle_state);
    (void)bms_parser_process(&can_context->bms_parser, message);
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

#ifdef RACE_CORE_DAEMON
    main_logger_lappro_sink(timestamp_us, sample, can_context);
#else
    if (can_context->logger != NULL) {
        logger_lappro_sink(timestamp_us, sample, can_context->logger);
    }
#endif

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
    options->soc_initial_percent = CAN_DECODER_DEFAULT_SOC_INITIAL_PERCENT;
    options->battery_usable_capacity_ah = 0.0;
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
        if (strcmp(argv[argument_index], "--soc-initial-percent") == 0) {
            if (argument_index + 1 >= argc ||
                !main_parse_speed(argv[++argument_index],
                                  &options->soc_initial_percent) ||
                options->soc_initial_percent > 100.0) {
                LOG_ERROR("Invalid --soc-initial-percent; use 0..100");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[argument_index], "--battery-capacity-ah") == 0) {
            if (argument_index + 1 >= argc ||
                !main_parse_speed(argv[++argument_index],
                                  &options->battery_usable_capacity_ah) ||
                options->battery_usable_capacity_ah <= 0.0) {
                LOG_ERROR("Invalid --battery-capacity-ah; use a positive value");
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
                  "[--replay-speed <number>] "
                  "[--soc-initial-percent <0..100>] "
                  "[--battery-capacity-ah <positive>]", argv[0]);
#else
        LOG_ERROR("Usage: %s [-v|--verbose] [--replay <directory>] "
                  "[--replay-speed <number>] "
                  "[--soc-initial-percent <0..100>] "
                  "[--battery-capacity-ah <positive>] [--video] "
                  "[--camera <path>] "
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
    CanDecoderConfig decoder_config;
#ifndef RACE_CORE_DAEMON
    bool lappro_ready = false;
    bool logger_ready = false;
#endif
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
    console_log_init();
    if (main_parse_arguments(argc, argv, &options) < 0) {
        return EXIT_FAILURE;
    }
    can_decoder_config_default(&decoder_config);
    decoder_config.soc_initial_percent = options.soc_initial_percent;
    decoder_config.battery_usable_capacity_ah =
        options.battery_usable_capacity_ah;
    can_decoder_init_with_config(&can_context.decoder, &decoder_config);
    bms_parser_init(&can_context.bms_parser);
    if (options.battery_usable_capacity_ah <= 0.0) {
        LOG_WARN("HV SOC disabled until --battery-capacity-ah is configured");
    }
#ifndef RACE_CORE_DAEMON
    if (main_install_signals() < 0) {
        LOG_ERROR("Cannot install video termination handlers: %s",
                  strerror(errno));
        return EXIT_FAILURE;
    }
#endif
#ifdef RACE_CORE_DAEMON
    if (main_core_install_signals() < 0) {
        LOG_ERROR("Cannot install core termination handlers: %s",
                  strerror(errno));
        return EXIT_FAILURE;
    }
    if (race_shared_state_writer_open(&can_context.shared_state, NULL) < 0) {
        LOG_ERROR("Cannot create %s: %s", RACE_SHARED_STATE_NAME,
                  strerror(errno));
        return EXIT_FAILURE;
    }
    if (race_shared_bms_state_writer_open(&can_context.bms_shared_state,
                                          NULL) < 0) {
        LOG_ERROR("Cannot create %s: %s", RACE_BMS_SHARED_STATE_NAME,
                  strerror(errno));
        race_shared_state_close(&can_context.shared_state);
        return EXIT_FAILURE;
    }
    if (race_core_status_writer_open(&can_context.core_status, NULL) < 0) {
        LOG_ERROR("Cannot create %s: %s", RACE_CORE_STATUS_NAME,
                  strerror(errno));
        race_shared_bms_state_close(&can_context.bms_shared_state);
        race_shared_state_close(&can_context.shared_state);
        return EXIT_FAILURE;
    }
    LOG_INFO("Shared VehicleState ready: name=%s version=%u size=%zu",
             RACE_SHARED_STATE_NAME, RACE_SHM_VERSION,
             sizeof(RaceSharedState));
    LOG_INFO("Shared BmsState ready: name=%s version=%u size=%zu",
             RACE_BMS_SHARED_STATE_NAME, RACE_BMS_SHM_VERSION,
             sizeof(RaceSharedBmsState));
#endif
    vehicle_state_clock_init(&can_context.clock, options.replay);
    context_mutex_result = pthread_mutex_init(&can_context.mutex, NULL);
    if (context_mutex_result != 0) {
        LOG_ERROR("Cannot initialize VehicleState mutex: %s",
                  strerror(context_mutex_result));
#ifdef RACE_CORE_DAEMON
        race_shared_bms_state_close(&can_context.bms_shared_state);
        race_shared_state_close(&can_context.shared_state);
        race_core_status_close(&can_context.core_status);
#endif
        return EXIT_FAILURE;
    }
#ifdef RACE_CORE_DAEMON
    int logger_mutex_result = pthread_mutex_init(&can_context.logger_mutex, NULL);
    int status_mutex_result = pthread_mutex_init(&can_context.status_mutex, NULL);
    if (logger_mutex_result != 0 || status_mutex_result != 0) {
        LOG_ERROR("Cannot initialize core lifecycle mutexes");
        if (logger_mutex_result == 0) {
            pthread_mutex_destroy(&can_context.logger_mutex);
        }
        if (status_mutex_result == 0) {
            pthread_mutex_destroy(&can_context.status_mutex);
        }
        pthread_mutex_destroy(&can_context.mutex);
        race_shared_bms_state_close(&can_context.bms_shared_state);
        race_shared_state_close(&can_context.shared_state);
        race_core_status_close(&can_context.core_status);
        return EXIT_FAILURE;
    }
#endif
#ifdef RACE_CORE_DAEMON
    can_context.logger_instance = &logger;
#else
    if (logger_init(&logger) < 0) {
        LOG_WARN("Data logger unavailable; continuing without file logging");
    } else if (logger_start(&logger) < 0) {
        logger_stop(&logger);
        LOG_WARN("Data logger did not start; continuing without file logging");
    } else {
        logger_ready = true;
    }
#endif
#ifndef RACE_CORE_DAEMON
    can_context.logger = logger_ready ? &logger : NULL;
#else
    can_context.logger = NULL;
#endif
    if (hmi_init(&hmi, HMI_DEFAULT_DEVICE_PATH) < 0) {
        LOG_WARN("HMI unavailable; continuing without display output");
    } else {
        hmi_ready = true;
    }
    can_context.hmi = hmi_ready ? &hmi : NULL;
    race_event_init(&can_context.race_event,
#ifdef RACE_CORE_DAEMON
                    NULL);
#else
                    logger_ready ? logger_get_session_directory(&logger)
                                 : NULL);
#endif

#ifdef RACE_CORE_DAEMON
    can_context.lappro = &lappro;
    can_context.lappro_config = &lappro_config;
    main_update_core_status(&can_context, 0U, true, false);
    if (pthread_create(&can_context.logger_thread, NULL,
                       main_logger_thread_main, &can_context) != 0) {
        LOG_ERROR("Cannot start logger supervisor thread");
        race_core_status_close(&can_context.core_status);
        pthread_mutex_destroy(&can_context.status_mutex);
        pthread_mutex_destroy(&can_context.logger_mutex);
        pthread_mutex_destroy(&can_context.mutex);
        race_shared_bms_state_close(&can_context.bms_shared_state);
        race_shared_state_close(&can_context.shared_state);
        return EXIT_FAILURE;
    }
    can_context.logger_thread_started = true;
#endif

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
        video_config.video_ipc_name = RACE_VIDEO_IPC_NAME;
        video_config.video_publish_fps = VIDEO_PIPELINE_DEFAULT_WEB_PUBLISH_FPS;
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
#ifndef RACE_CORE_DAEMON
        if (logger_ready) {
            logger_stop(&logger);
            logger_print_stats(&logger);
        }
#else
        main_update_core_status(&can_context, 0U, true, true);
        main_stop_logger_supervisor(&can_context);
#endif
        if (hmi_ready) {
            hmi_close(&hmi);
        }
        sync();
        pthread_mutex_destroy(&can_context.mutex);
#ifdef RACE_CORE_DAEMON
        race_shared_bms_state_close(&can_context.bms_shared_state);
        race_shared_state_close(&can_context.shared_state);
        race_core_status_close(&can_context.core_status);
        pthread_mutex_destroy(&can_context.status_mutex);
        pthread_mutex_destroy(&can_context.logger_mutex);
#endif
        if (result < 0) {
            LOG_ERROR("Replay stopped with an error");
            return EXIT_FAILURE;
        }
        LOG_INFO("Replay stopped cleanly");
        return EXIT_SUCCESS;
    }

    lappro_config_default(&lappro_config);
#ifdef RACE_CORE_DAEMON
    /* LapPro is deliberately held until logger storage is ready. The CAN
     * receiver and snapshotter still run during PRECHECK so HMI remains live. */
#else
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
#endif

#ifdef RACE_CORE_DAEMON
    if (vehicle_state_snapshotter_init(
            &snapshotter, main_vehicle_state_snapshot_provider,
            main_vehicle_state_snapshot_sink, &can_context) < 0 ||
        vehicle_state_snapshotter_start(&snapshotter) < 0) {
        vehicle_state_snapshotter_stop(&snapshotter);
        LOG_ERROR("VehicleState snapshotter did not start");
        result = -1;
        goto core_cleanup;
    }
    snapshotter_ready = true;
    result = 0;
    for (;;) {
        if (main_stop_requested) {
            break;
        }
        if (can_receiver_init(&receiver, interfaces) < 0) {
            /* Initialization installs the receiver's legacy signal handler
             * before it knows whether both interfaces exist. Restore the
             * daemon handler even on this retry path. */
            (void)main_core_install_signals();
            main_update_core_status(&can_context, 0U, true, false);
            main_logger_sleep(&can_context);
            continue;
        }
        main_active_receiver = &receiver;
        can_receiver_set_message_sink(&receiver, main_can_message_sink,
                                      &can_context);
        can_receiver_set_tick_callback(&receiver, main_can_tick,
                                       &can_context);
        /* can_receiver_init installs its legacy signal handler. Restore the
         * core handler so a SIGTERM also wakes the lifecycle supervisor. */
        if (main_core_install_signals() < 0) {
            LOG_ERROR("Cannot restore core signal handlers: %s",
                      strerror(errno));
            can_receiver_close(&receiver);
            main_active_receiver = NULL;
            result = -1;
            break;
        }
        main_update_core_status(&can_context,
                                can_receiver_active_mask(&receiver),
                                true, false);
        result = can_receiver_run(&receiver);
        can_receiver_print_stats(&receiver);
        can_receiver_close(&receiver);
        main_active_receiver = NULL;
        if (main_stop_requested) {
            break;
        }
        LOG_WARN("CAN receiver stopped; retrying both interfaces");
        main_update_core_status(&can_context, 0U, true, false);
        main_logger_sleep(&can_context);
    }
    goto core_cleanup;
#else
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
        race_shared_bms_state_close(&can_context.bms_shared_state);
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
#endif
#ifdef RACE_CORE_DAEMON
core_cleanup:
#endif
    #ifndef RACE_CORE_DAEMON
    if (video_ready) {
        video_pipeline_stop(&video_pipeline);
        video_pipeline_print_stats(&video_pipeline);
    }
    #endif
#ifdef RACE_CORE_DAEMON
    main_update_core_status(&can_context, 0U, true, true);
    if (snapshotter_ready) {
        vehicle_state_snapshotter_stop(&snapshotter);
    }
    if (main_lappro_get_ready(&can_context)) {
        lappro_stop(&lappro);
        main_lappro_set_ready(&can_context, false);
    }
    main_stop_logger_supervisor(&can_context);
    race_event_close(&can_context.race_event);
#else
    race_event_close(&can_context.race_event);
    if (logger_ready) {
        logger_stop(&logger);
        logger_print_stats(&logger);
    }
#endif
    if (hmi_ready) {
        hmi_close(&hmi);
    }
    sync();
    pthread_mutex_destroy(&can_context.mutex);
#ifdef RACE_CORE_DAEMON
    race_shared_bms_state_close(&can_context.bms_shared_state);
    race_shared_state_close(&can_context.shared_state);
    race_core_status_close(&can_context.core_status);
    pthread_mutex_destroy(&can_context.status_mutex);
    pthread_mutex_destroy(&can_context.logger_mutex);
#endif

    if (result < 0) {
        LOG_ERROR("CAN receiver stopped with an error");
        return EXIT_FAILURE;
    }

    LOG_INFO("CAN receiver stopped cleanly");
    return EXIT_SUCCESS;
}
