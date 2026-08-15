#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "t113_terminal/app_config.h"
#include "t113_terminal/can_service.h"
#include "t113_terminal/can_socket.h"
#include "t113_terminal/signal_control.h"

int main(int argc, char *argv[])
{
    struct app_config config;
    struct can_socket_filter filter;
    struct can_frame frame;
    int parse_result;
    int socket_fd;
    int result;

    app_config_init(&config);
    parse_result = app_config_parse(argc, argv, &config);
    if (parse_result > 0) {
        return EXIT_SUCCESS;
    }
    if (parse_result < 0) {
        app_config_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (signal_control_install() < 0) {
        return EXIT_FAILURE;
    }

    filter.enabled = config.mode == MODE_RECEIVE && config.id_set;
    filter.can_id = config.can_id;
    socket_fd = can_socket_open(config.ifname, config.mode == MODE_RECEIVE,
                                &filter);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    if (config.mode == MODE_SEND) {
        frame.can_id = config.can_id;
        frame.can_dlc = config.data_len;
        for (unsigned int index = 0; index < config.data_len; ++index) {
            frame.data[index] = config.data[index];
        }
        for (unsigned int index = config.data_len; index < CAN_MAX_DLEN;
             ++index) {
            frame.data[index] = 0;
        }

        result = can_service_run_sender(socket_fd, config.ifname, &frame,
                                        config.period_ms, config.count);
    } else {
        result = can_service_run_receiver(socket_fd, config.ifname);
    }

    close(socket_fd);
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
