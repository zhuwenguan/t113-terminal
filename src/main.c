#include <stdio.h>
#include <stdlib.h>

#include "can_receiver.h"
#include "logger.h"

int main(void)
{
    static const char *const interfaces[CAN_RECEIVER_BUS_COUNT] = {
        "awlink0",
        "awlink1"
    };
    CanReceiver receiver;
    Logger logger;
    int result;

    if (logger_init(&logger) < 0) {
        return EXIT_FAILURE;
    }
    if (logger_start(&logger) < 0) {
        logger_stop(&logger);
        return EXIT_FAILURE;
    }

    if (can_receiver_init(&receiver, interfaces) < 0) {
        logger_stop(&logger);
        return EXIT_FAILURE;
    }

    can_receiver_set_message_sink(&receiver, logger_message_sink, &logger);
    result = can_receiver_run(&receiver);
    can_receiver_print_stats(&receiver);
    can_receiver_close(&receiver);
    logger_stop(&logger);
    logger_print_stats(&logger);

    if (result < 0) {
        fprintf(stderr, "CAN receiver stopped with an error\n");
        return EXIT_FAILURE;
    }

    printf("CAN receiver stopped cleanly\n");
    return EXIT_SUCCESS;
}
