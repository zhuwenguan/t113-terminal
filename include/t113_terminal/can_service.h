#ifndef T113_TERMINAL_CAN_SERVICE_H
#define T113_TERMINAL_CAN_SERVICE_H

#include <stdbool.h>

#include <linux/can.h>

int can_service_run_receiver(int socket_fd, const char *ifname);

int can_service_run_sender(int socket_fd, const char *ifname,
                           const struct can_frame *frame,
                           unsigned int period_ms, unsigned long count);

#endif
