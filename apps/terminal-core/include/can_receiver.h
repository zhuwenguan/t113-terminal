#ifndef T113_CAN_RECEIVER_H
#define T113_CAN_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/can.h>
#include <net/if.h>

#define CAN_RECEIVER_BUS_COUNT 2U
#define CAN_RECEIVER_RECONNECT_INTERVAL_US 1000000ULL

enum {
    CAN_MESSAGE_FLAG_STANDARD = 1U << 0,
    CAN_MESSAGE_FLAG_EXTENDED = 1U << 1,
    CAN_MESSAGE_FLAG_REMOTE = 1U << 2,
    CAN_MESSAGE_FLAG_ERROR = 1U << 3
};

typedef struct {
    uint64_t timestamp_us;
    uint8_t bus;
    uint32_t id;
    uint32_t flags;
    uint8_t dlc;
    uint8_t data[8];
} CanMessage;

typedef struct {
    uint64_t rx_frames;
    uint64_t error_frames;
    uint64_t malformed_frames;
    uint64_t read_errors;
    uint64_t epoll_errors;
    uint64_t bus_off_events;
    bool interface_failed;
} CanReceiverStats;

typedef void (*CanMessageSink)(const CanMessage *message, void *context);
typedef void (*CanReceiverTick)(void *context);

typedef struct {
    int epoll_fd;
    int socket_fd[CAN_RECEIVER_BUS_COUNT];
    /* A bit is set only after that bus socket has been configured and added
     * to epoll.  Bit 0 is CAN0 and bit 1 is CAN1. */
    uint32_t active_mask;
    uint64_t reconnect_next_attempt_us[CAN_RECEIVER_BUS_COUNT];
    uint32_t reconnect_attempts[CAN_RECEIVER_BUS_COUNT];
    char interface_name[CAN_RECEIVER_BUS_COUNT][IF_NAMESIZE];
    CanReceiverStats stats[CAN_RECEIVER_BUS_COUNT];
    uint64_t epoll_wait_errors;
    uint64_t rate_last_report_us;
    uint64_t rate_last_rx_frames[CAN_RECEIVER_BUS_COUNT];
    CanMessageSink message_sink;
    void *message_sink_context;
    CanReceiverTick tick;
    void *tick_context;
} CanReceiver;

int can_receiver_init(CanReceiver *receiver,
                      const char *const interfaces[CAN_RECEIVER_BUS_COUNT]);

int can_receiver_run(CanReceiver *receiver);

void can_receiver_stop(CanReceiver *receiver);

void can_receiver_set_message_sink(CanReceiver *receiver,
                                    CanMessageSink sink, void *context);

/* Called once per epoll cycle, including timeout cycles.  This gives a
 * daemon a bounded-rate place to advance its lifecycle without moving CAN
 * reception to a second thread. */
void can_receiver_set_tick_callback(CanReceiver *receiver,
                                    CanReceiverTick tick, void *context);

/* Returns the buses whose configured socket is currently registered in
 * receiver->epoll_fd and available for reception. */
uint32_t can_receiver_active_mask(const CanReceiver *receiver);

void can_receiver_print_stats(const CanReceiver *receiver);

void can_receiver_close(CanReceiver *receiver);

#endif
