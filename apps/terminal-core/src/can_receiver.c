#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_receiver.h"
#include "console_log.h"

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
        LOG_ERROR("sigaction: %s", strerror(errno));
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
        LOG_ERROR("socket(%s): %s", ifname, strerror(errno));
        return -1;
    }

    memset(&interface_request, 0, sizeof(interface_request));
    snprintf(interface_request.ifr_name, sizeof(interface_request.ifr_name),
             "%s", ifname);
    if (ioctl(socket_fd, SIOCGIFINDEX, &interface_request) < 0) {
        LOG_ERROR("Cannot find CAN interface %s: %s", ifname,
                  strerror(errno));
        close(socket_fd);
        return -1;
    }
    ifindex = interface_request.ifr_ifindex;

    if (ioctl(socket_fd, SIOCGIFFLAGS, &interface_request) < 0) {
        LOG_ERROR("Cannot read flags for CAN interface %s: %s", ifname,
                  strerror(errno));
        close(socket_fd);
        return -1;
    }
    if ((interface_request.ifr_flags & IFF_UP) == 0) {
        LOG_ERROR("CAN interface %s is down", ifname);
        close(socket_fd);
        errno = ENETDOWN;
        return -1;
    }

    {
        can_err_mask_t error_mask = CAN_ERR_MASK;

        if (setsockopt(socket_fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                       &error_mask, sizeof(error_mask)) < 0) {
            LOG_ERROR("Cannot enable CAN error frames on %s: %s", ifname,
                      strerror(errno));
            close(socket_fd);
            return -1;
        }
    }

    memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = ifindex;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        LOG_ERROR("Cannot bind CAN interface %s: %s", ifname,
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

static void can_receiver_log_message(const CanMessage *message)
{
    char data[3U * CAN_MAX_DLEN];
    size_t offset = 0;
    unsigned int index;

    for (index = 0; index < message->dlc; ++index) {
        int written = snprintf(data + offset, sizeof(data) - offset,
                               index == 0U ? "%02x" : " %02x",
                               message->data[index]);

        if (written < 0 || (size_t)written >= sizeof(data) - offset) {
            data[0] = '\0';
            break;
        }
        offset += (size_t)written;
    }
    if (message->dlc == 0U) {
        data[0] = '\0';
    }

    LOG_DEBUG("CAN: rx ts_us=%llu bus=%u id=0x%08x flags=0x%08x dlc=%u "
              "data=%s",
              (unsigned long long)message->timestamp_us,
              (unsigned int)message->bus, message->id, message->flags,
              (unsigned int)message->dlc, data);
}

static void can_receiver_print_error_frame(unsigned int bus,
                                           const struct can_frame *frame)
{
    uint32_t error_mask = frame->can_id & CAN_ERR_MASK;

    LOG_WARN("CAN%u error frame: class=0x%08x%s%s%s", bus, error_mask,
             (error_mask & CAN_ERR_BUSOFF) != 0 ? " BUS-OFF" : "",
             (error_mask & CAN_ERR_ACK) != 0 ? " ACK" : "",
             (error_mask & CAN_ERR_BUSERROR) != 0 ? " BUS-ERROR" : "");
}

static void can_receiver_disable_bus(CanReceiver *receiver, unsigned int bus)
{
    int socket_fd;

    if (receiver == NULL || bus >= CAN_RECEIVER_BUS_COUNT) {
        return;
    }

    receiver->stats[bus].interface_failed = true;
    socket_fd = receiver->socket_fd[bus];
    if (socket_fd < 0) {
        return;
    }

    if (receiver->epoll_fd >= 0 &&
        epoll_ctl(receiver->epoll_fd, EPOLL_CTL_DEL, socket_fd, NULL) < 0 &&
        errno != ENOENT) {
        receiver->stats[bus].epoll_errors++;
        LOG_WARN("CAN%u: failed to remove isolated socket from epoll: %s",
                 bus, strerror(errno));
    }
    close(socket_fd);
    receiver->socket_fd[bus] = -1;
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
            LOG_ERROR("CAN%u (%s) receive failed: %s", bus,
                      receiver->interface_name[bus], strerror(errno));
            can_receiver_disable_bus(receiver, bus);
            return 0;
        }

        if (received == CANFD_MTU) {
            receiver->stats[bus].malformed_frames++;
            LOG_WARN("CAN%u received unsupported CAN FD frame", bus);
            continue;
        }
        if (received != CAN_MTU) {
            receiver->stats[bus].malformed_frames++;
            LOG_WARN("CAN%u received invalid frame size: %zd", bus, received);
            continue;
        }
        if (frame.can_dlc > CAN_MAX_DLEN) {
            receiver->stats[bus].malformed_frames++;
            LOG_WARN("CAN%u received invalid DLC: %u", bus,
                     (unsigned int)frame.can_dlc);
            continue;
        }

        if ((frame.can_id & CAN_ERR_FLAG) != 0) {
            receiver->stats[bus].error_frames++;
            can_receiver_print_error_frame(bus, &frame);
            if ((frame.can_id & CAN_ERR_BUSOFF) != 0) {
                receiver->stats[bus].bus_off_events++;
                LOG_ERROR("CAN%u (%s) bus-off detected", bus,
                          receiver->interface_name[bus]);
                can_receiver_disable_bus(receiver, bus);
                LOG_WARN("CAN%u (%s) isolated; other CAN and APP services "
                         "continue", bus, receiver->interface_name[bus]);
            }
            continue;
        }

        memset(&message, 0, sizeof(message));
        message.timestamp_us = can_receiver_timestamp_us();
        message.bus = (uint8_t)bus;
        message.id = frame.can_id & CAN_EFF_MASK;
        message.flags = (frame.can_id & CAN_EFF_FLAG) != 0
                            ? CAN_MESSAGE_FLAG_EXTENDED
                            : CAN_MESSAGE_FLAG_STANDARD;
        if ((frame.can_id & CAN_RTR_FLAG) != 0) {
            message.flags |= CAN_MESSAGE_FLAG_REMOTE;
        }
        message.dlc = frame.can_dlc;
        memcpy(message.data, frame.data, message.dlc);
        receiver->stats[bus].rx_frames++;
        if (console_log_enabled(CONSOLE_LOG_DEBUG)) {
            can_receiver_log_message(&message);
        }
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
        LOG_ERROR("epoll_create1: %s", strerror(errno));
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
            LOG_ERROR("Invalid CAN interface name for CAN%u", bus);
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
            LOG_ERROR("epoll_ctl(CAN%u): %s", bus, strerror(errno));
            can_receiver_close(receiver);
            return -1;
        }

        LOG_INFO("CAN%u: listening on %s", bus,
                 receiver->interface_name[bus]);
    }

    return 0;
}

