#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "hmi.h"
#include "console_log.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define HMI_TX_BUFFER_SIZE 2048U
#define HMI_REFRESH_DYNAMIC_US 50000ULL
#define HMI_REFRESH_LAP_US 100000ULL
#define HMI_REFRESH_MEDIUM_US 200000ULL
#define HMI_REFRESH_SLOW_US 500000ULL
#define HMI_REFRESH_VERY_SLOW_US 1000000ULL

static const HmiFieldDefinition hmi_field_map[HMI_FIELD_COUNT] = {
    /* TJC/Nextion component mapping. Change this table when the screen page changes. */
    [HMI_FIELD_RPM] = {
        HMI_FIELD_RPM, "t0", HMI_FIELD_FORMAT_RPM, HMI_REFRESH_DYNAMIC_US,
        VEHICLE_STATE_VALID_MOTOR_RPM
    },
    [HMI_FIELD_SPEED] = {
        HMI_FIELD_SPEED, "t1", HMI_FIELD_FORMAT_SPEED,
        HMI_REFRESH_DYNAMIC_US, VEHICLE_STATE_VALID_SPEED_KMH
    },
    [HMI_FIELD_SOC] = {
        HMI_FIELD_SOC, "t2", HMI_FIELD_FORMAT_PERCENT,
        HMI_REFRESH_VERY_SLOW_US, VEHICLE_STATE_VALID_HV_SOC_PERCENT
    },
    [HMI_FIELD_HV_VOLTAGE] = {
        HMI_FIELD_HV_VOLTAGE, "t3", HMI_FIELD_FORMAT_VOLTAGE,
        HMI_REFRESH_SLOW_US, VEHICLE_STATE_VALID_HV_VOLTAGE_V
    },
    [HMI_FIELD_HV_CURRENT] = {
        HMI_FIELD_HV_CURRENT, "t4", HMI_FIELD_FORMAT_CURRENT,
        HMI_REFRESH_SLOW_US, VEHICLE_STATE_VALID_HV_CURRENT_A
    },
    [HMI_FIELD_HV_POWER] = {
        HMI_FIELD_HV_POWER, "t5", HMI_FIELD_FORMAT_POWER,
        HMI_REFRESH_SLOW_US, VEHICLE_STATE_VALID_HV_POWER_W
    },
    [HMI_FIELD_LAP_NUMBER] = {
        HMI_FIELD_LAP_NUMBER, "t6", HMI_FIELD_FORMAT_UNSIGNED,
        HMI_REFRESH_LAP_US, 0U
    },
    [HMI_FIELD_LAP_TIME] = {
        HMI_FIELD_LAP_TIME, "t7", HMI_FIELD_FORMAT_TIME,
        HMI_REFRESH_LAP_US, 0U
    },
    [HMI_FIELD_DELTA_TIME] = {
        HMI_FIELD_DELTA_TIME, "t8", HMI_FIELD_FORMAT_TIME,
        HMI_REFRESH_MEDIUM_US, 0U
    },
    [HMI_FIELD_ACCEL_DISTANCE] = {
        HMI_FIELD_ACCEL_DISTANCE, "t9", HMI_FIELD_FORMAT_DISTANCE,
        HMI_REFRESH_MEDIUM_US, 0U
    },
    [HMI_FIELD_ACCEL_50M_TIME] = {
        HMI_FIELD_ACCEL_50M_TIME, "t10", HMI_FIELD_FORMAT_TIME,
        HMI_REFRESH_MEDIUM_US, 0U
    },
    [HMI_FIELD_ACCEL_TIME] = {
        HMI_FIELD_ACCEL_TIME, "t11", HMI_FIELD_FORMAT_TIME,
        HMI_REFRESH_MEDIUM_US, 0U
    },
    [HMI_FIELD_MAX_ACCELERATION] = {
        HMI_FIELD_MAX_ACCELERATION, "t12", HMI_FIELD_FORMAT_ACCELERATION,
        HMI_REFRESH_MEDIUM_US, 0U
    },
    [HMI_FIELD_TORQUE] = {
        HMI_FIELD_TORQUE, "t13", HMI_FIELD_FORMAT_TORQUE,
        HMI_REFRESH_MEDIUM_US, VEHICLE_STATE_VALID_TORQUE_NM
    },
    [HMI_FIELD_LV_VOLTAGE] = {
        HMI_FIELD_LV_VOLTAGE, "t14", HMI_FIELD_FORMAT_VOLTAGE,
        HMI_REFRESH_VERY_SLOW_US, VEHICLE_STATE_VALID_LV_VOLTAGE_V
    },
    [HMI_FIELD_LAST_LAP_TIME] = {
        HMI_FIELD_LAST_LAP_TIME, "t15", HMI_FIELD_FORMAT_TIME,
        HMI_REFRESH_SLOW_US, 0U
    },
    [HMI_FIELD_BEST_LAP_TIME] = {
        HMI_FIELD_BEST_LAP_TIME, "t16", HMI_FIELD_FORMAT_TIME,
        HMI_REFRESH_SLOW_US, 0U
    },
    [HMI_FIELD_SECTOR_NUMBER] = {
        HMI_FIELD_SECTOR_NUMBER, "t17", HMI_FIELD_FORMAT_UNSIGNED,
        HMI_REFRESH_SLOW_US, 0U
    },
    [HMI_FIELD_EVENT_MODE] = {
        HMI_FIELD_EVENT_MODE, "t18", HMI_FIELD_FORMAT_TEXT,
        HMI_REFRESH_SLOW_US, 0U
    }
};

