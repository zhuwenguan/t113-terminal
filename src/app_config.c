#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "t113_terminal/app_config.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t113_terminal/can_frame.h"

static int parse_unsigned(const char *text, unsigned long maximum,
                          unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > maximum) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int parse_can_id(const char *text, canid_t *can_id)
{
    unsigned long value;

    if (parse_unsigned(text, CAN_EFF_MASK, &value) < 0) {
        return -1;
    }

    *can_id = (canid_t)value;
    return 0;
}

void app_config_init(struct app_config *config)
{
    memset(config, 0, sizeof(*config));
    snprintf(config->ifname, sizeof(config->ifname), "%s",
             APP_DEFAULT_INTERFACE);
    config->mode = MODE_RECEIVE;
    config->can_id = APP_DEFAULT_CAN_ID;
    config->period_ms = APP_DEFAULT_PERIOD_MS;
    config->data[0] = 0x01;
    config->data[1] = 0x02;
    config->data[2] = 0x03;
    config->data[3] = 0x04;
    config->data_len = 4;
}

void app_config_print_usage(const char *program)
{
    printf("Usage:\n");
    printf("  %s recv [options]\n", program);
    printf("  %s send [options]\n", program);
    printf("\n");
    printf("If the mode is omitted, recv is used.\n");
    printf("\nOptions:\n");
    printf("  -i, --interface NAME  CAN interface (default: %s)\n",
           APP_DEFAULT_INTERFACE);
    printf("  -I, --id ID           CAN ID; recv uses it as an exact filter\n");
    printf("  -d, --data HEX        Data bytes, e.g. 01020304 or 01:02:03:04\n");
    printf("  -p, --period-ms N     Send period in milliseconds (default: %u)\n",
           APP_DEFAULT_PERIOD_MS);
    printf("  -n, --count N         Number of frames; 0 means forever\n");
    printf("  -e, --extended        Use a 29-bit extended CAN ID\n");
    printf("  -h, --help            Show this help\n");
    printf("\nExamples:\n");
    printf("  %s recv -i awlink0 -I 0x123\n", program);
    printf("  %s send -i awlink0 -I 0x123 -d 01020304 -p 1000\n", program);
}

int app_config_parse(int argc, char *argv[], struct app_config *config)
{
    static const struct option options[] = {
        {"interface", required_argument, NULL, 'i'},
        {"id", required_argument, NULL, 'I'},
        {"data", required_argument, NULL, 'd'},
        {"period-ms", required_argument, NULL, 'p'},
        {"count", required_argument, NULL, 'n'},
        {"extended", no_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int option_start = 1;
    int option;

    if (argc > 1 && strcmp(argv[1], "send") == 0) {
        config->mode = MODE_SEND;
        option_start = 2;
    } else if (argc > 1 && strcmp(argv[1], "recv") == 0) {
        config->mode = MODE_RECEIVE;
        option_start = 2;
    }

    opterr = 0;
    optind = option_start;
    while ((option = getopt_long(argc, argv, "i:I:d:p:n:eh", options, NULL)) !=
           -1) {
        unsigned long value;

        switch (option) {
        case 'i':
            if (optarg[0] == '\0' || strlen(optarg) >= sizeof(config->ifname)) {
                fprintf(stderr, "Invalid CAN interface name: %s\n", optarg);
                return -1;
            }
            snprintf(config->ifname, sizeof(config->ifname), "%s", optarg);
            break;

        case 'I':
            if (parse_can_id(optarg, &config->can_id) < 0) {
                fprintf(stderr, "Invalid CAN ID: %s\n", optarg);
                return -1;
            }
            config->id_set = true;
            break;

        case 'd':
            if (can_frame_parse_hex_data(optarg, config->data,
                                          &config->data_len) < 0) {
                fprintf(stderr, "Invalid CAN data: %s\n", optarg);
                return -1;
            }
            break;

        case 'p':
            if (parse_unsigned(optarg, UINT_MAX, &value) < 0) {
                fprintf(stderr, "Invalid period: %s\n", optarg);
                return -1;
            }
            config->period_ms = (unsigned int)value;
            break;

        case 'n':
            if (parse_unsigned(optarg, ULONG_MAX, &value) < 0) {
                fprintf(stderr, "Invalid count: %s\n", optarg);
                return -1;
            }
            config->count = value;
            break;

        case 'e':
            config->extended = true;
            break;

        case 'h':
            app_config_print_usage(argv[0]);
            return 1;

        case '?':
        default:
            fprintf(stderr, "Unknown or incomplete option\n");
            return -1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return -1;
    }

    if (config->extended) {
        config->can_id |= CAN_EFF_FLAG;
    } else if (config->can_id > CAN_SFF_MASK) {
        fprintf(stderr, "Standard CAN ID must be in the range 0x000-0x7ff\n");
        return -1;
    }

    return 0;
}
