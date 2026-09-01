#define _POSIX_C_SOURCE 200809L

#include "webd_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <race/shared_state.h>
#include <race/video_ipc.h>

static char *trim_space(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) {
        --end;
        *end = '\0';
    }
    return text;
}

static int copy_string(char *destination, size_t destination_size,
                       const char *value)
{
    if (destination == NULL || destination_size == 0U || value == NULL ||
        value[0] == '\0' || strlen(value) >= destination_size) {
        errno = EINVAL;
        return -1;
    }
    (void)snprintf(destination, destination_size, "%s", value);
    return 0;
}

static int parse_port(const char *value, uint16_t *port)
{
    char *end;
    unsigned long parsed;

    if (value == NULL || port == NULL || value[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0U ||
        parsed > 65535U) {
        errno = EINVAL;
        return -1;
    }
    *port = (uint16_t)parsed;
    return 0;
}

static int parse_rate(const char *value, unsigned int *rate)
{
    char *end;
    unsigned long parsed;

    if (value == NULL || rate == NULL || value[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0U ||
        parsed > WEBD_MAX_RATE_HZ) {
        errno = EINVAL;
        return -1;
    }
    *rate = (unsigned int)parsed;
    return 0;
}

static int apply_pair(WebdConfig *config, const char *key, const char *value)
{
    if (config == NULL || key == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(key, "listen_address") == 0) {
        return copy_string(config->listen_address,
                           sizeof(config->listen_address), value);
    }
    if (strcmp(key, "listen_port") == 0) {
        return parse_port(value, &config->listen_port);
    }
    if (strcmp(key, "web_root") == 0) {
        return copy_string(config->web_root, sizeof(config->web_root), value);
    }
    if (strcmp(key, "telemetry_rate_hz") == 0) {
        return parse_rate(value, &config->telemetry_rate_hz);
    }
    if (strcmp(key, "state_shm_name") == 0) {
        if (value[0] != '/') {
            errno = EINVAL;
            return -1;
        }
        return copy_string(config->state_shm_name,
                           sizeof(config->state_shm_name), value);
    }
    if (strcmp(key, "video_shm_name") == 0) {
        if (value[0] != '/') {
            errno = EINVAL;
            return -1;
        }
        return copy_string(config->video_shm_name,
                           sizeof(config->video_shm_name), value);
    }
    errno = ENOENT;
    return -1;
}

void webd_config_defaults(WebdConfig *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    (void)snprintf(config->listen_address, sizeof(config->listen_address),
                   "%s", WEBD_DEFAULT_LISTEN_ADDRESS);
    config->listen_port = WEBD_DEFAULT_LISTEN_PORT;
    (void)snprintf(config->web_root, sizeof(config->web_root),
                   "%s", "web/dashboard/dist");
    config->telemetry_rate_hz = WEBD_DEFAULT_TELEMETRY_RATE_HZ;
    (void)snprintf(config->state_shm_name, sizeof(config->state_shm_name),
                   "%s", RACE_SHARED_STATE_NAME);
    (void)snprintf(config->video_shm_name, sizeof(config->video_shm_name),
                   "%s", RACE_VIDEO_IPC_NAME);
}

int webd_config_load_file(WebdConfig *config, const char *path)
{
    FILE *file;
    char line[PATH_MAX + 128U];
    unsigned int line_number = 0U;

    if (config == NULL || path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *text;
        char *separator;

        ++line_number;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            fprintf(stderr, "webd: config line %u is too long\n", line_number);
            fclose(file);
            errno = EOVERFLOW;
            return -1;
        }
        text = trim_space(line);
        if (text[0] == '\0' || text[0] == '#') {
            continue;
        }
        separator = strchr(text, '=');
        if (separator == NULL) {
            fprintf(stderr, "webd: config line %u must be key=value\n",
                    line_number);
            fclose(file);
            errno = EINVAL;
            return -1;
        }
        *separator = '\0';
        text = trim_space(text);
        separator = trim_space(separator + 1);
        {
            char *comment = strchr(separator, '#');
            if (comment != NULL) {
                *comment = '\0';
                separator = trim_space(separator);
            }
        }
        if (apply_pair(config, text, separator) < 0) {
            fprintf(stderr, "webd: invalid config line %u (%s)\n",
                    line_number, text);
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        int saved_errno = errno != 0 ? errno : EIO;
        fclose(file);
        errno = saved_errno;
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    return 0;
}

static int argument_value(int argc, char **argv, int *index,
                          const char **value)
{
    if (index == NULL || value == NULL || *index + 1 >= argc) {
        errno = EINVAL;
        return -1;
    }
    *value = argv[++(*index)];
    if (*value == NULL || (*value)[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int webd_config_parse_arguments(WebdConfig *config, int argc, char **argv,
                                const char **config_path)
{
    int index;

    if (config == NULL || argv == NULL || argc < 1) {
        errno = EINVAL;
        return -1;
    }
    if (config_path != NULL) {
        *config_path = NULL;
    }
    for (index = 1; index < argc; ++index) {
        const char *value;

        if (strcmp(argv[index], "--help") == 0 ||
            strcmp(argv[index], "-h") == 0) {
            return 1;
        }
        if (strcmp(argv[index], "--verbose") == 0 ||
            strcmp(argv[index], "-v") == 0) {
            continue;
        }
        if (strcmp(argv[index], "--config") == 0) {
            if (argument_value(argc, argv, &index, &value) < 0) {
                return -1;
            }
            if (config_path != NULL) {
                *config_path = value;
            }
            continue;
        }
        if (strcmp(argv[index], "--listen-address") == 0) {
            if (argument_value(argc, argv, &index, &value) < 0 ||
                copy_string(config->listen_address,
                            sizeof(config->listen_address), value) < 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--port") == 0 ||
            strcmp(argv[index], "--listen-port") == 0) {
            if (argument_value(argc, argv, &index, &value) < 0 ||
                parse_port(value, &config->listen_port) < 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--web-root") == 0) {
            if (argument_value(argc, argv, &index, &value) < 0 ||
                copy_string(config->web_root, sizeof(config->web_root), value) < 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--telemetry-rate-hz") == 0) {
            if (argument_value(argc, argv, &index, &value) < 0 ||
                parse_rate(value, &config->telemetry_rate_hz) < 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--state-shm-name") == 0) {
            if (argument_value(argc, argv, &index, &value) < 0 ||
                value[0] != '/' ||
                copy_string(config->state_shm_name,
                            sizeof(config->state_shm_name), value) < 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--video-shm-name") == 0) {
            if (argument_value(argc, argv, &index, &value) < 0 ||
                value[0] != '/' ||
                copy_string(config->video_shm_name,
                            sizeof(config->video_shm_name), value) < 0) {
                return -1;
            }
            continue;
        }
        fprintf(stderr, "webd: unknown argument: %s\n", argv[index]);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void webd_config_print_usage(const char *program_name)
{
    fprintf(stdout,
            "Usage: %s [options]\n"
            "  -c, --config PATH             read key=value configuration\n"
            "  -a, --listen-address ADDRESS  default: %s\n"
            "  -p, --port PORT               default: %u\n"
            "      --web-root PATH           default: web/dashboard/dist\n"
            "      --telemetry-rate-hz HZ    default: %u (max %u)\n"
            "      --state-shm-name NAME     default: %s\n"
            "      --video-shm-name NAME     default: %s\n"
            "  -v, --verbose                 enable debug logging\n",
            program_name != NULL ? program_name : "webd",
            WEBD_DEFAULT_LISTEN_ADDRESS, WEBD_DEFAULT_LISTEN_PORT,
            WEBD_DEFAULT_TELEMETRY_RATE_HZ, WEBD_MAX_RATE_HZ,
            RACE_SHARED_STATE_NAME, RACE_VIDEO_IPC_NAME);
}
