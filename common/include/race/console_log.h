#ifndef T113_CONSOLE_LOG_H
#define T113_CONSOLE_LOG_H

#include <stdbool.h>

typedef enum {
    CONSOLE_LOG_DEBUG = 0,
    CONSOLE_LOG_INFO,
    CONSOLE_LOG_WARN,
    CONSOLE_LOG_ERROR,
    CONSOLE_LOG_FATAL
} ConsoleLogLevel;

/* Initializes the logger. The default level is INFO. */
void console_log_init(void);

void console_log_set_level(ConsoleLogLevel level);

bool console_log_enabled(ConsoleLogLevel level);

void console_log_write(ConsoleLogLevel level, const char *format, ...);

#define LOG_DEBUG(...)                                                        \
    do {                                                                      \
        if (console_log_enabled(CONSOLE_LOG_DEBUG)) {                        \
            console_log_write(CONSOLE_LOG_DEBUG, __VA_ARGS__);                \
        }                                                                     \
    } while (0)

#define LOG_INFO(...)                                                         \
    do {                                                                      \
        if (console_log_enabled(CONSOLE_LOG_INFO)) {                         \
            console_log_write(CONSOLE_LOG_INFO, __VA_ARGS__);                 \
        }                                                                     \
    } while (0)

#define LOG_WARN(...)                                                         \
    do {                                                                      \
        if (console_log_enabled(CONSOLE_LOG_WARN)) {                         \
            console_log_write(CONSOLE_LOG_WARN, __VA_ARGS__);                 \
        }                                                                     \
    } while (0)

#define LOG_ERROR(...)                                                        \
    do {                                                                      \
        if (console_log_enabled(CONSOLE_LOG_ERROR)) {                         \
            console_log_write(CONSOLE_LOG_ERROR, __VA_ARGS__);                \
        }                                                                     \
    } while (0)

#define LOG_FATAL(...)                                                        \
    do {                                                                      \
        if (console_log_enabled(CONSOLE_LOG_FATAL)) {                         \
            console_log_write(CONSOLE_LOG_FATAL, __VA_ARGS__);                \
        }                                                                     \
    } while (0)

#endif
