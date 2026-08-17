#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "console_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include <pthread.h>

static pthread_once_t console_log_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t console_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static ConsoleLogLevel console_log_level = CONSOLE_LOG_INFO;
static struct timespec console_log_start;

static ConsoleLogLevel console_log_parse_level(const char *value)
{
    char level[16];
    size_t index;

    if (value == NULL) {
        return CONSOLE_LOG_INFO;
    }

    for (index = 0; index + 1U < sizeof(level) && value[index] != '\0';
         ++index) {
        level[index] = (char)toupper((unsigned char)value[index]);
    }
    level[index] = '\0';

    if (strcmp(level, "DEBUG") == 0) {
        return CONSOLE_LOG_DEBUG;
    }
    if (strcmp(level, "WARN") == 0 || strcmp(level, "WARNING") == 0) {
        return CONSOLE_LOG_WARN;
    }
    if (strcmp(level, "ERROR") == 0) {
        return CONSOLE_LOG_ERROR;
    }
    if (strcmp(level, "FATAL") == 0) {
        return CONSOLE_LOG_FATAL;
    }
    return CONSOLE_LOG_INFO;
}

static void console_log_initialize_once(void)
{
    const char *level;

    if (clock_gettime(CLOCK_MONOTONIC, &console_log_start) < 0) {
        console_log_start.tv_sec = 0;
        console_log_start.tv_nsec = 0;
    }

    level = getenv("T113_LOG_LEVEL");
    if (level == NULL) {
        level = getenv("LOG_LEVEL");
    }
    console_log_level = console_log_parse_level(level);
}

void console_log_init(void)
{
    (void)pthread_once(&console_log_once, console_log_initialize_once);
}

void console_log_set_level(ConsoleLogLevel level)
{
    if (level < CONSOLE_LOG_DEBUG || level > CONSOLE_LOG_FATAL) {
        return;
    }

    console_log_init();
    pthread_mutex_lock(&console_log_mutex);
    console_log_level = level;
    pthread_mutex_unlock(&console_log_mutex);
}

bool console_log_enabled(ConsoleLogLevel level)
{
    bool enabled;

    if (level < CONSOLE_LOG_DEBUG || level > CONSOLE_LOG_FATAL) {
        return false;
    }

    console_log_init();
    pthread_mutex_lock(&console_log_mutex);
    enabled = level >= console_log_level;
    pthread_mutex_unlock(&console_log_mutex);
    return enabled;
}

static const char *console_log_level_name(ConsoleLogLevel level)
{
    static const char *const names[] = {
        "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
    };

    return names[level];
}

static void console_log_vwrite(ConsoleLogLevel level, const char *format,
                               va_list arguments)
{
    struct timespec now;
    time_t elapsed_sec;
    long elapsed_nsec;
    uint64_t elapsed_us;
    FILE *stream;

    if (level < CONSOLE_LOG_DEBUG || level > CONSOLE_LOG_FATAL ||
        format == NULL) {
        return;
    }

    console_log_init();
    pthread_mutex_lock(&console_log_mutex);
    if (level < console_log_level) {
        pthread_mutex_unlock(&console_log_mutex);
        return;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
        (now.tv_sec > console_log_start.tv_sec ||
         (now.tv_sec == console_log_start.tv_sec &&
          now.tv_nsec >= console_log_start.tv_nsec))) {
        elapsed_sec = now.tv_sec - console_log_start.tv_sec;
        elapsed_nsec = now.tv_nsec - console_log_start.tv_nsec;
        if (elapsed_nsec < 0) {
            elapsed_sec--;
            elapsed_nsec += 1000000000L;
        }
        elapsed_us = (uint64_t)elapsed_sec * 1000000ULL +
                     (uint64_t)elapsed_nsec / 1000ULL;
    } else {
        elapsed_us = 0;
    }

    stream = level >= CONSOLE_LOG_ERROR ? stderr : stdout;
    fprintf(stream, "[%llu.%03llu] %s ",
            (unsigned long long)(elapsed_us / 1000000ULL),
            (unsigned long long)((elapsed_us / 1000ULL) % 1000ULL),
            console_log_level_name(level));
    vfprintf(stream, format, arguments);
    fputc('\n', stream);
    if (level >= CONSOLE_LOG_INFO) {
        fflush(stream);
    }
    pthread_mutex_unlock(&console_log_mutex);
}

void console_log_write(ConsoleLogLevel level, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    console_log_vwrite(level, format, arguments);
    va_end(arguments);
}
