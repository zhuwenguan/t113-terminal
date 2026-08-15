#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_receiver.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/can/error.h>
#include <linux/can/raw.h>

static volatile sig_atomic_t receiver_running;

static void can_receiver_signal_handler(int signal_number)
{
    (void)signal_number;
    receiver_running = 0;
}

static int can_receiver_install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = can_receiver_signal_handler;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return -1;
    }

    return 0;
}

static void can_receiver_reset(CanReceiver *receiver)
{
    unsigned int bus;

    memset(receiver, 0, sizeof(*receiver));
    receiver->epoll_fd = -1;
    for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
        receiver->socket_fd[bus] = -1;
    }
}

static int can_receiver_open_socket(const char *ifname)
{
    struct ifreq interface_request;
    struct sockaddr_can address;
    int ifindex;
    int socket_fd;

    socket_fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
    if (socket_fd < 0) {
        fprintf(stderr, "socket(%s): %s\n", ifname, strerror(errno));
        return -1;
    }

    memset(&interface_request, 0, sizeof(interface_request));
    snprintf(interface_request.ifr_name, sizeof(interface_request.ifr_name),
             "%s", ifname);
    if (ioctl(socket_fd, SIOCGIFINDEX, &interface_request) < 0) {
        fprintf(stderr, "Cannot find CAN interface %s: %s\n", ifname,
                strerror(errno));
        close(socket_fd);
        return -1;
    }
    ifindex = interface_request.ifr_ifindex;

    if (ioctl(socket_fd, SIOCGIFFLAGS, &interface_request) < 0) {
        fprintf(stderr, "Cannot read flags for CAN interface %s: %s\n", ifname,
                strerror(errno));
        close(socket_fd);
        return -1;
    }
    if ((interface_request.ifr_flags & IFF_UP) == 0) {
        fprintf(stderr, "CAN interface %s is down\n", ifname);
        close(socket_fd);
        errno = ENETDOWN;
        return -1;
    }

    {
        can_err_mask_t error_mask = CAN_ERR_MASK;

        if (setsockopt(socket_fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                       &error_mask, sizeof(error_mask)) < 0) {
            fprintf(stderr, "Cannot enable CAN error frames on %s: %s\n",
                    ifname, strerror(errno));
            close(socket_fd);
            return -1;
        }
    }

    memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = ifindex;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "Cannot bind CAN interface %s: %s\n", ifname,
                strerror(errno));
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static uint64_t can_receiver_timestamp_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) < 0) {
        return 0;
    }

    return (uint64_t)timestamp.tv_sec * 1000000ULL +
           (uint64_t)timestamp.tv_nsec / 1000ULL;
}

static void can_receiver_print_message(const CanMessage *message,
                                        bool extended_id)
{
    unsigned int index;

    printf("[%llu us] CAN%u  ID=",
           (unsigned long long)message->timestamp_us, message->bus);
    if (extended_id) {
        printf("0x%08x", message->can_id);
    } else {
        printf("0x%03x", message->can_id);
    }
    printf(" DLC=%u DATA=", (unsigned int)message->dlc);
    for (index = 0; index < message->dlc; ++index) {
        printf("%s%02x", index == 0 ? "" : " ", message->data[index]);
    }
    printf("\n");
    fflush(stdout);
}

static void can_receiver_print_error_frame(unsigned int bus,
                                           const struct can_frame *frame)
{
    uint32_t error_mask = frame->can_id & CAN_ERR_MASK;

    fprintf(stderr, "CAN%u error frame: class=0x%08x", bus, error_mask);
    if ((error_mask & CAN_ERR_BUSOFF) != 0) {
        fprintf(stderr, " BUS-OFF");
    }
    if ((error_mask & CAN_ERR_ACK) != 0) {
        fprintf(stderr, " ACK");
    }
    if ((error_mask & CAN_ERR_BUSERROR) != 0) {
        fprintf(stderr, " BUS-ERROR");
    }
    fprintf(stderr, "\n");
}

static int can_receiver_handle_socket(CanReceiver *receiver, unsigned int bus)
{
    for (;;) {
        struct can_frame frame;
        CanMessage message;
        ssize_t received;

        received = recv(receiver->socket_fd[bus], &frame, sizeof(frame),
                        MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            receiver->stats[bus].read_errors++;
            receiver->stats[bus].interface_failed = true;
            fprintf(stderr, "CAN%u receive failed: %s\n", bus,
                    strerror(errno));
            return -1;
        }

        if (received == CANFD_MTU) {
            receiver->stats[bus].malformed_frames++;
            fprintf(stderr, "CAN%u received unsupported CAN FD frame\n", bus);
            continue;
        }
        if (received != CAN_MTU) {
            receiver->stats[bus].malformed_frames++;
            fprintf(stderr, "CAN%u received invalid frame size: %zd\n", bus,
                    received);
            continue;
        }
        if (frame.can_dlc > CAN_MAX_DLEN) {
            receiver->stats[bus].malformed_frames++;
            fprintf(stderr, "CAN%u received invalid DLC: %u\n", bus,
                    (unsigned int)frame.can_dlc);
            continue;
        }

        if ((frame.can_id & CAN_ERR_FLAG) != 0) {
            receiver->stats[bus].error_frames++;
            can_receiver_print_error_frame(bus, &frame);
            if ((frame.can_id & CAN_ERR_BUSOFF) != 0) {
                receiver->stats[bus].bus_off_events++;
                receiver->stats[bus].interface_failed = true;
                receiver_running = 0;
                return -1;
            }
            continue;
        }

        memset(&message, 0, sizeof(message));
        message.timestamp_us = can_receiver_timestamp_us();
        message.bus = (uint8_t)bus;
        message.can_id = frame.can_id & CAN_EFF_MASK;
        message.dlc = frame.can_dlc;
        memcpy(message.data, frame.data, message.dlc);
        receiver->stats[bus].rx_frames++;
        can_receiver_print_message(&message,
                                   (frame.can_id & CAN_EFF_FLAG) != 0);
        if (receiver->message_sink != NULL) {
            receiver->message_sink(&message,
                                   receiver->message_sink_context);
        }
    }
}

