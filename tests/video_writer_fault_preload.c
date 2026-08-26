#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef size_t (*FaultRealFwrite)(const void *, size_t, size_t, FILE *);
typedef int (*FaultRealFflush)(FILE *);

static pthread_once_t fault_symbols_once = PTHREAD_ONCE_INIT;
static FaultRealFwrite fault_real_fwrite;
static FaultRealFflush fault_real_fflush;
static atomic_ulong fault_target_write_calls;
static atomic_ulong fault_injection_count;

static void fault_load_symbols(void)
{
    *(void **)(&fault_real_fwrite) = dlsym(RTLD_NEXT, "fwrite");
    *(void **)(&fault_real_fflush) = dlsym(RTLD_NEXT, "fflush");
}

static unsigned long fault_environment_ulong(const char *name)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return 0UL;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' ? value : 0UL;
}

static bool fault_stream_matches(FILE *stream)
{
    const char *target = getenv("T113_FAULT_WRITE_PATH");
    char descriptor_path[64];
    char resolved_path[PATH_MAX];
    ssize_t length;
    int descriptor;

    if (target == NULL || target[0] == '\0' || stream == NULL ||
        (descriptor = fileno(stream)) < 0 ||
        snprintf(descriptor_path, sizeof(descriptor_path),
                 "/proc/self/fd/%d", descriptor) <= 0) {
        return false;
    }
    length = readlink(descriptor_path, resolved_path,
                      sizeof(resolved_path) - 1U);
    if (length < 0 || (size_t)length >= sizeof(resolved_path)) {
        return false;
    }
    resolved_path[length] = '\0';
    return strcmp(resolved_path, target) == 0;
}

static void fault_delay_if_requested(void)
{
    unsigned long delay_ms = fault_environment_ulong("T113_FAULT_DELAY_MS");
    struct timespec delay;

    if (delay_ms == 0UL) {
        return;
    }
    delay.tv_sec = (time_t)(delay_ms / 1000UL);
    delay.tv_nsec = (long)(delay_ms % 1000UL) * 1000000L;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

size_t fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    const char *mode;
    unsigned long call;
    unsigned long after;
    unsigned long every;
    unsigned long maximum;

    (void)pthread_once(&fault_symbols_once, fault_load_symbols);
    if (fault_real_fwrite == NULL) {
        errno = EIO;
        return 0U;
    }
    if (!fault_stream_matches(stream)) {
        return fault_real_fwrite(data, size, count, stream);
    }
    call = atomic_fetch_add_explicit(&fault_target_write_calls, 1UL,
                                     memory_order_relaxed) + 1UL;
    after = fault_environment_ulong("T113_FAULT_AFTER_CALLS");
    if (call <= after) {
        return fault_real_fwrite(data, size, count, stream);
    }
    every = fault_environment_ulong("T113_FAULT_EVERY_CALLS");
    if (every > 0UL && (call - after - 1UL) % every != 0UL) {
        return fault_real_fwrite(data, size, count, stream);
    }
    maximum = fault_environment_ulong("T113_FAULT_MAX_INJECTIONS");
    if (maximum > 0UL &&
        atomic_fetch_add_explicit(&fault_injection_count, 1UL,
                                  memory_order_relaxed) >= maximum) {
        return fault_real_fwrite(data, size, count, stream);
    }

    fault_delay_if_requested();
    mode = getenv("T113_FAULT_WRITE_MODE");
    if (mode == NULL || strcmp(mode, "delay") == 0) {
        return fault_real_fwrite(data, size, count, stream);
    }
    if (strcmp(mode, "short") == 0) {
        size_t short_count = count > 1U ? count / 2U : 0U;

        return short_count > 0U
                   ? fault_real_fwrite(data, size, short_count, stream)
                   : 0U;
    }
    if (strcmp(mode, "enospc") == 0) {
        errno = ENOSPC;
        return 0U;
    }
    if (strcmp(mode, "erofs") == 0) {
        errno = EROFS;
        return 0U;
    }
    if (strcmp(mode, "eio") == 0) {
        errno = EIO;
        return 0U;
    }
    return fault_real_fwrite(data, size, count, stream);
}

int fflush(FILE *stream)
{
    const char *mode;

    (void)pthread_once(&fault_symbols_once, fault_load_symbols);
    if (fault_real_fflush == NULL) {
        errno = EIO;
        return EOF;
    }
    mode = getenv("T113_FAULT_WRITE_MODE");
    if (stream != NULL && fault_stream_matches(stream) && mode != NULL &&
        strcmp(mode, "flush_eio") == 0) {
        errno = EIO;
        return EOF;
    }
    return fault_real_fflush(stream);
}
