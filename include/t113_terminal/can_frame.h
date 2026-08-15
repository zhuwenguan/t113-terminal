#ifndef T113_TERMINAL_CAN_FRAME_H
#define T113_TERMINAL_CAN_FRAME_H

#include <stdint.h>

#include <linux/can.h>

int can_frame_parse_hex_data(const char *text, uint8_t *data,
                             uint8_t *data_len);

void can_frame_print(const char *direction, const char *ifname,
                     const struct can_frame *frame);

#endif
