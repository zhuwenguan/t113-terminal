#include <stdio.h>
#include <stdlib.h>

#include "can_receiver.h"
#include "console_log.h"
#include "lappro.h"
#include "logger.h"

int main(void)
{
    static const char *const interfaces[CAN_RECEIVER_BUS_COUNT] = {
        "awlink0",
        "awlink1"
    };
    CanReceiver receiver;
    Logger logger;
    LapProConfig lappro_config;
    LapPro lappro;
    bool lappro_ready = false;
    bool logger_ready = false;
    int result;

    console_log_init();

    if (logger_init(&logger) < 0) {
        LOG_WARN("CAN logger unavailable; continuing without file logging");
    } else if (logger_start(&logger) < 0) {
        logger_stop(&logger);
        LOG_WARN("CAN logger did not start; continuing without file logging");
    } else {
        logger_ready = true;
    }

    lappro_config_default(&lappro_config);
    if (lappro_init(&lappro, &lappro_config) == 0) {
        lappro_ready = true;
        if (lappro_start(&lappro) < 0) {
            LOG_WARN("LapPro worker did not start");
        }
    } else {
        LOG_WARN("LapPro initialization failed; continuing CAN");
    }

    if (can_receiver_init(&receiver, interfaces) < 0) {
        if (lappro_ready) {
            lappro_stop(&lappro);
        }
        if (logger_ready) {
            logger_stop(&logger);
        }
        return EXIT_FAILURE;
    }

    if (logger_ready) {
        can_receiver_set_message_sink(&receiver, logger_message_sink, &logger);
    }
    result = can_receiver_run(&receiver);
    can_receiver_print_stats(&receiver);
    can_receiver_close(&receiver);
    if (lappro_ready) {
        lappro_stop(&lappro);
    }
    if (logger_ready) {
        logger_stop(&logger);
        logger_print_stats(&logger);
    }

    if (result < 0) {
        LOG_ERROR("CAN receiver stopped with an error");
        return EXIT_FAILURE;
    }

    LOG_INFO("CAN receiver stopped cleanly");
    return EXIT_SUCCESS;
}
