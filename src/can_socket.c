#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "t113_terminal/can_socket.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can/raw.h>
#include <net/if.h>

int can_socket_open(const char *ifname, bool receive_mode,
                    const struct can_socket_filter *filter)
{
    struct ifreq interface_request;
    struct sockaddr_can address;
    int socket_fd;

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        perror("socket(PF_CAN)");
        return -1;
    }

    memset(&interface_request, 0, sizeof(interface_request));
    snprintf(interface_request.ifr_name, sizeof(interface_request.ifr_name),
             "%s", ifname);
    if (ioctl(socket_fd, SIOCGIFINDEX, &interface_request) < 0) {
        fprintf(stderr, "Cannot find CAN interface %s: %s\n", ifname,
                strerror(errno));
        close(socket_fd);
        return -1;
    }

    if (receive_mode && filter != NULL && filter->enabled) {
        struct can_filter can_filter;

        can_filter.can_id = filter->can_id;
        can_filter.can_mask = CAN_EFF_MASK | CAN_EFF_FLAG;
        if (setsockopt(socket_fd, SOL_CAN_RAW, CAN_RAW_FILTER, &can_filter,
                       sizeof(can_filter)) < 0) {
            perror("setsockopt(CAN_RAW_FILTER)");
            close(socket_fd);
            return -1;
        }
    }

    memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = interface_request.ifr_ifindex;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "Cannot bind CAN interface %s: %s\n", ifname,
                strerror(errno));
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

ssize_t can_socket_receive(int socket_fd, struct can_frame *frame)
{
    return read(socket_fd, frame, sizeof(*frame));
}

int can_socket_send(int socket_fd, const struct can_frame *frame)
{
    ssize_t written = write(socket_fd, frame, CAN_MTU);

    if (written < 0) {
        return -1;
    }
    if (written != CAN_MTU) {
        errno = EIO;
        return -1;
    }

    return 0;
}
