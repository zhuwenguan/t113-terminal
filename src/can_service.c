#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "t113_terminal/can_service.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "t113_terminal/can_frame.h"
#include "t113_terminal/can_socket.h"
#include "t113_terminal/signal_control.h"

static int sleep_period(unsigned int period_ms)
{
    struct timespec remaining;
    struct timespec requested;

    if (period_ms == 0) {
        return 0;
    }

    requested.tv_sec = period_ms / 1000U;
    requested.tv_nsec = (long)(period_ms % 1000U) * 1000000L;
    while (nanosleep(&requested, &remaining) < 0) {
        if (errno != EINTR) {
            perror("nanosleep");
            return -1;
        }
        if (!signal_control_is_running()) {
            return 0;
        }
        requested = remaining;
    }

    return 0;
}

int can_service_run_receiver(int socket_fd, const char *ifname)
{
    struct can_frame frame;

    printf("Receiving on %s. Press Ctrl-C to stop.\n", ifname);

    while (signal_control_is_running()) {
        ssize_t received = can_socket_receive(socket_fd, &frame);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read(CAN)");
            return -1;
        }
        if ((size_t)received != CAN_MTU) {
            fprintf(stderr, "Invalid CAN frame size: %zd\n", received);
            return -1;
        }
        if (frame.can_dlc > CAN_MAX_DLEN) {
            fprintf(stderr, "Invalid CAN DLC: %u\n",
                    (unsigned int)frame.can_dlc);
            return -1;
        }

        can_frame_print("RX", ifname, &frame);
    }

    return 0;
}

int can_service_run_sender(int socket_fd, const char *ifname,
                           const struct can_frame *frame,
                           unsigned int period_ms, unsigned long count)
{
    unsigned long sent_count = 0;

    printf("Sending on %s every %u ms. Press Ctrl-C to stop.\n", ifname,
           period_ms);

    while (signal_control_is_running() &&
           (count == 0 || sent_count < count)) {
        if (can_socket_send(socket_fd, frame) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write(CAN)");
            return -1;
        }

        can_frame_print("TX", ifname, frame);
        ++sent_count;

        if (sent_count < count || count == 0) {
            if (sleep_period(period_ms) < 0) {
                return -1;
            }
        }
    }

    return 0;
}
