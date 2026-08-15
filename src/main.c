#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_INTERFACE "awlink0"
#define DEFAULT_CAN_ID 0x123U
#define DEFAULT_PERIOD_MS 1000U

enum app_mode {
    MODE_RECEIVE,
    MODE_SEND
};

struct app_config {
    char ifname[IFNAMSIZ];
    enum app_mode mode;
    canid_t can_id;
    bool id_set;
    bool extended;
    uint8_t data[CAN_MAX_DLEN];
    uint8_t data_len;
    unsigned int period_ms;
    unsigned long count;
};

static volatile sig_atomic_t running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return -1;
    }

    return 0;
}

static void print_usage(const char *program)
{
    printf("Usage:\n");
    printf("  %s recv [options]\n", program);
    printf("  %s send [options]\n", program);
    printf("\n");
    printf("If the mode is omitted, recv is used.\n");
    printf("\nOptions:\n");
    printf("  -i, --interface NAME  CAN interface (default: %s)\n",
           DEFAULT_INTERFACE);
    printf("  -I, --id ID           CAN ID; recv uses it as an exact filter\n");
    printf("  -d, --data HEX        Data bytes, e.g. 01020304 or 01:02:03:04\n");
    printf("  -p, --period-ms N     Send period in milliseconds (default: %u)\n",
           DEFAULT_PERIOD_MS);
    printf("  -n, --count N         Number of frames; 0 means forever\n");
    printf("  -e, --extended        Use a 29-bit extended CAN ID\n");
    printf("  -h, --help            Show this help\n");
    printf("\nExamples:\n");
    printf("  %s recv -i awlink0 -I 0x123\n", program);
    printf("  %s send -i awlink0 -I 0x123 -d 01020304 -p 1000\n", program);
}

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

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    return -1;
}

static int parse_data(const char *text, uint8_t *data, uint8_t *data_len)
{
    int high_nibble = -1;
    size_t length = 0;
    size_t index;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    for (index = 0; text[index] != '\0'; ++index) {
        int nibble;

        if (isspace((unsigned char)text[index]) || text[index] == ':' ||
            text[index] == '-' || text[index] == '_') {
            continue;
        }

        nibble = hex_value(text[index]);
        if (nibble < 0) {
            return -1;
        }

        if (high_nibble < 0) {
            high_nibble = nibble;
        } else {
            if (length >= CAN_MAX_DLEN) {
                return -1;
            }
            data[length++] = (uint8_t)((high_nibble << 4) | nibble);
            high_nibble = -1;
        }
    }

    if (high_nibble >= 0 || length == 0) {
        return -1;
    }

    *data_len = (uint8_t)length;
    return 0;
}

static void init_config(struct app_config *config)
{
    memset(config, 0, sizeof(*config));
    snprintf(config->ifname, sizeof(config->ifname), "%s", DEFAULT_INTERFACE);
    config->mode = MODE_RECEIVE;
    config->can_id = DEFAULT_CAN_ID;
    config->period_ms = DEFAULT_PERIOD_MS;
    config->data[0] = 0x01;
    config->data[1] = 0x02;
    config->data[2] = 0x03;
    config->data[3] = 0x04;
    config->data_len = 4;
}

static int parse_options(int argc, char *argv[], int option_start,
                         struct app_config *config)
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
    int option;

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
            if (parse_data(optarg, config->data, &config->data_len) < 0) {
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
            print_usage(argv[0]);
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

static int open_can_socket(const struct app_config *config)
{
    struct ifreq interface_request;
    struct sockaddr_can address;
    int socket_fd;

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        perror("socket(PF_CAN)");
        return -1;
    }

    memset(&interface_request, 0, sizeof(interface_request));
    snprintf(interface_request.ifr_name, sizeof(interface_request.ifr_name),
             "%s", config->ifname);
    if (ioctl(socket_fd, SIOCGIFINDEX, &interface_request) < 0) {
        fprintf(stderr, "Cannot find CAN interface %s: %s\n", config->ifname,
                strerror(errno));
        close(socket_fd);
        return -1;
    }

    if (config->mode == MODE_RECEIVE && config->id_set) {
        struct can_filter filter;

        filter.can_id = config->can_id;
        filter.can_mask = CAN_EFF_MASK | CAN_EFF_FLAG;
        if (setsockopt(socket_fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter,
                       sizeof(filter)) < 0) {
            perror("setsockopt(CAN_RAW_FILTER)");
            close(socket_fd);
            return -1;
        }
    }

    memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = interface_request.ifr_ifindex;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "Cannot bind CAN interface %s: %s\n", config->ifname,
                strerror(errno));
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void print_frame(const char *direction, const char *ifname,
                        const struct can_frame *frame)
{
    struct timespec timestamp;
    canid_t id = frame->can_id & CAN_EFF_MASK;
    unsigned int index;

    if (clock_gettime(CLOCK_REALTIME, &timestamp) < 0) {
        timestamp.tv_sec = 0;
        timestamp.tv_nsec = 0;
    }

    printf("[%lld.%03ld] %s if=%s id=",
           (long long)timestamp.tv_sec, timestamp.tv_nsec / 1000000L,
           direction, ifname);
    if (frame->can_id & CAN_EFF_FLAG) {
        printf("0x%08x", (unsigned int)id);
    } else {
        printf("0x%03x", (unsigned int)id);
    }
    printf(" dlc=%u", (unsigned int)frame->can_dlc);
    if (frame->can_id & CAN_RTR_FLAG) {
        printf(" RTR");
    } else {
        printf(" data=");
        for (index = 0; index < frame->can_dlc; ++index) {
            printf("%s%02x", index == 0 ? "" : " ", frame->data[index]);
        }
    }
    printf("\n");
    fflush(stdout);
}

