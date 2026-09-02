#ifndef T113_CAN_RECEIVER_H
#define T113_CAN_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/can.h>
#include <net/if.h>

#define CAN_RECEIVER_BUS_COUNT 2U

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

void can_receiver_print_stats(const CanReceiver *receiver);

void can_receiver_close(CanReceiver *receiver);

#endif
