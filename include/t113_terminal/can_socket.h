#ifndef T113_TERMINAL_CAN_SOCKET_H
#define T113_TERMINAL_CAN_SOCKET_H

#include <stdbool.h>
#include <sys/types.h>

#include <linux/can.h>

struct can_socket_filter {
    bool enabled;
    canid_t can_id;
};

int can_socket_open(const char *ifname, bool receive_mode,
                    const struct can_socket_filter *filter);

ssize_t can_socket_receive(int socket_fd, struct can_frame *frame);

int can_socket_send(int socket_fd, const struct can_frame *frame);

#endif
