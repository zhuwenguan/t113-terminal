#define _POSIX_C_SOURCE 200809L

#include <race/shared_state.h>
#include <race/time_utils.h>

#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int parse_unsigned(const char *text, unsigned long *value)
{
    char *end;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value = parsed;
    return 0;
}

static void fill_state(VehicleState *state, uint64_t timestamp_us,
                       uint64_t sample)
{
    double phase = (double)(sample % 200U) / 200.0;

    memset(state, 0, sizeof(*state));
    state->timestamp_us = timestamp_us;
    state->valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                        VEHICLE_STATE_VALID_HEADING_DEG |
                        VEHICLE_STATE_VALID_LONGITUDE_DEG |
                        VEHICLE_STATE_VALID_LATITUDE_DEG |
                        VEHICLE_STATE_VALID_MOTOR_RPM |
                        VEHICLE_STATE_VALID_TORQUE_NM |
                        VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                        VEHICLE_STATE_VALID_HV_CURRENT_A |
                        VEHICLE_STATE_VALID_HV_POWER_W |
                        VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                        VEHICLE_STATE_VALID_LV_VOLTAGE_V |
                        VEHICLE_STATE_VALID_GPS_FIX_TYPE |
                        VEHICLE_STATE_VALID_GPS_NUM_SV |
                        VEHICLE_STATE_VALID_THROTTLE_PERCENT |
                        VEHICLE_STATE_VALID_BRAKE_PERCENT |
                        VEHICLE_STATE_VALID_DRIVE_STATE |
                        VEHICLE_STATE_VALID_MODE;
    state->can_valid = true;
    state->gps_valid = true;
    state->gps_fix_type = 3U;
    state->gps_num_sv = 12U;
    state->speed_kmh = 45.0 + phase * 90.0;
    state->heading_deg = phase * 360.0;
    state->latitude = 31.2304 + phase * 0.001;
    state->longitude = 121.4737 + phase * 0.001;
    state->acc_x = (phase - 0.5) * 2.0;
    state->acc_y = 0.3;
    state->acc_z = 9.81;
    state->motor_rpm = 1800.0 + state->speed_kmh * 50.0;
    state->torque_nm = 80.0 + phase * 40.0;
    state->motor_temperature_c = 55.0 + phase * 10.0;
    state->controller_temperature_c = 48.0 + phase * 8.0;
    state->battery_temperature_c = 28.0 + phase * 4.0;
    state->hv_voltage_v = 352.0 + phase * 4.0;
    state->hv_current_a = 50.0 + phase * 20.0;
    state->hv_power_w = state->hv_voltage_v * state->hv_current_a;
    state->hv_soc_percent = 82.0 - phase * 0.5;
    state->lv_voltage_v = 13.6;
    state->throttle_percent = 25.0 + phase * 50.0;
    state->brake_percent = phase < 0.8 ? 0.0 : (phase - 0.8) * 500.0;
    state->drive_state = VEHICLE_DRIVE_STATE_READY;
    state->mode = 1U;
    state->lap_number = (uint32_t)(sample / 1800U) + 1U;
    state->lap_valid = true;
    state->lap_time_s = (double)(sample % 1800U) * 0.05;
    state->best_lap_time_s = 82.9;
    state->delta_time_s = phase - 0.5;
    state->track_distance_m = (double)(sample % 1800U) * 2.1;
}

int main(int argc, char **argv)
{
    const char *name = RACE_SHARED_STATE_NAME;
    unsigned long rate_hz = 20U;
    unsigned long duration_s = 0U;
    bool unlink_on_exit = false;
    RaceSharedStateHandle shared;
    uint64_t start_us;
    uint64_t deadline_us;
    uint64_t sample = 0U;
    int index;

    memset(&shared, 0, sizeof(shared));
    shared.fd = -1;
    for (index = 1; index < argc; ++index) {
        if ((strcmp(argv[index], "--name") == 0 ||
             strcmp(argv[index], "--state-shm-name") == 0) &&
            index + 1 < argc) {
            name = argv[++index];
        } else if (strcmp(argv[index], "--rate-hz") == 0 &&
                   index + 1 < argc) {
            if (parse_unsigned(argv[++index], &rate_hz) < 0 ||
                rate_hz == 0U || rate_hz > 1000U) {
                fprintf(stderr, "fake_state_producer: invalid rate\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--seconds") == 0 &&
                   index + 1 < argc) {
            if (parse_unsigned(argv[++index], &duration_s) < 0) {
                fprintf(stderr, "fake_state_producer: invalid duration\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--unlink-on-exit") == 0) {
            unlink_on_exit = true;
        } else if (strcmp(argv[index], "--help") == 0 ||
                   strcmp(argv[index], "-h") == 0) {
            fprintf(stdout,
                    "Usage: %s [--name /shm] [--rate-hz HZ] [--seconds N] "
                    "[--unlink-on-exit]\n", argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "fake_state_producer: unknown argument %s\n",
                    argv[index]);
            return EXIT_FAILURE;
        }
    }
    if (name[0] != '/') {
        fprintf(stderr, "fake_state_producer: shared memory name must start with /\n");
        return EXIT_FAILURE;
    }
    if (signal(SIGINT, handle_signal) == SIG_ERR ||
        signal(SIGTERM, handle_signal) == SIG_ERR) {
        perror("fake_state_producer: signal");
        return EXIT_FAILURE;
    }
    if (race_shared_state_writer_open(&shared, name) < 0) {
        fprintf(stderr, "fake_state_producer: cannot open %s: %s\n", name,
                strerror(errno));
        return EXIT_FAILURE;
    }
    start_us = 0U;
    if (race_monotonic_us(&start_us) < 0) {
        fprintf(stderr, "fake_state_producer: clock failed: %s\n",
                strerror(errno));
        race_shared_state_close(&shared);
        return EXIT_FAILURE;
    }
    deadline_us = start_us;
    while (!stop_requested) {
        VehicleState state;
        uint64_t now_us;

        if (race_monotonic_us(&now_us) < 0) {
            break;
        }
        if (duration_s != 0U && now_us >= start_us &&
            now_us - start_us >= (uint64_t)duration_s * 1000000ULL) {
            break;
        }
        fill_state(&state, now_us, sample++);
        if (race_shared_state_publish(&shared, &state, now_us) < 0) {
            fprintf(stderr, "fake_state_producer: publish failed: %s\n",
                    strerror(errno));
            break;
        }
        deadline_us += 1000000ULL / rate_hz;
        if (now_us > deadline_us) {
            deadline_us = now_us + 1000000ULL / rate_hz;
        }
        {
            struct timespec deadline = {
                (time_t)(deadline_us / 1000000ULL),
                (long)((deadline_us % 1000000ULL) * 1000ULL)
            };
            while (!stop_requested &&
                   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                   &deadline, NULL) == EINTR) {
            }
        }
    }
    race_shared_state_close(&shared);
    if (unlink_on_exit) {
        (void)shm_unlink(name);
    }
    return EXIT_SUCCESS;
}
