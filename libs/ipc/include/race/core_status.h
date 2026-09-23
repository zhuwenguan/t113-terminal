#ifndef RACE_CORE_STATUS_H
#define RACE_CORE_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* This object is a small control-plane ABI.  VehicleState remains in its
 * existing shared memory object; this object describes whether that data is
 * usable by the other daemons. */
#define RACE_CORE_STATUS_NAME "/race_core_status"
#define RACE_CORE_STATUS_MAGIC UINT32_C(0x52435354)
#define RACE_CORE_STATUS_VERSION 2U
#define RACE_CORE_STATUS_PATH_MAX 256U
#define RACE_CORE_STATUS_STALE_US UINT64_C(3000000)

typedef enum {
    RACE_CORE_LIFECYCLE_STOPPED = 0,
    RACE_CORE_LIFECYCLE_PRECHECK = 1,
    RACE_CORE_LIFECYCLE_RUNNING = 2,
    RACE_CORE_LIFECYCLE_DEGRADED = 3,
    RACE_CORE_LIFECYCLE_STOPPING = 4
} RaceCoreLifecycleState;

enum {
    RACE_CORE_STATUS_LOGGER_READY = 1U << 0,
    RACE_CORE_STATUS_CAN0_READY = 1U << 1,
    RACE_CORE_STATUS_CAN1_READY = 1U << 2,
    RACE_CORE_STATUS_HMI_READY = 1U << 3,
    RACE_CORE_STATUS_LAPPRO_READY = 1U << 4
};

typedef struct {
    uint32_t state;
    uint32_t flags;
    uint32_t can_ready_mask;
    uint32_t reserved;
    uint64_t heartbeat_mono_us;
    uint64_t can_rx_frames[2];
    uint64_t can_error_frames[2];
    uint64_t can_bus_off_events[2];
    char session_directory[RACE_CORE_STATUS_PATH_MAX];
} RaceCoreStatusData;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    _Atomic uint32_t seq;
    RaceCoreStatusData data;
} RaceCoreStatus;

typedef struct {
    int fd;
    RaceCoreStatus *mapping;
    bool writer;
} RaceCoreStatusHandle;

int race_core_status_writer_open(RaceCoreStatusHandle *handle,
                                 const char *name);
int race_core_status_reader_open(RaceCoreStatusHandle *handle,
                                 const char *name);
int race_core_status_validate(const RaceCoreStatus *status);
int race_core_status_publish(RaceCoreStatusHandle *handle,
                             const RaceCoreStatusData *data);
int race_core_status_read(const RaceCoreStatusHandle *handle,
                          RaceCoreStatusData *data);
bool race_core_status_is_stale(uint64_t now_us, uint64_t heartbeat_us,
                               uint64_t threshold_us);
const char *race_core_lifecycle_name(RaceCoreLifecycleState state);
void race_core_status_close(RaceCoreStatusHandle *handle);

#endif
