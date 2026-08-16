#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lappro.h"
#include "lappro_crypto.h"
#include "lappro_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#ifndef BTPROTO_L2CAP
#define BTPROTO_L2CAP 0
#endif

#define LAPPRO_ATT_CID 4U
#define LAPPRO_ATT_DEFAULT_MTU 23U
#define LAPPRO_ATT_LOCAL_MTU 247U
#define LAPPRO_ATT_OP_ERROR_RESPONSE 0x01U
#define LAPPRO_ATT_OP_EXCHANGE_MTU_REQUEST 0x02U
#define LAPPRO_ATT_OP_EXCHANGE_MTU_RESPONSE 0x03U
#define LAPPRO_ATT_OP_WRITE_REQUEST 0x12U
#define LAPPRO_ATT_OP_WRITE_RESPONSE 0x13U
#define LAPPRO_ATT_OP_HANDLE_VALUE_NOTIFICATION 0x1BU
#define LAPPRO_ATT_OP_WRITE_COMMAND 0x52U

#define LAPPRO_HANDLE_NOTIFY_VALUE 0x002AU
#define LAPPRO_HANDLE_NOTIFY_CCCD 0x002BU
#define LAPPRO_HANDLE_WRITE_VALUE 0x002DU

#define LAPPRO_RECONNECT_DELAY_MS 2000U
#define LAPPRO_RECEIVE_TIMEOUT_SEC 1
#define LAPPRO_AUTH_TIMEOUT_US 5000000ULL

enum lappro_auth_step {
    LAPPRO_AUTH_WAIT_STEP1 = 0,
    LAPPRO_AUTH_WAIT_STEP3,
    LAPPRO_AUTHENTICATED
};

static const uint8_t lappro_prefix_key[32] = {
    0x31, 0xDE, 0xA7, 0x55, 0x92, 0x0B, 0x14, 0xC7,
    0x83, 0x29, 0x71, 0x6A, 0x0D, 0xB2, 0x48, 0xE9,
    0xAF, 0x5C, 0x3E, 0x18, 0x90, 0x44, 0x72, 0xCD,
    0x12, 0xA1, 0xB5, 0x66, 0x9E, 0x37, 0x58, 0xF1
};

typedef struct {
    uint8_t b[6];
} lappro_bdaddr_t;

struct lappro_sockaddr_l2 {
    sa_family_t l2_family;
    uint16_t l2_psm;
    lappro_bdaddr_t l2_bdaddr;
    uint16_t l2_cid;
    uint8_t l2_bdaddr_type;
};

typedef struct {
    LapPro *lappro;
    LapproTransport *transport;
    int socket_fd;
    uint16_t att_mtu;
    uint64_t auth_deadline_us;
    uint8_t auth_key[LAPPRO_SHA256_DIGEST_SIZE];
    uint8_t rnd_s[32];
    uint8_t rnd_c[16];
    uint8_t session_key[32];
    enum lappro_auth_step auth_step;
    bool authenticated;
    bool auth_failed;
} lappro_connection;

static void lappro_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    fputs("[LapPro] ", stdout);
    vprintf(format, arguments);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(arguments);
}

static uint64_t lappro_monotonic_us(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) < 0) {
        return 0;
    }

    return (uint64_t)timestamp.tv_sec * 1000000ULL +
           (uint64_t)timestamp.tv_nsec / 1000ULL;
}

static uint16_t lappro_to_le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap16(value);
#endif
}

static bool lappro_should_stop(LapPro *lappro)
{
    bool stop;

    pthread_mutex_lock(&lappro->mutex);
    stop = lappro->stop_requested;
    pthread_mutex_unlock(&lappro->mutex);
    return stop;
}

static void lappro_set_active_fd(LapPro *lappro, int fd)
{
    pthread_mutex_lock(&lappro->mutex);
    lappro->active_fd = fd;
    pthread_mutex_unlock(&lappro->mutex);
}

