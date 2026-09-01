#ifndef T113_WEBD_CONFIG_H
#define T113_WEBD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limits.h>

#include <race/video_ipc.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define WEBD_DEFAULT_LISTEN_ADDRESS "0.0.0.0"
#define WEBD_DEFAULT_LISTEN_PORT 8080U
#define WEBD_DEFAULT_TELEMETRY_RATE_HZ 20U
#define WEBD_MAX_RATE_HZ 100U

typedef struct {
    char listen_address[64];
    uint16_t listen_port;
    char web_root[PATH_MAX];
    unsigned int telemetry_rate_hz;
    char state_shm_name[NAME_MAX];
    char video_shm_name[NAME_MAX];
} WebdConfig;

void webd_config_defaults(WebdConfig *config);

int webd_config_load_file(WebdConfig *config, const char *path);

int webd_config_parse_arguments(WebdConfig *config, int argc, char **argv,
                                const char **config_path);

void webd_config_print_usage(const char *program_name);

#endif
