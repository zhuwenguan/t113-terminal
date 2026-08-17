#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "can_decoder.h"
#include "can_receiver.h"
#include "console_log.h"
#include "lap_logic.h"
#include "lappro.h"
#include "logger.h"
#include "replay.h"

typedef struct {
    pthread_mutex_t mutex;
    Logger *logger;
    LapLogic lap_logic;
    VehicleState vehicle_state;
} MainCanContext;

typedef struct {
    bool replay;
    char replay_directory[PATH_MAX];
    double replay_speed;
} MainOptions;

static void main_log_vehicle_state_locked(MainCanContext *can_context,
                                          uint64_t timestamp_us)
{
    const uint32_t required_mask = VEHICLE_STATE_CAN_REQUIRED_MASK |
                                    VEHICLE_STATE_LAPPRO_REQUIRED_MASK;
    VehicleState *state = &can_context->vehicle_state;

    state->can_valid = (state->valid_mask & VEHICLE_STATE_CAN_REQUIRED_MASK) ==
                       VEHICLE_STATE_CAN_REQUIRED_MASK;
    state->gps_valid =
        (state->valid_mask & VEHICLE_STATE_LAPPRO_REQUIRED_MASK) ==
        VEHICLE_STATE_LAPPRO_REQUIRED_MASK;

    if (can_context->logger == NULL ||
        (state->valid_mask & required_mask) != required_mask) {
        return;
    }

    state->timestamp_us = timestamp_us;
    logger_vehicle_state_sink(timestamp_us, state, can_context->logger);
}

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
    if (can_decoder_process(message, &can_context->vehicle_state)) {
        main_log_vehicle_state_locked(can_context,
                                      can_context->vehicle_state.timestamp_us);
    }
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
    can_context->vehicle_state.timestamp_us = timestamp_us;
    can_context->vehicle_state.gps_fix_type = sample->fix_type;
    can_context->vehicle_state.gps_num_sv = sample->num_sv;
    if (sample->fix_type >= 3U) {
        can_context->vehicle_state.speed_kmh = sample->speed_kmh;
        can_context->vehicle_state.heading_deg = sample->heading_deg;
        can_context->vehicle_state.hacc_m = sample->hacc_m;
        can_context->vehicle_state.altitude = sample->altitude;
        can_context->vehicle_state.longitude = sample->longitude;
        can_context->vehicle_state.latitude = sample->latitude;
        can_context->vehicle_state.acc_x = sample->acc_x;
        can_context->vehicle_state.acc_y = sample->acc_y;
        can_context->vehicle_state.acc_z = sample->acc_z;
        can_context->vehicle_state.valid_mask |= lappro_valid_mask;
    } else {
        can_context->vehicle_state.valid_mask &= ~lappro_valid_mask;
    }
    lap_logic_update(&can_context->lap_logic, timestamp_us, sample,
                     &can_context->vehicle_state);
    main_log_vehicle_state_locked(can_context, timestamp_us);
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

        LOG_ERROR("Usage: %s [-v|--verbose] [--replay <directory>] "
                  "[--replay-speed <number>]",
                  argv[0]);
        return -1;
    }

    return 0;
}

static void main_get_track_config_path(const MainOptions *options,
                                       char *path, size_t path_length)
{
    int written;

    if (options != NULL && options->replay) {
        written = snprintf(path, path_length, "%s/track.json",
                           options->replay_directory);
        if (written >= 0 && (size_t)written < path_length &&
            access(path, R_OK) == 0) {
            return;
        }
    }
    snprintf(path, path_length, "%s", "track.json");
}

int main(int argc, char *argv[])
{
    static const char *const interfaces[CAN_RECEIVER_BUS_COUNT] = {
        "awlink0",
        "awlink1"
    };
    CanReceiver receiver;
    Logger logger;
    LapProConfig lappro_config;
    LapPro lappro;
    MainCanContext can_context;
    MainOptions options;
    bool lappro_ready = false;
    bool logger_ready = false;
    char track_config_path[PATH_MAX];
    int context_mutex_result;
    int result;

    memset(&can_context, 0, sizeof(can_context));
    console_log_init();
    if (main_parse_arguments(argc, argv, &options) < 0) {
        return EXIT_FAILURE;
    }
    context_mutex_result = pthread_mutex_init(&can_context.mutex, NULL);
    if (context_mutex_result != 0) {
        LOG_ERROR("Cannot initialize VehicleState mutex: %s",
                  strerror(context_mutex_result));
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
    main_get_track_config_path(&options, track_config_path,
                               sizeof(track_config_path));
    if (lap_logic_init(&can_context.lap_logic, track_config_path,
                       logger_ready ? LOGGER_LOG_DIRECTORY : NULL) < 0) {
        LOG_WARN("Lap logic initialization failed; continuing without lap updates");
    }

    if (options.replay) {
        ReplayConfig replay_config;

        memset(&replay_config, 0, sizeof(replay_config));
        replay_config.directory = options.replay_directory;
        replay_config.speed = options.replay_speed;
        replay_config.can_sink = main_can_message_sink;
        replay_config.lappro_sink = main_lappro_sample_sink;
        replay_config.sink_context = &can_context;
        replay_config.vehicle_state = &can_context.vehicle_state;
        result = replay_run(&replay_config);
        lap_logic_close(&can_context.lap_logic);
        if (logger_ready) {
            logger_stop(&logger);
            logger_print_stats(&logger);
        }
        sync();
        pthread_mutex_destroy(&can_context.mutex);
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
        if (lappro_ready) {
            lappro_stop(&lappro);
        }
        lap_logic_close(&can_context.lap_logic);
        if (logger_ready) {
            logger_stop(&logger);
        }
        pthread_mutex_destroy(&can_context.mutex);
        return EXIT_FAILURE;
    }

    can_receiver_set_message_sink(&receiver, main_can_message_sink,
                                  &can_context);
    result = can_receiver_run(&receiver);
    can_receiver_print_stats(&receiver);
    can_receiver_close(&receiver);
    if (lappro_ready) {
        lappro_stop(&lappro);
    }
    lap_logic_close(&can_context.lap_logic);
    if (logger_ready) {
        logger_stop(&logger);
        logger_print_stats(&logger);
    }
    sync();
    pthread_mutex_destroy(&can_context.mutex);

    if (result < 0) {
        LOG_ERROR("CAN receiver stopped with an error");
        return EXIT_FAILURE;
    }

    LOG_INFO("CAN receiver stopped cleanly");
    return EXIT_SUCCESS;
}