static int run_receiver(int socket_fd, const struct app_config *config)
{
    struct can_frame frame;

    printf("Receiving on %s", config->ifname);
    if (config->id_set) {
        printf(" (CAN ID filter: 0x%x)",
               (unsigned int)(config->can_id & CAN_EFF_MASK));
    }
    printf(". Press Ctrl-C to stop.\n");

    while (running) {
        ssize_t received = read(socket_fd, &frame, sizeof(frame));

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read(CAN)");
            return -1;
        }
        if ((size_t)received != CAN_MTU) {
            fprintf(stderr, "Invalid CAN frame size: %zd\n", received);
            return -1;
        }
        if (frame.can_dlc > CAN_MAX_DLEN) {
            fprintf(stderr, "Invalid CAN DLC: %u\n",
                    (unsigned int)frame.can_dlc);
            return -1;
        }

        print_frame("RX", config->ifname, &frame);
    }

    return 0;
}

static int sleep_period(unsigned int period_ms)
{
    struct timespec remaining;
    struct timespec requested;

    if (period_ms == 0) {
        return 0;
    }

    requested.tv_sec = period_ms / 1000U;
    requested.tv_nsec = (long)(period_ms % 1000U) * 1000000L;
    while (nanosleep(&requested, &remaining) < 0) {
        if (errno != EINTR) {
            perror("nanosleep");
            return -1;
        }
        if (!running) {
            return 0;
        }
        requested = remaining;
    }

    return 0;
}

static int run_sender(int socket_fd, const struct app_config *config)
{
    struct can_frame frame;
    unsigned long sent_count = 0;

    memset(&frame, 0, sizeof(frame));
    frame.can_id = config->can_id;
    frame.can_dlc = config->data_len;
    memcpy(frame.data, config->data, config->data_len);

    printf("Sending on %s every %u ms. Press Ctrl-C to stop.\n",
           config->ifname, config->period_ms);

    while (running && (config->count == 0 || sent_count < config->count)) {
        ssize_t written = write(socket_fd, &frame, CAN_MTU);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write(CAN)");
            return -1;
        }
        if (written != CAN_MTU) {
            fprintf(stderr, "Short CAN write: %zd\n", written);
            return -1;
        }

        print_frame("TX", config->ifname, &frame);
        ++sent_count;

        if (sent_count < config->count || config->count == 0) {
            if (sleep_period(config->period_ms) < 0) {
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct app_config config;
    int option_start = 1;
    int option_result;
    int socket_fd;
    int result;

    init_config(&config);

    if (argc > 1 && strcmp(argv[1], "send") == 0) {
        config.mode = MODE_SEND;
        option_start = 2;
    } else if (argc > 1 && strcmp(argv[1], "recv") == 0) {
        config.mode = MODE_RECEIVE;
        option_start = 2;
    }

    option_result = parse_options(argc, argv, option_start, &config);
    if (option_result > 0) {
        return EXIT_SUCCESS;
    }
    if (option_result < 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() < 0) {
        return EXIT_FAILURE;
    }

    socket_fd = open_can_socket(&config);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    if (config.mode == MODE_SEND) {
        result = run_sender(socket_fd, &config);
    } else {
        result = run_receiver(socket_fd, &config);
    }

    close(socket_fd);
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
