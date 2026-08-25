#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef int (*LifecycleRealOpen)(const char *, int, ...);
typedef void *(*LifecycleRealDlopen)(const char *, int);

static pthread_once_t lifecycle_symbols_once = PTHREAD_ONCE_INIT;
static LifecycleRealOpen lifecycle_real_open;
static LifecycleRealOpen lifecycle_real_open64;
static LifecycleRealDlopen lifecycle_real_dlopen;

static void lifecycle_load_symbols(void)
{
    *(void **)(&lifecycle_real_open) = dlsym(RTLD_NEXT, "open");
    *(void **)(&lifecycle_real_open64) = dlsym(RTLD_NEXT, "open64");
    *(void **)(&lifecycle_real_dlopen) = dlsym(RTLD_NEXT, "dlopen");
}

static bool lifecycle_path_matches(const char *path, const char *variable)
{
    const char *target = getenv(variable);

    return path != NULL && target != NULL && target[0] != '\0' &&
           strcmp(path, target) == 0;
}

static int lifecycle_open(LifecycleRealOpen real_open, const char *path,
                          int flags, va_list arguments)
{
    mode_t mode = 0;

    if ((flags & O_CREAT) != 0) {
        mode = (mode_t)va_arg(arguments, int);
    }
    if (lifecycle_path_matches(path, "T113_FAULT_OPEN_PATH")) {
        errno = EIO;
        return -1;
    }
    if (real_open == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return (flags & O_CREAT) != 0 ? real_open(path, flags, mode) :
                                   real_open(path, flags);
}

int open(const char *path, int flags, ...)
{
    va_list arguments;
    int result;

    (void)pthread_once(&lifecycle_symbols_once, lifecycle_load_symbols);
    va_start(arguments, flags);
    result = lifecycle_open(lifecycle_real_open, path, flags, arguments);
    va_end(arguments);
    return result;
}

int open64(const char *path, int flags, ...)
{
    va_list arguments;
    int result;

    (void)pthread_once(&lifecycle_symbols_once, lifecycle_load_symbols);
    va_start(arguments, flags);
    result = lifecycle_open(lifecycle_real_open64 != NULL ?
                                lifecycle_real_open64 : lifecycle_real_open,
                            path, flags, arguments);
    va_end(arguments);
    return result;
}

void *dlopen(const char *path, int flags)
{
    (void)pthread_once(&lifecycle_symbols_once, lifecycle_load_symbols);
    if (lifecycle_path_matches(path, "T113_FAULT_DLOPEN_PATH")) {
        errno = ENOENT;
        return NULL;
    }
    if (lifecycle_real_dlopen == NULL) {
        errno = ENOSYS;
        return NULL;
    }
    return lifecycle_real_dlopen(path, flags);
}