int can_receiver_init(CanReceiver *receiver,
                      const char *const interfaces[CAN_RECEIVER_BUS_COUNT])
{
    unsigned int bus;

    if (receiver == NULL || interfaces == NULL) {
        errno = EINVAL;
        return -1;
    }

    can_receiver_reset(receiver);
    receiver->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (receiver->epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    receiver_running = 1;
    if (can_receiver_install_signal_handlers() < 0) {
        can_receiver_close(receiver);
        return -1;
    }

    for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
        struct epoll_event event;

        if (interfaces[bus] == NULL ||
            strlen(interfaces[bus]) >= IF_NAMESIZE) {
            fprintf(stderr, "Invalid CAN interface name for CAN%u\n", bus);
            can_receiver_close(receiver);
            return -1;
        }

        snprintf(receiver->interface_name[bus], IF_NAMESIZE, "%s",
                 interfaces[bus]);
        receiver->socket_fd[bus] =
            can_receiver_open_socket(receiver->interface_name[bus]);
        if (receiver->socket_fd[bus] < 0) {
            can_receiver_close(receiver);
            return -1;
        }

        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        event.data.u32 = bus;
        if (epoll_ctl(receiver->epoll_fd, EPOLL_CTL_ADD,
                      receiver->socket_fd[bus], &event) < 0) {
            receiver->stats[bus].epoll_errors++;
            receiver->stats[bus].interface_failed = true;
            fprintf(stderr, "epoll_ctl(CAN%u): %s\n", bus,
                    strerror(errno));
            can_receiver_close(receiver);
            return -1;
        }

        printf("CAN%u: listening on %s\n", bus,
               receiver->interface_name[bus]);
    }
    fflush(stdout);

    return 0;
}

int can_receiver_run(CanReceiver *receiver)
{
    struct epoll_event events[CAN_RECEIVER_BUS_COUNT];

    if (receiver == NULL || receiver->epoll_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    while (receiver_running) {
        int event_count;
        int event_index;

        event_count = epoll_wait(receiver->epoll_fd, events,
                                 CAN_RECEIVER_BUS_COUNT, 1000);
        if (event_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            receiver->epoll_wait_errors++;
            perror("epoll_wait");
            return -1;
        }

        for (event_index = 0; event_index < event_count; ++event_index) {
            unsigned int bus = events[event_index].data.u32;
            uint32_t event_flags = events[event_index].events;

            if (bus >= CAN_RECEIVER_BUS_COUNT) {
                fprintf(stderr, "Invalid epoll bus index: %u\n", bus);
                return -1;
            }
            if ((event_flags & (EPOLLERR | EPOLLHUP)) != 0) {
                receiver->stats[bus].epoll_errors++;
                receiver->stats[bus].interface_failed = true;
                fprintf(stderr, "CAN%u interface event: 0x%x\n", bus,
                        event_flags);
                return -1;
            }
            if ((event_flags & EPOLLIN) != 0 &&
                can_receiver_handle_socket(receiver, bus) < 0) {
                return -1;
            }
        }
    }

    return 0;
}

void can_receiver_stop(CanReceiver *receiver)
{
    (void)receiver;
    receiver_running = 0;
}

void can_receiver_set_message_sink(CanReceiver *receiver,
                                   CanMessageSink sink, void *context)
{
    if (receiver == NULL) {
        return;
    }

    receiver->message_sink = sink;
    receiver->message_sink_context = context;
}

void can_receiver_print_stats(const CanReceiver *receiver)
{
    unsigned int bus;

    if (receiver == NULL) {
        return;
    }

    printf("CAN statistics:\n");
    for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
        const CanReceiverStats *stats = &receiver->stats[bus];

        printf("  CAN%u (%s): rx_frames=%llu error_frames=%llu "
               "malformed_frames=%llu read_errors=%llu epoll_errors=%llu "
               "bus_off_events=%llu interface_failed=%s\n",
               bus, receiver->interface_name[bus],
               (unsigned long long)stats->rx_frames,
               (unsigned long long)stats->error_frames,
               (unsigned long long)stats->malformed_frames,
               (unsigned long long)stats->read_errors,
               (unsigned long long)stats->epoll_errors,
               (unsigned long long)stats->bus_off_events,
               stats->interface_failed ? "yes" : "no");
    }
    printf("  epoll_wait_errors=%llu\n",
           (unsigned long long)receiver->epoll_wait_errors);
}

void can_receiver_close(CanReceiver *receiver)
{
    unsigned int bus;

    if (receiver == NULL) {
        return;
    }

    receiver_running = 0;
    for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
        if (receiver->socket_fd[bus] >= 0) {
            close(receiver->socket_fd[bus]);
            receiver->socket_fd[bus] = -1;
        }
    }
    if (receiver->epoll_fd >= 0) {
        close(receiver->epoll_fd);
        receiver->epoll_fd = -1;
    }
}