static ssize_t hmi_serial_write(void *context, const void *buffer,
                                size_t length)
{
    const Hmi *hmi = context;

    if (hmi == NULL || hmi->fd < 0) {
        errno = EBADF;
        return -1;
    }
    return write(hmi->fd, buffer, length);
}

static int hmi_configure_termios(int fd)
{
    struct termios settings;

    if (tcgetattr(fd, &settings) < 0) {
        return -1;
    }
    cfmakeraw(&settings);
    if (cfsetispeed(&settings, B115200) < 0 ||
        cfsetospeed(&settings, B115200) < 0) {
        return -1;
    }
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
#ifdef CRTSCTS
    settings.c_cflag &= ~CRTSCTS;
#endif
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &settings) < 0) {
        return -1;
    }
    return tcflush(fd, TCIOFLUSH);
}

int hmi_init(Hmi *hmi, const char *device_path)
{
    const char *path = device_path != NULL ? device_path
                                            : HMI_DEFAULT_DEVICE_PATH;
    HmiTransport transport;
    int fd;

    if (hmi == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    memset(hmi, 0, sizeof(*hmi));
    hmi->fd = -1;
    fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        LOG_WARN("HMI: cannot open UART1 %s: %s", path, strerror(errno));
        return -1;
    }
    if (hmi_configure_termios(fd) < 0) {
        LOG_WARN("HMI: cannot configure %s as 115200 8N1: %s", path,
                 strerror(errno));
        close(fd);
        return -1;
    }
    hmi->fd = fd;
    transport.write = hmi_serial_write;
    /* UART1 uses external automatic RS485 direction control.  Keeping the
     * descriptor non-blocking and avoiding tcdrain ensures a failed display
     * cannot stall the VehicleState snapshot and shutdown paths. */
    transport.drain = NULL;
    transport.context = hmi;
    hmi->transport = transport;
    hmi->initialized = true;
    LOG_INFO("HMI: UART1 %s initialized, RS485 %u 8N1, external auto "
             "TX/RX direction control",
             path, HMI_BAUD_RATE);
    return 0;
}

