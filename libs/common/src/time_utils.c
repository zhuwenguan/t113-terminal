#define _POSIX_C_SOURCE 200809L
#include <race/time_utils.h>
#include <errno.h>
#include <time.h>
int race_monotonic_us(uint64_t *timestamp_us) {
    struct timespec now;
    if (!timestamp_us) { errno = EINVAL; return -1; }
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    *timestamp_us = (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
    return 0;
}
