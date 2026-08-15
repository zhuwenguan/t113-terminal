#include <stdio.h>
#include <stdlib.h>

#include "can_receiver.h"

int main(void)
{
    static const char *const interfaces[CAN_RECEIVER_BUS_COUNT] = {
        "awlink0",
        "awlink1"
    };
    CanReceiver receiver;
    int result;

    if (can_receiver_init(&receiver, interfaces) < 0) {
        return EXIT_FAILURE;
    }

    result = can_receiver_run(&receiver);
    can_receiver_print_stats(&receiver);
    can_receiver_close(&receiver);

    if (result < 0) {
        fprintf(stderr, "CAN receiver stopped with an error\n");
        return EXIT_FAILURE;
    }

    printf("CAN receiver stopped cleanly\n");
    return EXIT_SUCCESS;
}