int hmi_init_transport(Hmi *hmi, const HmiTransport *transport)
{
    if (hmi == NULL || transport == NULL || transport->write == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(hmi, 0, sizeof(*hmi));
    hmi->fd = -1;
    hmi->transport = *transport;
    hmi->initialized = true;
    LOG_DEBUG("HMI: custom transport initialized for protocol test");
    return 0;
}

void hmi_close(Hmi *hmi)
{
    if (hmi == NULL) {
        return;
    }
    if (hmi->fd >= 0) {
        close(hmi->fd);
    }
    hmi->fd = -1;
    hmi->initialized = false;
}

const HmiFieldDefinition *hmi_get_field_map(size_t *count)
{
    if (count != NULL) {
        *count = HMI_FIELD_COUNT;
    }
    return hmi_field_map;
}

static bool hmi_field_is_valid(const HmiFieldDefinition *field,
                               const VehicleState *state)
{
    if (field->valid_mask != 0U) {
        return (state->valid_mask & field->valid_mask) == field->valid_mask;
    }
    switch (field->id) {
    case HMI_FIELD_LAP_NUMBER:
    case HMI_FIELD_LAP_TIME:
    case HMI_FIELD_DELTA_TIME:
    case HMI_FIELD_LAST_LAP_TIME:
    case HMI_FIELD_BEST_LAP_TIME:
        return state->lap_valid;
    default:
        return true;
    }
}

static bool hmi_format_double(char *value, size_t value_size, double number,
                              const char *format)
{
    int written;

    if (!isfinite(number)) {
        written = snprintf(value, value_size, "--");
    } else {
        written = snprintf(value, value_size, format, number);
    }
    return written >= 0 && (size_t)written < value_size;
}

static bool hmi_get_numeric_value(HmiFieldId id, const VehicleState *state,
                                  double *number)
{
    switch (id) {
    case HMI_FIELD_RPM:
        *number = state->motor_rpm;
        return true;
    case HMI_FIELD_SPEED:
        *number = state->speed_kmh;
        return true;
    case HMI_FIELD_SOC:
        *number = state->hv_soc_percent;
        return true;
    case HMI_FIELD_HV_VOLTAGE:
        *number = state->hv_voltage_v;
        return true;
    case HMI_FIELD_HV_CURRENT:
        *number = state->hv_current_a;
        return true;
    case HMI_FIELD_HV_POWER:
        *number = state->hv_power_w;
        return true;
    case HMI_FIELD_LAP_TIME:
        *number = state->lap_time_s;
        return true;
    case HMI_FIELD_DELTA_TIME:
        *number = state->delta_time_s;
        return true;
    case HMI_FIELD_ACCEL_DISTANCE:
        *number = state->accel_distance_m;
        return true;
    case HMI_FIELD_ACCEL_50M_TIME:
        *number = state->accel_50m_time_s;
        return true;
    case HMI_FIELD_ACCEL_TIME:
        *number = state->accel_time_s;
        return true;
    case HMI_FIELD_MAX_ACCELERATION:
        *number = state->max_acceleration;
        return true;
    case HMI_FIELD_TORQUE:
        *number = state->torque_nm;
        return true;
    case HMI_FIELD_LV_VOLTAGE:
        *number = state->lv_voltage_v;
        return true;
    case HMI_FIELD_LAST_LAP_TIME:
        *number = state->last_lap_time_s;
        return true;
    case HMI_FIELD_BEST_LAP_TIME:
        *number = state->best_lap_time_s;
        return true;
    default:
        return false;
    }
}

static const char *hmi_numeric_format(HmiFieldFormat format)
{
    switch (format) {
    case HMI_FIELD_FORMAT_RPM:
    case HMI_FIELD_FORMAT_POWER:
    case HMI_FIELD_FORMAT_TORQUE:
        return "%.0f";
    case HMI_FIELD_FORMAT_SPEED:
    case HMI_FIELD_FORMAT_PERCENT:
    case HMI_FIELD_FORMAT_VOLTAGE:
    case HMI_FIELD_FORMAT_CURRENT:
        return "%.1f";
    case HMI_FIELD_FORMAT_TIME:
        return "%.2f";
    case HMI_FIELD_FORMAT_DISTANCE:
        return "%.1f";
    case HMI_FIELD_FORMAT_ACCELERATION:
        return "%.2f";
    default:
        return NULL;
    }
}

static bool hmi_format_field(const HmiFieldDefinition *field,
                             const VehicleState *state, char *value,
                             size_t value_size)
{
    double number;
    const char *format;

    if (!hmi_field_is_valid(field, state)) {
        return snprintf(value, value_size, "--") >= 0 &&
               strlen(value) < value_size;
    }

    if (field->format == HMI_FIELD_FORMAT_UNSIGNED) {
        if (field->id != HMI_FIELD_LAP_NUMBER &&
            field->id != HMI_FIELD_SECTOR_NUMBER) {
            return false;
        }
        return snprintf(value, value_size, "%u",
                        field->id == HMI_FIELD_LAP_NUMBER
                            ? state->lap_number
                            : state->sector_number) >= 0 &&
               strlen(value) < value_size;
    }
    if (field->format == HMI_FIELD_FORMAT_TEXT) {
        if (field->id != HMI_FIELD_EVENT_MODE) {
            return false;
        }
        return snprintf(value, value_size, "%s",
                        vehicle_state_event_mode_name(state->event_mode)) >= 0 &&
               strlen(value) < value_size;
    }
    format = hmi_numeric_format(field->format);
    if (format == NULL || !hmi_get_numeric_value(field->id, state, &number)) {
        return false;
    }
    return hmi_format_double(value, value_size, number, format);
}

static bool hmi_period_elapsed(const Hmi *hmi, HmiFieldId id,
                               uint64_t timestamp_us, uint64_t period_us)
{
    if (!hmi->last_sent_valid[id] || timestamp_us <
                                            hmi->last_sent_timestamp_us[id]) {
        return true;
    }
    return timestamp_us - hmi->last_sent_timestamp_us[id] >= period_us;
}

static int hmi_write_batch(Hmi *hmi, const unsigned char *buffer, size_t length)
{
    size_t offset = 0U;

    while (offset < length) {
        ssize_t written = hmi->transport.write(
            hmi->transport.context, buffer + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written == 0) {
            errno = EIO;
        }
        LOG_ERROR("HMI: RS485 write failed after %zu/%zu bytes: %s", offset,
                  length, strerror(errno));
        return -1;
    }
    if (hmi->transport.drain != NULL &&
        hmi->transport.drain(hmi->transport.context) < 0) {
        LOG_ERROR("HMI: UART transmit completion failed: %s",
                  strerror(errno));
        return -1;
    }
    return 0;
}

int hmi_update(Hmi *hmi, uint64_t timestamp_us, const VehicleState *state)
{
    unsigned char tx_buffer[HMI_TX_BUFFER_SIZE];
    HmiFieldId pending[HMI_FIELD_COUNT];
    char values[HMI_FIELD_COUNT][HMI_MAX_VALUE_LENGTH];
    size_t pending_count = 0U;
    size_t tx_length = 0U;
    size_t field_index;

    if (hmi == NULL || state == NULL || !hmi->initialized ||
        hmi->transport.write == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (timestamp_us == 0U) {
        timestamp_us = state->timestamp_us;
    }

    for (field_index = 0U; field_index < HMI_FIELD_COUNT; ++field_index) {
        const HmiFieldDefinition *field = &hmi_field_map[field_index];
        HmiFieldId id = field->id;
        int written;

        if (!hmi_format_field(field, state, values[id], sizeof(values[id]))) {
            LOG_WARN("HMI: cannot format field %s", field->target);
            continue;
        }
        if (hmi->last_sent_valid[id] &&
            strcmp(hmi->last_sent_value[id], values[id]) == 0) {
            continue;
        }
        if (!hmi_period_elapsed(hmi, id, timestamp_us,
                                field->minimum_period_us)) {
            continue;
        }
        written = snprintf((char *)tx_buffer + tx_length,
                           sizeof(tx_buffer) - tx_length, "%s.txt=\"%s\"",
                           field->target, values[id]);
        if (written < 0 || (size_t)written >= sizeof(tx_buffer) - tx_length ||
            sizeof(tx_buffer) - tx_length - (size_t)written <
                HMI_COMMAND_TERMINATOR_LENGTH) {
            errno = EMSGSIZE;
            LOG_ERROR("HMI: command buffer is too small for %s", field->target);
            return -1;
        }
        tx_length += (size_t)written;
        tx_buffer[tx_length++] = 0xFFU;
        tx_buffer[tx_length++] = 0xFFU;
        tx_buffer[tx_length++] = 0xFFU;
        pending[pending_count++] = id;
    }

    if (pending_count == 0U) {
        return 0;
    }
    if (hmi_write_batch(hmi, tx_buffer, tx_length) < 0) {
        return -1;
    }
    for (field_index = 0U; field_index < pending_count; ++field_index) {
        HmiFieldId id = pending[field_index];

        LOG_DEBUG("HMI TX: %s.txt=\"%s\" + FF FF FF",
                  hmi_field_map[id].target, values[id]);
        memcpy(hmi->last_sent_value[id], values[id], sizeof(values[id]));
        hmi->last_sent_timestamp_us[id] = timestamp_us;
        hmi->last_sent_valid[id] = true;
    }
    return (int)pending_count;
}
