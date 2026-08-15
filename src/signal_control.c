#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "t113_terminal/signal_control.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

int signal_control_install(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        fprintf(stderr, "Cannot install signal handlers: %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}

bool signal_control_is_running(void)
{
    return running != 0;
}
