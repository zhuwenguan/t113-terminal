#ifndef T113_TERMINAL_SIGNAL_CONTROL_H
#define T113_TERMINAL_SIGNAL_CONTROL_H

#include <stdbool.h>

int signal_control_install(void);

bool signal_control_is_running(void);

#endif
