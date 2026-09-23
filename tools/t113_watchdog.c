#define _GNU_SOURCE
#include <race/core_status.h>
#include <race/time_utils.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;
static void handle_signal(int signal_number) { (void)signal_number; stop_requested = 1; }

int main(int argc, char **argv)
{
    const char *status_name = RACE_CORE_STATUS_NAME;
    const char *pid_path = "/run/race/race_core.pid";
    int timeout = 15;
    int option;
    int watchdog_fd;
    RaceCoreStatusHandle status = {.fd = -1};
    struct sigaction action = {0};

    while ((option = getopt(argc, argv, "t:s:p:")) != -1) {
        if (option == 't') timeout = atoi(optarg);
        else if (option == 's') status_name = optarg;
        else if (option == 'p') pid_path = optarg;
        else return 2;
    }
    if (timeout < 5) timeout = 5;
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0) return 2;
    watchdog_fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    if (watchdog_fd < 0) return 1;
    (void)ioctl(watchdog_fd, WDIOC_SETTIMEOUT, &timeout);
    if (race_core_status_reader_open(&status, status_name) < 0) {
        close(watchdog_fd);
        return 1;
    }
    {
        unsigned int stale_ticks = 0U;
        while (!stop_requested) {
        RaceCoreStatusData data;
        uint64_t now_us = 0U;
        bool stale = race_monotonic_us(&now_us) < 0 ||
            race_core_status_read(&status, &data) < 0 ||
            race_core_status_is_stale(now_us, data.heartbeat_mono_us,
                                      (uint64_t)timeout * 1000000ULL);
        FILE *pid_file = fopen(pid_path, "r");
        unsigned long core_pid = 0UL;
        bool core_alive = false;
        if (pid_file != NULL) {
            if (fscanf(pid_file, "%lu", &core_pid) != 1) {
                core_pid = 0UL;
            }
            fclose(pid_file);
            core_alive = core_pid > 1UL && kill((pid_t)core_pid, 0) == 0;
            if (core_alive) {
                char proc_path[64];
                char command_byte;
                int proc_fd;

                (void)snprintf(proc_path, sizeof(proc_path),
                               "/proc/%lu/cmdline", core_pid);
                proc_fd = open(proc_path, O_RDONLY | O_CLOEXEC);
                if (proc_fd < 0 || read(proc_fd, &command_byte, 1) <= 0) {
                    core_alive = false;
                }
                if (proc_fd >= 0) close(proc_fd);
            }
        }
        if (!stale || !core_alive) {
            stale_ticks = 0U;
            (void)ioctl(watchdog_fd, WDIOC_KEEPALIVE, 0);
        }
        else {
            /* Give the supervisor a short crash-recovery window. A stuck
             * core stops receiving keepalives and the hardware reboots. */
            if (++stale_ticks >= (unsigned int)(timeout * 2)) break;
        }
        sleep(1);
        }
    }
    if (!stop_requested) {
        /* Keep the fd open until the kernel watchdog resets the board. */
        for (;;) pause();
    }
    if (write(watchdog_fd, "V", 1) < 0) {
        /* The close magic is best effort; kernels with nowayout ignore it. */
    }
    close(watchdog_fd);
    race_core_status_close(&status);
    return 0;
}
