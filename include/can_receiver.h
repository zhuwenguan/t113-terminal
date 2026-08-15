#ifndef T113_CAN_RECEIVER_H
#define T113_CAN_RECEIVER_H

#include <stdint.h>

#include <linux/can.h>
#include <net/if.h>

#define CAN_RECEIVER_BUS_COUNT 2U

typedef struct {
    uint64_t timestamp_us;
    uint8_t bus;
    uint32_t can_id;
    uint8_t dlc;
    uint8_t data[8];
} CanMessage;

typedef struct {
    int epoll_fd;
    int socket_fd[CAN_RECEIVER_BUS_COUNT];
    char interface_name[CAN_RECEIVER_BUS_COUNT][IF_NAMESIZE];
} CanReceiver;

int can_receiver_init(CanReceiver *receiver,
                      const char *const interfaces[CAN_RECEIVER_BUS_COUNT]);

int can_receiver_run(CanReceiver *receiver);

void can_receiver_stop(CanReceiver *receiver);

void can_receiver_close(CanReceiver *receiver);

#endif
