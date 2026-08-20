#ifndef T113_HMI_H
#define T113_HMI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "vehicle_state.h"

#define HMI_DEFAULT_DEVICE_PATH "/dev/ttyS1"
#define HMI_BAUD_RATE 115200U
#define HMI_COMMAND_TERMINATOR_LENGTH 3U
#define HMI_MAX_VALUE_LENGTH 32U

typedef ssize_t (*HmiWriteFunction)(void *context, const void *buffer,
                                    size_t length);
typedef int (*HmiDrainFunction)(void *context);

typedef struct {
    HmiWriteFunction write;
    HmiDrainFunction drain;
    void *context;
} HmiTransport;

typedef enum {
    HMI_FIELD_RPM = 0,
    HMI_FIELD_SPEED,
    HMI_FIELD_SOC,
    HMI_FIELD_HV_VOLTAGE,
    HMI_FIELD_HV_CURRENT,
    HMI_FIELD_HV_POWER,
    HMI_FIELD_LAP_NUMBER,
    HMI_FIELD_LAP_TIME,
    HMI_FIELD_DELTA_TIME,
    HMI_FIELD_ACCEL_DISTANCE,
    HMI_FIELD_ACCEL_50M_TIME,
    HMI_FIELD_ACCEL_TIME,
    HMI_FIELD_MAX_ACCELERATION,
    HMI_FIELD_TORQUE,
    HMI_FIELD_LV_VOLTAGE,
    HMI_FIELD_LAST_LAP_TIME,
    HMI_FIELD_BEST_LAP_TIME,
    HMI_FIELD_SECTOR_NUMBER,
    HMI_FIELD_EVENT_MODE,
    HMI_FIELD_COUNT
} HmiFieldId;

typedef enum {
    HMI_FIELD_FORMAT_RPM = 0,
    HMI_FIELD_FORMAT_SPEED,
    HMI_FIELD_FORMAT_PERCENT,
    HMI_FIELD_FORMAT_VOLTAGE,
    HMI_FIELD_FORMAT_CURRENT,
    HMI_FIELD_FORMAT_POWER,
    HMI_FIELD_FORMAT_TORQUE,
    HMI_FIELD_FORMAT_UNSIGNED,
    HMI_FIELD_FORMAT_TIME,
    HMI_FIELD_FORMAT_DISTANCE,
    HMI_FIELD_FORMAT_ACCELERATION,
    HMI_FIELD_FORMAT_TEXT
} HmiFieldFormat;

/* The target component and refresh policy are kept in one mapping table. */
typedef struct {
    HmiFieldId id;
    const char *target;
    HmiFieldFormat format;
    uint64_t minimum_period_us;
    uint32_t valid_mask;
} HmiFieldDefinition;

typedef struct {
    int fd;
    HmiTransport transport;
    bool initialized;
    bool last_sent_valid[HMI_FIELD_COUNT];
    uint64_t last_sent_timestamp_us[HMI_FIELD_COUNT];
    char last_sent_value[HMI_FIELD_COUNT][HMI_MAX_VALUE_LENGTH];
} Hmi;

/* Opens UART1 and configures 115200 8N1; external hardware controls TX/RX. */
int hmi_init(Hmi *hmi, const char *device_path);

/* Supplies a deterministic transport for host-side protocol tests. */
int hmi_init_transport(Hmi *hmi, const HmiTransport *transport);

/* Sends only changed fields whose individual refresh period has elapsed. */
int hmi_update(Hmi *hmi, uint64_t timestamp_us,
               const VehicleState *state);

void hmi_close(Hmi *hmi);

const HmiFieldDefinition *hmi_get_field_map(size_t *count);

#endif
