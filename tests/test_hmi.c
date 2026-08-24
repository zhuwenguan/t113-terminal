#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "console_log.h"
#include "hmi.h"

#define CAPTURE_BUFFER_SIZE 8192U

typedef struct {
    unsigned char buffer[CAPTURE_BUFFER_SIZE];
    size_t length;
    unsigned int write_calls;
    unsigned int drain_calls;
} Capture;

static ssize_t capture_write(void *context, const void *buffer, size_t length)
{
    Capture *capture = context;
    size_t accepted = length > 7U ? 7U : length;

    assert(capture != NULL);
    assert(capture->length + accepted <= sizeof(capture->buffer));
    memcpy(capture->buffer + capture->length, buffer, accepted);
    capture->length += accepted;
    capture->write_calls++;
    return (ssize_t)accepted;
}

static int capture_drain(void *context)
{
    Capture *capture = context;

    assert(capture != NULL);
    capture->drain_calls++;
    return 0;
}

static void assert_commands_are_terminated(const Capture *capture,
                                           size_t expected_commands)
{
    size_t offset = 0U;
    size_t command_count = 0U;

    while (offset < capture->length) {
        size_t command_end = offset;

        while (command_end + HMI_COMMAND_TERMINATOR_LENGTH <=
                   capture->length &&
               !(capture->buffer[command_end] == 0xFFU &&
                 capture->buffer[command_end + 1U] == 0xFFU &&
                 capture->buffer[command_end + 2U] == 0xFFU)) {
            command_end++;
        }
        assert(command_end + HMI_COMMAND_TERMINATOR_LENGTH <=
               capture->length);
        offset = command_end + HMI_COMMAND_TERMINATOR_LENGTH;
        command_count++;
    }
    assert(command_count == expected_commands);
    assert(capture->length >= HMI_COMMAND_TERMINATOR_LENGTH);
    assert(capture->buffer[capture->length - 3U] == 0xFFU);
    assert(capture->buffer[capture->length - 2U] == 0xFFU);
    assert(capture->buffer[capture->length - 1U] == 0xFFU);
}

static void make_test_state(VehicleState *state)
{
    memset(state, 0, sizeof(*state));
    state->timestamp_us = 1000000ULL;
    state->valid_mask = VEHICLE_STATE_VALID_SPEED_KMH |
                        VEHICLE_STATE_VALID_MOTOR_RPM |
                        VEHICLE_STATE_VALID_HV_VOLTAGE_V |
                        VEHICLE_STATE_VALID_HV_CURRENT_A |
                        VEHICLE_STATE_VALID_HV_POWER_W |
                        VEHICLE_STATE_VALID_HV_SOC_PERCENT |
                        VEHICLE_STATE_VALID_TORQUE_NM |
                        VEHICLE_STATE_VALID_LV_VOLTAGE_V;
    state->motor_rpm = 1234.0;
    state->speed_kmh = 52.3;
    state->hv_soc_percent = 78.5;
    state->hv_voltage_v = 523.4;
    state->hv_current_a = 12.0;
    state->hv_power_w = 6276.0;
    state->torque_nm = 321.0;
    state->lv_voltage_v = 13.8;
    state->lap_valid = true;
    state->lap_number = 3U;
    state->lap_time_s = 82.12;
    state->delta_time_s = -0.34;
    state->accel_distance_m = 50.0;
    state->accel_50m_time_s = 3.21;
    state->accel_time_s = 5.67;
    state->max_acceleration = 1.95;
}

static uint64_t monotonic_us(void)
{
    struct timespec now;

    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static void test_serial_failure_is_nonblocking(const VehicleState *state)
{
    Hmi serial_hmi;
    char *slave_path;
    uint64_t start_us;
    int master_fd;

    master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master_fd >= 0);
    assert(grantpt(master_fd) == 0);
    assert(unlockpt(master_fd) == 0);
    slave_path = ptsname(master_fd);
    assert(slave_path != NULL);
    assert(hmi_init(&serial_hmi, slave_path) == 0);
    assert((fcntl(serial_hmi.fd, F_GETFL) & O_NONBLOCK) != 0);
    assert(serial_hmi.transport.drain == NULL);

    assert(close(master_fd) == 0);
    start_us = monotonic_us();
    assert(hmi_update(&serial_hmi, state->timestamp_us, state) < 0);
    assert(monotonic_us() - start_us < 100000ULL);
    hmi_close(&serial_hmi);
}

int main(int argc, char *argv[])
{
    Capture capture;
    HmiTransport transport;
    Hmi hmi;
    VehicleState state;
    size_t mapping_count;
    size_t first_length;
    int sent;

    assert(argc <= 2);
    make_test_state(&state);
    if (argc == 2) {
        console_log_set_level(CONSOLE_LOG_DEBUG);
        assert(hmi_init(&hmi, argv[1]) == 0);
        assert((fcntl(hmi.fd, F_GETFL) & O_NONBLOCK) != 0);
        assert(hmi.transport.drain == NULL);
        assert(hmi_update(&hmi, state.timestamp_us, &state) == HMI_FIELD_COUNT);
        hmi_close(&hmi);
        return 0;
    }

    memset(&capture, 0, sizeof(capture));
    transport.write = capture_write;
    transport.drain = capture_drain;
    transport.context = &capture;
    assert(hmi_init_transport(&hmi, &transport) == 0);

    sent = hmi_update(&hmi, state.timestamp_us, &state);
    assert(sent == HMI_FIELD_COUNT);
    assert(capture.write_calls > 1U);
    assert(capture.drain_calls == 1U);
    first_length = capture.length;
    assert(memcmp(capture.buffer, "t0.txt=\"1234\"\xff\xff\xff",
                  strlen("t0.txt=\"1234\"\xff\xff\xff")) == 0);
    assert_commands_are_terminated(&capture, HMI_FIELD_COUNT);

    assert(hmi_update(&hmi, state.timestamp_us + 1000U, &state) == 0);
    assert(capture.length == first_length);

    state.motor_rpm = 1235.0;
    state.speed_kmh = 53.0;
    state.hv_soc_percent = 78.6;
    sent = hmi_update(&hmi, state.timestamp_us + 50000ULL, &state);
    assert(sent == 2);
    assert(capture.drain_calls == 2U);
    assert_commands_are_terminated(&capture, HMI_FIELD_COUNT + 2U);

    state.hv_soc_percent = 78.7;
    assert(hmi_update(&hmi, state.timestamp_us + 100000ULL, &state) == 0);
    assert(hmi_update(&hmi, state.timestamp_us + 1000000ULL, &state) == 1);
    assert(capture.drain_calls == 3U);

    assert(hmi_get_field_map(&mapping_count) != NULL);
    assert(mapping_count == HMI_FIELD_COUNT);
    hmi_close(&hmi);
    test_serial_failure_is_nonblocking(&state);
    return 0;
}