static void lappro_set_connection_state(LapPro *lappro, bool connected)
{
    pthread_mutex_lock(&lappro->mutex);
    lappro->state.connected = connected;
    if (!connected) {
        lappro->state.authenticated = false;
        lappro->state.streaming = false;
    }
    pthread_mutex_unlock(&lappro->mutex);
}

static void lappro_set_authenticated(LapPro *lappro)
{
    pthread_mutex_lock(&lappro->mutex);
    lappro->state.authenticated = true;
    pthread_mutex_unlock(&lappro->mutex);
}

static int lappro_parse_mac(const char *text, lappro_bdaddr_t *address)
{
    const char *cursor = text;
    unsigned int index;

    for (index = 0; index < 6U; ++index) {
        char *end = NULL;
        unsigned long value;

        errno = 0;
        value = strtoul(cursor, &end, 16);
        if (errno == ERANGE || end == cursor || value > 0xFFU ||
            (end - cursor) != 2) {
            return -1;
        }
        address->b[5U - index] = (uint8_t)value;
        if (index < 5U) {
            if (*end != ':') {
                return -1;
            }
            cursor = end + 1;
        } else if (*end != '\0') {
            return -1;
        }
    }

    return 0;
}

static int lappro_random_bytes(uint8_t *data, size_t length)
{
    size_t offset = 0;
    int random_fd = -1;

    while (offset < length) {
        ssize_t received = getrandom(data + offset, length - offset, 0);

        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (offset == length) {
        return 0;
    }

    random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) {
        return -1;
    }
    while (offset < length) {
        ssize_t received = read(random_fd, data + offset, length - offset);

        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        close(random_fd);
        return -1;
    }
    close(random_fd);
    return 0;
}

static int lappro_connect(LapPro *lappro)
{
    struct lappro_sockaddr_l2 local_address;
    struct lappro_sockaddr_l2 remote_address;
    struct timeval receive_timeout;
    int socket_fd;

    socket_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (socket_fd < 0) {
        lappro_log("socket L2CAP failed: %s", strerror(errno));
        return -1;
    }

    receive_timeout.tv_sec = LAPPRO_RECEIVE_TIMEOUT_SEC;
    receive_timeout.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)) < 0) {
        lappro_log("cannot set L2CAP receive timeout: %s", strerror(errno));
        close(socket_fd);
        return -1;
    }

    memset(&remote_address, 0, sizeof(remote_address));
    remote_address.l2_family = AF_BLUETOOTH;
    remote_address.l2_bdaddr_type = lappro->config.address_type;
    remote_address.l2_cid = lappro_to_le16(LAPPRO_ATT_CID);
    if (lappro_parse_mac(lappro->config.mac, &remote_address.l2_bdaddr) < 0) {
        lappro_log("invalid LapPro MAC address: %s", lappro->config.mac);
        close(socket_fd);
        return -1;
    }

    memset(&local_address, 0, sizeof(local_address));
    local_address.l2_family = AF_BLUETOOTH;
    local_address.l2_cid = lappro_to_le16(LAPPRO_ATT_CID);
    local_address.l2_bdaddr_type = LAPPRO_BDADDR_BREDR;
    if (bind(socket_fd, (struct sockaddr *)&local_address,
             sizeof(local_address)) < 0) {
        lappro_log("L2CAP local bind failed: %s", strerror(errno));
        close(socket_fd);
        return -1;
    }

    lappro_set_active_fd(lappro, socket_fd);
    lappro_log("connecting to %s", lappro->config.mac);
    if (connect(socket_fd, (struct sockaddr *)&remote_address,
                sizeof(remote_address)) < 0) {
        if (!lappro_should_stop(lappro)) {
            lappro_log("L2CAP connect failed: %s", strerror(errno));
        }
        lappro_set_active_fd(lappro, -1);
        close(socket_fd);
        return -1;
    }

    lappro_log("connected");
    return socket_fd;
}

