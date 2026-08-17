#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lappro.h"
#include "lappro_crypto.h"
#include "lappro_transport.h"
#include "console_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
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
#define LAPPRO_RECEIVE_TIMEOUT_MS 100U
#define LAPPRO_AUTH_TIMEOUT_US 5000000ULL
#define LAPPRO_JSON_DELAY_US 500000ULL

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
    uint64_t authenticated_at_us;
    bool json_request_sent;
    bool json_response_received;
    bool gps_request_sent;
} lappro_connection;

static void lappro_log(const char *format, ...)
{
    char message[1024];
    va_list arguments;

    if (!console_log_enabled(CONSOLE_LOG_INFO)) {
        return;
    }

    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG_INFO("LapPro: %s", message);
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

static int lappro_wait_for_connect(LapPro *lappro, int socket_fd)
{
    struct pollfd descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = socket_fd;
    descriptor.events = POLLOUT | POLLERR | POLLHUP;

    for (;;) {
        int poll_result;
        int socket_error;
        socklen_t socket_error_length = sizeof(socket_error);

        if (lappro_should_stop(lappro)) {
            errno = ECANCELED;
            return -1;
        }

        poll_result = poll(&descriptor, 1, LAPPRO_RECEIVE_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (poll_result == 0) {
            continue;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return -1;
        }

        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_length) < 0) {
            return -1;
        }
        if (socket_error != 0) {
            errno = socket_error;
            return -1;
        }
        if ((descriptor.revents & POLLOUT) != 0) {
            return 0;
        }

        errno = ECONNABORTED;
        return -1;
    }
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
    int socket_flags;
    int socket_fd;

    socket_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (socket_fd < 0) {
        lappro_log("socket L2CAP failed: %s", strerror(errno));
        return -1;
    }

    receive_timeout.tv_sec = 0;
    receive_timeout.tv_usec = LAPPRO_RECEIVE_TIMEOUT_MS * 1000U;
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
    if (lappro_should_stop(lappro)) {
        lappro_set_active_fd(lappro, -1);
        close(socket_fd);
        errno = ECANCELED;
        return -1;
    }

    socket_flags = fcntl(socket_fd, F_GETFL, 0);
    if (socket_flags < 0 ||
        fcntl(socket_fd, F_SETFL, socket_flags | O_NONBLOCK) < 0) {
        lappro_log("cannot make L2CAP socket non-blocking: %s",
                   strerror(errno));
        lappro_set_active_fd(lappro, -1);
        close(socket_fd);
        return -1;
    }

    lappro_log("connecting to %s", lappro->config.mac);
    if (connect(socket_fd, (struct sockaddr *)&remote_address,
                sizeof(remote_address)) < 0 &&
        (errno != EINPROGRESS ||
         lappro_wait_for_connect(lappro, socket_fd) < 0)) {
        if (!lappro_should_stop(lappro)) {
            lappro_log("L2CAP connect failed: %s", strerror(errno));
        }
        lappro_set_active_fd(lappro, -1);
        close(socket_fd);
        return -1;
    }

    if (fcntl(socket_fd, F_SETFL, socket_flags) < 0) {
        lappro_log("cannot restore L2CAP socket mode: %s", strerror(errno));
        lappro_set_active_fd(lappro, -1);
        close(socket_fd);
        return -1;
    }

    if (lappro_should_stop(lappro)) {
        lappro_set_active_fd(lappro, -1);
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
        errno = ECANCELED;
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

static int lappro_send_json_request(lappro_connection *connection,
                                    const char *request)
{
    uint8_t iv[LAPPRO_AES_BLOCK_SIZE];
    uint8_t encrypted[LAPPRO_TRANSPORT_MAX_PAYLOAD];
    uint8_t payload[LAPPRO_AES_BLOCK_SIZE + LAPPRO_TRANSPORT_MAX_PAYLOAD];
    size_t request_length = strlen(request);

    if (request_length > LAPPRO_TRANSPORT_MAX_PAYLOAD -
                             LAPPRO_AES_BLOCK_SIZE) {
        return -1;
    }

    if (lappro_random_bytes(iv, sizeof(iv)) < 0) {
        return -1;
    }
    lappro_aes256_ctr(connection->session_key, iv, (const uint8_t *)request,
                      request_length, encrypted);
    memcpy(payload, iv, sizeof(iv));
    memcpy(payload + sizeof(iv), encrypted, request_length);
    return lappro_send_transport(connection, 0x01U, payload,
                                 sizeof(iv) + request_length);
}

static int lappro_send_properties_request(lappro_connection *connection)
{
    static const char request[] =
        "{\"method\":\"read\",\"properties\":[\"product\",\"active\",\"bleMac\",\"mtu\"]}";

    if (lappro_send_json_request(connection, request) < 0) {
        return -1;
    }
    connection->json_request_sent = true;
    lappro_log("product query sent");
    return 0;
}

static int lappro_send_gps_enable_request(lappro_connection *connection)
{
    static const char request[] =
        "{\"method\":\"write\",\"properties\":{\"gpsDataReport\":1}}";

    if (lappro_send_json_request(connection, request) < 0) {
        return -1;
    }
    connection->gps_request_sent = true;
    lappro_log("GPS stream enabled");
    return 0;
}

static void lappro_handle_json_frame(lappro_connection *connection,
                                     const uint8_t *payload,
                                     size_t payload_length)
{
    uint8_t plaintext[LAPPRO_TRANSPORT_MAX_PAYLOAD];
    size_t ciphertext_length;

    if (payload_length < LAPPRO_AES_BLOCK_SIZE ||
        payload_length - LAPPRO_AES_BLOCK_SIZE >= sizeof(plaintext)) {
        lappro_log("invalid JSON payload length=%zu", payload_length);
        return;
    }

    ciphertext_length = payload_length - LAPPRO_AES_BLOCK_SIZE;
    lappro_aes256_ctr(connection->session_key, payload, payload + 16U,
                      ciphertext_length, plaintext);
    plaintext[ciphertext_length] = '\0';
    lappro_log("product response: %s", (const char *)plaintext);
    if (!connection->json_response_received) {
        connection->json_response_received = true;
        if (lappro_send_gps_enable_request(connection) < 0) {
            lappro_auth_failed(connection, "cannot enable GPS stream");
        }
    }
}

static uint16_t lappro_read_be16(const uint8_t *data)
{
    return (uint16_t)data[0] << 8U | data[1];
}

static uint32_t lappro_read_be32(const uint8_t *data)
{
    return (uint32_t)data[0] << 24U | (uint32_t)data[1] << 16U |
           (uint32_t)data[2] << 8U | data[3];
}

static void lappro_handle_gps_frame(lappro_connection *connection,
                                    const uint8_t *payload,
                                    size_t payload_length)
{
    uint8_t plaintext[27];
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    unsigned int millisecond;
    uint8_t fix_type;
    uint8_t num_sv;
    double speed_kmh;
    double heading_deg;
    double hacc_m;
    double altitude;
    double longitude;
    double latitude;
    double acc_x;
    double acc_y;
    double acc_z;
    uint64_t timestamp_us;

    if (payload_length != LAPPRO_AES_BLOCK_SIZE + sizeof(plaintext)) {
        lappro_log("invalid GPS payload length=%zu", payload_length);
        return;
    }

    lappro_aes256_ctr(connection->session_key, payload, payload + 16U,
                      sizeof(plaintext), plaintext);
    year = 2025U + (unsigned int)(plaintext[0] >> 4U);
    month = plaintext[0] & 0x0FU;
    day = plaintext[1] >> 3U;
    fix_type = plaintext[1] & 0x07U;
    hour = plaintext[2] >> 3U;
    minute = ((unsigned int)(plaintext[2] & 0x07U) << 3U) |
             (plaintext[3] >> 5U);
    second = ((unsigned int)(plaintext[3] & 0x1FU) << 1U) |
             (plaintext[4] >> 7U);
    millisecond = ((unsigned int)(plaintext[4] & 0x7FU) << 3U) |
                  (plaintext[5] >> 5U);
    num_sv = plaintext[5] & 0x1FU;
    speed_kmh = (double)lappro_read_be16(plaintext + 6U) / 100.0;
    heading_deg = (double)lappro_read_be16(plaintext + 8U) / 100.0;
    hacc_m = (double)plaintext[10] / 10.0;
    altitude = (double)lappro_read_be16(plaintext + 11U);
    longitude = (double)lappro_read_be32(plaintext + 13U) / 10000000.0;
    latitude = (double)lappro_read_be32(plaintext + 17U) / 10000000.0;
    acc_x = (double)(int16_t)lappro_read_be16(plaintext + 21U) / 4096.0;
    acc_y = (double)(int16_t)lappro_read_be16(plaintext + 23U) / 4096.0;
    acc_z = (double)(int16_t)lappro_read_be16(plaintext + 25U) / 4096.0;
    timestamp_us = lappro_monotonic_us();

    pthread_mutex_lock(&connection->lappro->mutex);
    connection->lappro->state.speed_kmh = speed_kmh;
    connection->lappro->state.heading_deg = heading_deg;
    connection->lappro->state.hacc_m = hacc_m;
    connection->lappro->state.altitude = altitude;
    connection->lappro->state.longitude = longitude;
    connection->lappro->state.latitude = latitude;
    connection->lappro->state.acc_x = acc_x;
    connection->lappro->state.acc_y = acc_y;
    connection->lappro->state.acc_z = acc_z;
    connection->lappro->state.fix_type = fix_type;
    connection->lappro->state.num_sv = num_sv;
    connection->lappro->state.last_update_monotonic_us = timestamp_us;
    connection->lappro->state.streaming = true;
    pthread_mutex_unlock(&connection->lappro->mutex);

    lappro_log("GPS %04u-%02u-%02u %02u:%02u:%02u.%03u fix=%u sv=%u "
               "speed=%.2f heading=%.2f hAcc=%.1f altitude=%.0f "
               "lat=%.7f lon=%.7f acc=(%.4f,%.4f,%.4f)",
               year, month, day, hour, minute, second, millisecond,
               (unsigned int)fix_type, (unsigned int)num_sv, speed_kmh,
               heading_deg, hacc_m, altitude, latitude, longitude, acc_x,
               acc_y, acc_z);
}

static void lappro_handle_transport_frame(uint8_t type, const uint8_t *payload,
                                          size_t payload_length, void *context)
{
    lappro_connection *connection = context;

    if (type == 0x00U) {
        if (connection->auth_step == LAPPRO_AUTH_WAIT_STEP1) {
            uint8_t step2_payload[48];
            uint8_t encrypted_random[32];

            if (payload_length != 38U) {
                lappro_auth_failed(connection, "invalid Auth Step 1 length");
                return;
            }
            memcpy(connection->rnd_s, payload + 6U,
                   sizeof(connection->rnd_s));
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
            memcpy(step2_payload, connection->rnd_c,
                   sizeof(connection->rnd_c));
            memcpy(step2_payload + sizeof(connection->rnd_c),
                   encrypted_random, sizeof(encrypted_random));
            if (lappro_send_transport(connection, 0x00U, step2_payload,
                                      sizeof(step2_payload)) < 0) {
                lappro_auth_failed(connection, "cannot send Auth Step 2");
                return;
            }
            connection->auth_step = LAPPRO_AUTH_WAIT_STEP3;
            lappro_log("auth step 1 received");
            lappro_log("auth step 2 sent");
        } else if (connection->auth_step == LAPPRO_AUTH_WAIT_STEP3) {
            uint8_t plaintext[64];
            uint8_t step4_payload = 0x00U;

            if (payload_length != 80U) {
                lappro_auth_failed(connection, "invalid Auth Step 3 length");
                return;
            }
            lappro_aes256_ctr(connection->auth_key, payload, payload + 16U,
                              64U, plaintext);
            if (memcmp(plaintext, connection->rnd_s,
                       sizeof(connection->rnd_s)) != 0) {
                lappro_auth_failed(connection,
                                   "Auth Step 3 challenge mismatch");
                return;
            }
            memcpy(connection->session_key, plaintext + 32U,
                   sizeof(connection->session_key));
            lappro_log("auth step 3 verified");
            if (lappro_send_transport(connection, 0x00U, &step4_payload, 1U) <
                0) {
                lappro_auth_failed(connection, "cannot send Auth Step 4");
                return;
            }
            connection->auth_step = LAPPRO_AUTHENTICATED;
            connection->authenticated = true;
            connection->authenticated_at_us = lappro_monotonic_us();
            lappro_set_authenticated(connection->lappro);
            lappro_log("authenticated");
        } else {
            lappro_log("unexpected Auth frame after authentication");
        }
        return;
    }

    if (type == 0x01U && connection->authenticated) {
        lappro_handle_json_frame(connection, payload, payload_length);
        return;
    }

    if (type == 0x02U && connection->authenticated) {
        lappro_handle_gps_frame(connection, payload, payload_length);
        return;
    }

    lappro_log("transport frame type=0x%02X payload_len=%zu", type,
               payload_length);
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

static int lappro_maybe_send_properties_request(lappro_connection *connection)
{
    uint64_t now;

    if (!connection->authenticated || connection->json_request_sent) {
        return 0;
    }
    now = lappro_monotonic_us();
    if (now == 0U || now < connection->authenticated_at_us +
                              LAPPRO_JSON_DELAY_US) {
        return 0;
    }
    if (lappro_send_properties_request(connection) < 0) {
        lappro_log("cannot send product query");
        return -1;
    }
    return 0;
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

    if (lappro->raw_sink != NULL) {
        lappro->raw_sink(lappro_monotonic_us(), packet, (size_t)received,
                         lappro->raw_sink_context);
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
        if (lappro_maybe_send_properties_request(connection) < 0) {
            return -1;
        }
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
            if (!lappro_should_stop(lappro)) {
                lappro_set_connection_state(lappro, true);
                if (lappro_exchange_mtu(&connection) == 0 &&
                    lappro_write_cccd(&connection) == 0) {
                    lappro_receive_notifications(&connection);
                }
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

void lappro_set_raw_sink(LapPro *lappro, LapProRawSink sink, void *context)
{
    if (lappro == NULL || !lappro->initialized || lappro->started) {
        return;
    }

    lappro->raw_sink = sink;
    lappro->raw_sink_context = context;
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
