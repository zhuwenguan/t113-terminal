#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "t113_terminal/can_frame.h"

#include <ctype.h>
#include <stdio.h>
#include <time.h>

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    return -1;
}

int can_frame_parse_hex_data(const char *text, uint8_t *data,
                             uint8_t *data_len)
{
    int high_nibble = -1;
    size_t length = 0;
    size_t index;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    for (index = 0; text[index] != '\0'; ++index) {
        int nibble;

        if (isspace((unsigned char)text[index]) || text[index] == ':' ||
            text[index] == '-' || text[index] == '_') {
            continue;
        }

        nibble = hex_value(text[index]);
        if (nibble < 0) {
            return -1;
        }

        if (high_nibble < 0) {
            high_nibble = nibble;
        } else {
            if (length >= CAN_MAX_DLEN) {
                return -1;
            }
            data[length++] = (uint8_t)((high_nibble << 4) | nibble);
            high_nibble = -1;
        }
    }

    if (high_nibble >= 0 || length == 0) {
        return -1;
    }

    *data_len = (uint8_t)length;
    return 0;
}

void can_frame_print(const char *direction, const char *ifname,
                     const struct can_frame *frame)
{
    struct timespec timestamp;
    canid_t id = frame->can_id & CAN_EFF_MASK;
    unsigned int index;

    if (clock_gettime(CLOCK_REALTIME, &timestamp) < 0) {
        timestamp.tv_sec = 0;
        timestamp.tv_nsec = 0;
    }

    printf("[%lld.%03ld] %s if=%s id=",
           (long long)timestamp.tv_sec, timestamp.tv_nsec / 1000000L,
           direction, ifname);
    if (frame->can_id & CAN_EFF_FLAG) {
        printf("0x%08x", (unsigned int)id);
    } else {
        printf("0x%03x", (unsigned int)id);
    }
    printf(" dlc=%u", (unsigned int)frame->can_dlc);
    if (frame->can_id & CAN_RTR_FLAG) {
        printf(" RTR");
    } else {
        printf(" data=");
        for (index = 0; index < frame->can_dlc; ++index) {
            printf("%s%02x", index == 0 ? "" : " ", frame->data[index]);
        }
    }
    printf("\n");
    fflush(stdout);
}