static int lappro_send_packet(int socket_fd, const uint8_t *packet,
                              size_t packet_length)
{
    ssize_t sent = send(socket_fd, packet, packet_length, 0);

    if (sent < 0) {
        return -1;
    }
    if ((size_t)sent != packet_length) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int lappro_send_transport(lappro_connection *connection, uint8_t type,
                                 const uint8_t *payload,
                                 size_t payload_length)
{
    uint8_t frame[LAPPRO_TRANSPORT_MAX_FRAME];
    size_t frame_length;
    size_t fragment_capacity;
    size_t offset = 0;

    if (connection->att_mtu <= 3U ||
        connection->att_mtu - 3U > sizeof(frame)) {
        return -1;
    }
    fragment_capacity = connection->att_mtu - 3U;
    if (lappro_transport_encode(type, payload, payload_length, frame,
                                sizeof(frame), &frame_length) < 0) {
        return -1;
    }

    while (offset < frame_length) {
        uint8_t packet[512];
        size_t fragment_length = frame_length - offset;

        if (fragment_length > fragment_capacity) {
            fragment_length = fragment_capacity;
        }
        if (fragment_length + 3U > sizeof(packet)) {
            return -1;
        }
        packet[0] = LAPPRO_ATT_OP_WRITE_COMMAND;
        packet[1] = (uint8_t)(LAPPRO_HANDLE_WRITE_VALUE & 0xFFU);
        packet[2] = (uint8_t)(LAPPRO_HANDLE_WRITE_VALUE >> 8U);
        memcpy(packet + 3U, frame + offset, fragment_length);
        if (lappro_send_packet(connection->socket_fd, packet,
                                fragment_length + 3U) < 0) {
            return -1;
        }
        offset += fragment_length;
    }

    return 0;
}

static void lappro_auth_failed(lappro_connection *connection,
                               const char *reason)
{
    connection->auth_failed = true;
    lappro_log("authentication failed: %s", reason);
}

static void lappro_handle_transport_frame(uint8_t type, const uint8_t *payload,
                                          size_t payload_length, void *context)
{
    lappro_connection *connection = context;

    if (type != 0x00U) {
        lappro_log("transport frame type=0x%02X payload_len=%zu", type,
                   payload_length);
        return;
    }

    if (connection->auth_step == LAPPRO_AUTH_WAIT_STEP1) {
        uint8_t step2_payload[48];
        uint8_t encrypted_random[32];

        if (payload_length != 38U) {
            lappro_auth_failed(connection, "invalid Auth Step 1 length");
            return;
        }
        memcpy(connection->rnd_s, payload + 6U, sizeof(connection->rnd_s));
        lappro_hmac_sha256(lappro_prefix_key, sizeof(lappro_prefix_key),
                           payload, 6U, connection->auth_key);
        if (lappro_random_bytes(connection->rnd_c,
                                sizeof(connection->rnd_c)) < 0) {
            lappro_auth_failed(connection, "cannot obtain random bytes");
            return;
        }
        lappro_aes256_ctr(connection->auth_key, connection->rnd_c,
                          connection->rnd_s, sizeof(connection->rnd_s),
                          encrypted_random);
        memcpy(step2_payload, connection->rnd_c, sizeof(connection->rnd_c));
        memcpy(step2_payload + sizeof(connection->rnd_c), encrypted_random,
               sizeof(encrypted_random));
        if (lappro_send_transport(connection, 0x00U, step2_payload,
                                  sizeof(step2_payload)) < 0) {
            lappro_auth_failed(connection, "cannot send Auth Step 2");
            return;
        }
        connection->auth_step = LAPPRO_AUTH_WAIT_STEP3;
        lappro_log("auth step 1 received");
        lappro_log("auth step 2 sent");
        return;
    }

    if (connection->auth_step == LAPPRO_AUTH_WAIT_STEP3) {
        uint8_t plaintext[64];
        uint8_t step4_payload = 0x00U;

        if (payload_length != 80U) {
            lappro_auth_failed(connection, "invalid Auth Step 3 length");
            return;
        }
        lappro_aes256_ctr(connection->auth_key, payload, payload + 16U, 64U,
                          plaintext);
        if (memcmp(plaintext, connection->rnd_s, sizeof(connection->rnd_s)) !=
            0) {
            lappro_auth_failed(connection, "Auth Step 3 challenge mismatch");
            return;
        }
        memcpy(connection->session_key, plaintext + 32U,
               sizeof(connection->session_key));
        lappro_log("auth step 3 verified");
        if (lappro_send_transport(connection, 0x00U, &step4_payload, 1U) < 0) {
            lappro_auth_failed(connection, "cannot send Auth Step 4");
            return;
        }
        connection->auth_step = LAPPRO_AUTHENTICATED;
        connection->authenticated = true;
        lappro_set_authenticated(connection->lappro);
        lappro_log("authenticated");
        return;
    }

    lappro_log("unexpected Auth frame after authentication");
}

static void lappro_handle_notification(lappro_connection *connection,
                                       const uint8_t *packet, size_t length)
{
    uint16_t handle;

    if (length < 3U) {
        lappro_log("malformed notification length=%zu", length);
        return;
    }
    if (packet[0] != LAPPRO_ATT_OP_HANDLE_VALUE_NOTIFICATION) {
        return;
    }

    handle = (uint16_t)packet[1] | (uint16_t)packet[2] << 8U;
    if (handle != LAPPRO_HANDLE_NOTIFY_VALUE) {
        lappro_log("notification from unexpected handle=0x%04X", handle);
        return;
    }

    if (lappro_transport_feed(connection->transport, packet + 3U,
                               length - 3U, lappro_handle_transport_frame,
                               connection) < 0) {
        lappro_log("transport feed failed");
    }
}

static int lappro_receive_packet(LapPro *lappro, int socket_fd,
                                 uint8_t *packet, size_t packet_capacity)
{
    ssize_t received;

    received = recv(socket_fd, packet, packet_capacity, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        if (!lappro_should_stop(lappro)) {
            lappro_log("L2CAP receive failed: %s", strerror(errno));
        }
        return -1;
    }
    if (received == 0) {
        lappro_log("disconnected");
        return -1;
    }

    return (int)received;
}

static bool lappro_auth_timeout_expired(const lappro_connection *connection)
{
    uint64_t now = lappro_monotonic_us();

    return now != 0U && now >= connection->auth_deadline_us;
}

static int lappro_wait_for_response(lappro_connection *connection,
                                    uint8_t expected_opcode, uint8_t *response,
                                    size_t response_capacity,
                                    size_t *response_length)
{
    uint8_t packet[512];

    for (;;) {
        int received = lappro_receive_packet(connection->lappro,
                                             connection->socket_fd, packet,
                                             sizeof(packet));
        if (received < 0) {
            return -1;
        }
        if (received == 0) {
            if (lappro_auth_timeout_expired(connection)) {
                lappro_auth_failed(connection, "authentication timeout");
                return -1;
            }
            if (lappro_should_stop(connection->lappro)) {
                return -1;
            }
            continue;
        }
        if (packet[0] == LAPPRO_ATT_OP_HANDLE_VALUE_NOTIFICATION) {
            lappro_handle_notification(connection, packet, (size_t)received);
            if (connection->auth_failed) {
                return -1;
            }
            continue;
        }
        if (packet[0] == LAPPRO_ATT_OP_ERROR_RESPONSE) {
            if (received >= 5) {
                lappro_log("ATT error request=0x%02X code=0x%02X",
                           packet[1], packet[4]);
            } else {
                lappro_log("malformed ATT error response");
            }
            return -1;
        }
        if (packet[0] == expected_opcode) {
            if (response != NULL && response_length != NULL) {
                if ((size_t)received > response_capacity) {
                    return -1;
                }
                memcpy(response, packet, (size_t)received);
                *response_length = (size_t)received;
            }
            return 0;
        }
        lappro_log("unexpected ATT opcode=0x%02X expected=0x%02X",
                   packet[0], expected_opcode);
    }
}

static int lappro_exchange_mtu(lappro_connection *connection)
{
    uint8_t request[3] = {
        LAPPRO_ATT_OP_EXCHANGE_MTU_REQUEST,
        (uint8_t)(LAPPRO_ATT_LOCAL_MTU & 0xFFU),
        (uint8_t)(LAPPRO_ATT_LOCAL_MTU >> 8)
    };

    uint8_t response[3];
    size_t response_length = 0;

    if (lappro_send_packet(connection->socket_fd, request, sizeof(request)) <
            0 ||
        lappro_wait_for_response(connection,
                                 LAPPRO_ATT_OP_EXCHANGE_MTU_RESPONSE, response,
                                 sizeof(response), &response_length) < 0) {
        lappro_log("ATT MTU exchange failed; using default MTU %u",
                   LAPPRO_ATT_DEFAULT_MTU);
        return 0;
    }

    if (response_length >= 3U) {
        uint16_t remote_mtu = (uint16_t)response[1] |
                              (uint16_t)response[2] << 8U;

        if (remote_mtu >= LAPPRO_ATT_DEFAULT_MTU) {
            connection->att_mtu = remote_mtu < LAPPRO_ATT_LOCAL_MTU
                                      ? remote_mtu
                                      : LAPPRO_ATT_LOCAL_MTU;
        }
    }

    lappro_log("ATT MTU exchange complete, mtu=%u", connection->att_mtu);
    return 0;
}

static int lappro_write_cccd(lappro_connection *connection)
{
    uint8_t request[5] = {
        LAPPRO_ATT_OP_WRITE_REQUEST,
        (uint8_t)(LAPPRO_HANDLE_NOTIFY_CCCD & 0xFFU),
        (uint8_t)(LAPPRO_HANDLE_NOTIFY_CCCD >> 8),
        0x01U,
        0x00U
    };

    if (lappro_send_packet(connection->socket_fd, request, sizeof(request)) <
            0 ||
        lappro_wait_for_response(connection, LAPPRO_ATT_OP_WRITE_RESPONSE,
                                 NULL, 0U, NULL) < 0) {
        lappro_log("FF01 notify subscription failed");
        return -1;
    }

    lappro_log("FF01 notifications enabled (CCCD 0x%04X)",
               LAPPRO_HANDLE_NOTIFY_CCCD);
    return 0;
}

static int lappro_receive_notifications(lappro_connection *connection)
{
    uint8_t packet[512];

    while (!lappro_should_stop(connection->lappro)) {
        int received = lappro_receive_packet(connection->lappro,
                                             connection->socket_fd, packet,
                                             sizeof(packet));
        if (received < 0) {
            return -1;
        }
        if (received == 0) {
            if (!connection->authenticated &&
                lappro_auth_timeout_expired(connection)) {
                lappro_auth_failed(connection, "authentication timeout");
                return -1;
            }
            continue;
        }
        if (packet[0] == LAPPRO_ATT_OP_HANDLE_VALUE_NOTIFICATION) {
            lappro_handle_notification(connection, packet, (size_t)received);
            if (connection->auth_failed) {
                return -1;
            }
        } else {
            lappro_log("received ATT opcode=0x%02X length=%d", packet[0],
                       received);
        }
    }

    return 0;
}

static void lappro_sleep_before_reconnect(LapPro *lappro)
{
    unsigned int elapsed = 0;

    while (elapsed < LAPPRO_RECONNECT_DELAY_MS &&
           !lappro_should_stop(lappro)) {
        struct timespec delay = {0, 100000000L};

        nanosleep(&delay, NULL);
        elapsed += 100U;
    }
}

static void *lappro_worker(void *context)
{
    LapPro *lappro = context;

    while (!lappro_should_stop(lappro)) {
        int socket_fd = lappro_connect(lappro);

        if (socket_fd >= 0) {
            LapproTransport transport;
            lappro_connection connection;

            lappro_transport_init(&transport);
            memset(&connection, 0, sizeof(connection));
            connection.lappro = lappro;
            connection.transport = &transport;
            connection.socket_fd = socket_fd;
            connection.att_mtu = LAPPRO_ATT_DEFAULT_MTU;
            connection.auth_deadline_us = lappro_monotonic_us() +
                                          LAPPRO_AUTH_TIMEOUT_US;
            connection.auth_step = LAPPRO_AUTH_WAIT_STEP1;
            lappro_set_connection_state(lappro, true);
            if (lappro_exchange_mtu(&connection) == 0 &&
                lappro_write_cccd(&connection) == 0) {
                lappro_receive_notifications(&connection);
            }

            lappro_set_connection_state(lappro, false);
            lappro_set_active_fd(lappro, -1);
            shutdown(socket_fd, SHUT_RDWR);
            close(socket_fd);
        }

        if (!lappro_should_stop(lappro)) {
            lappro_sleep_before_reconnect(lappro);
        }
    }

    return NULL;
}

void lappro_config_default(LapProConfig *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    snprintf(config->mac, sizeof(config->mac), "%s", LAPPRO_DEFAULT_MAC);
    config->address_type = LAPPRO_DEFAULT_ADDRESS_TYPE;
}

int lappro_init(LapPro *lappro, const LapProConfig *config)
{
    int result;

    if (lappro == NULL || config == NULL || config->mac[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (lappro_transport_self_test() < 0) {
        errno = EPROTO;
        return -1;
    }
    if (lappro_crypto_self_test() < 0) {
        errno = EPROTO;
        return -1;
    }

    memset(lappro, 0, sizeof(*lappro));
    lappro->config = *config;
    lappro->active_fd = -1;
    result = pthread_mutex_init(&lappro->mutex, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    lappro->initialized = true;
    return 0;
}

int lappro_start(LapPro *lappro)
{
    int result;

    if (lappro == NULL || !lappro->initialized || lappro->started) {
        errno = EINVAL;
        return -1;
    }

    lappro->stop_requested = false;
    result = pthread_create(&lappro->thread, NULL, lappro_worker, lappro);
    if (result != 0) {
        errno = result;
        lappro_log("worker start failed: %s", strerror(result));
        return -1;
    }
    lappro->started = true;
    lappro_log("worker started, target=%s", lappro->config.mac);
    return 0;
}

void lappro_stop(LapPro *lappro)
{
    int active_fd;

    if (lappro == NULL || !lappro->initialized) {
        return;
    }

    pthread_mutex_lock(&lappro->mutex);
    lappro->stop_requested = true;
    active_fd = lappro->active_fd;
    pthread_mutex_unlock(&lappro->mutex);

    if (active_fd >= 0) {
        shutdown(active_fd, SHUT_RDWR);
    }
    if (lappro->started) {
        pthread_join(lappro->thread, NULL);
        lappro->started = false;
    }

    pthread_mutex_destroy(&lappro->mutex);
    lappro->initialized = false;
}

bool lappro_get_state(const LapPro *lappro, LapProState *state)
{
    LapPro *mutable_lappro = (LapPro *)lappro;

    if (lappro == NULL || state == NULL || !lappro->initialized) {
        return false;
    }

    pthread_mutex_lock(&mutable_lappro->mutex);
    *state = lappro->state;
    pthread_mutex_unlock(&mutable_lappro->mutex);
    return true;
}