static void can_receiver_log_rate(CanReceiver *receiver, uint64_t now_us)
{
    uint64_t elapsed_us;
    uint64_t rates[CAN_RECEIVER_BUS_COUNT];
    uint64_t total_rate = 0;
    unsigned int bus;

    if (receiver->rate_last_report_us == 0U) {
        receiver->rate_last_report_us = now_us;
        for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
            receiver->rate_last_rx_frames[bus] =
                receiver->stats[bus].rx_frames;
        }
        return;
    }

    elapsed_us = now_us - receiver->rate_last_report_us;
    if (elapsed_us < 1000000ULL) {
        return;
    }

    for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
        uint64_t frames = receiver->stats[bus].rx_frames -
                          receiver->rate_last_rx_frames[bus];

        rates[bus] = frames * 1000000ULL / elapsed_us;
        total_rate += rates[bus];
        receiver->rate_last_rx_frames[bus] = receiver->stats[bus].rx_frames;
    }
    receiver->rate_last_report_us = now_us;

    LOG_INFO("CAN: rx/s %s=%llu %s=%llu total=%llu",
             receiver->interface_name[0], (unsigned long long)rates[0],
             receiver->interface_name[1], (unsigned long long)rates[1],
             (unsigned long long)total_rate);
}

int can_receiver_run(CanReceiver *receiver)
{
    struct epoll_event events[CAN_RECEIVER_BUS_COUNT];
    uint64_t start_us;
    unsigned int bus;

    if (receiver == NULL || receiver->epoll_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    start_us = can_receiver_timestamp_us();
    if (start_us != 0U) {
        receiver->rate_last_report_us = start_us;
        for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
            receiver->rate_last_rx_frames[bus] =
                receiver->stats[bus].rx_frames;
        }
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
            LOG_ERROR("epoll_wait: %s", strerror(errno));
            return -1;
        }

        for (event_index = 0; event_index < event_count; ++event_index) {
            unsigned int bus = events[event_index].data.u32;
            uint32_t event_flags = events[event_index].events;

            if (bus >= CAN_RECEIVER_BUS_COUNT) {
                LOG_ERROR("Invalid epoll bus index: %u", bus);
                return -1;
            }
            if ((event_flags & (EPOLLERR | EPOLLHUP)) != 0) {
                receiver->stats[bus].epoll_errors++;
                LOG_ERROR("CAN%u (%s) interface event: 0x%x", bus,
                          receiver->interface_name[bus], event_flags);
                can_receiver_disable_bus(receiver, bus);
                LOG_WARN("CAN%u (%s) isolated after interface error", bus,
                         receiver->interface_name[bus]);
                continue;
            }
            if ((event_flags & EPOLLIN) != 0 &&
                receiver->socket_fd[bus] >= 0 &&
                can_receiver_handle_socket(receiver, bus) < 0) {
                return -1;
            }
        }

        {
            uint64_t now_us = can_receiver_timestamp_us();

            if (now_us != 0U) {
                can_receiver_log_rate(receiver, now_us);
            }
        }
        if (receiver->tick != NULL) {
            receiver->tick(receiver->tick_context);
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

void can_receiver_set_tick_callback(CanReceiver *receiver,
                                    CanReceiverTick tick, void *context)
{
    if (receiver == NULL) {
        return;
    }

    receiver->tick = tick;
    receiver->tick_context = context;
}

void can_receiver_print_stats(const CanReceiver *receiver)
{
    unsigned int bus;

    if (receiver == NULL) {
        return;
    }

    LOG_INFO("CAN statistics:");
    for (bus = 0; bus < CAN_RECEIVER_BUS_COUNT; ++bus) {
        const CanReceiverStats *stats = &receiver->stats[bus];

        LOG_INFO("CAN%u (%s): rx_frames=%llu error_frames=%llu "
                 "malformed_frames=%llu read_errors=%llu epoll_errors=%llu "
                 "bus_off_events=%llu interface_failed=%s", bus,
                 receiver->interface_name[bus],
                 (unsigned long long)stats->rx_frames,
                 (unsigned long long)stats->error_frames,
                 (unsigned long long)stats->malformed_frames,
                 (unsigned long long)stats->read_errors,
                 (unsigned long long)stats->epoll_errors,
                 (unsigned long long)stats->bus_off_events,
                 stats->interface_failed ? "yes" : "no");
    }
    LOG_INFO("CAN epoll_wait_errors=%llu",
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
