#include "lappro_transport.h"

#include <stdbool.h>
#include <string.h>

#define LAPPRO_TRANSPORT_MAGIC_AA 0xAAU
#define LAPPRO_TRANSPORT_MAGIC_55 0x55U
#define LAPPRO_TRANSPORT_HEADER_LENGTH 5U
#define LAPPRO_TRANSPORT_CRC_LENGTH 2U
#define LAPPRO_TRANSPORT_MIN_FRAME_LENGTH \
    (LAPPRO_TRANSPORT_HEADER_LENGTH + LAPPRO_TRANSPORT_CRC_LENGTH)

enum lappro_transport_state {
    LAPPRO_TRANSPORT_WAIT_AA = 0,
    LAPPRO_TRANSPORT_WAIT_55,
    LAPPRO_TRANSPORT_READ_TYPE,
    LAPPRO_TRANSPORT_READ_LEN_HI,
    LAPPRO_TRANSPORT_READ_LEN_LO,
    LAPPRO_TRANSPORT_READ_BODY
};

typedef struct {
    unsigned int frames;
    uint8_t type;
    size_t payload_length;
    uint8_t payload[LAPPRO_TRANSPORT_MAX_PAYLOAD];
} lappro_transport_test_capture;

uint16_t lappro_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0x0000U;
    size_t index;

    for (index = 0; index < length; ++index) {
        unsigned int bit;

        crc ^= data[index];
        for (bit = 0; bit < 8U; ++bit) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1U) ^ 0x8408U)
                             : (uint16_t)(crc >> 1U);
        }
    }

    return (uint16_t)~crc;
}

static void lappro_transport_reset_frame(LapproTransport *transport)
{
    transport->frame_length = 0;
    transport->expected_length = 0;
    transport->payload_length = 0;
    transport->state = LAPPRO_TRANSPORT_WAIT_AA;
}

void lappro_transport_init(LapproTransport *transport)
{
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
    lappro_transport_reset_frame(transport);
}

static bool lappro_transport_append(LapproTransport *transport, uint8_t value)
{
    if (transport->frame_length >= sizeof(transport->frame)) {
        transport->malformed_frames++;
        lappro_transport_reset_frame(transport);
        return false;
    }

    transport->frame[transport->frame_length++] = value;
    return true;
}

static int lappro_transport_complete_frame(
    LapproTransport *transport, LapproTransportFrameHandler handler,
    void *context)
{
    size_t crc_length = transport->expected_length - LAPPRO_TRANSPORT_CRC_LENGTH;
    uint16_t calculated_crc = lappro_crc(transport->frame, crc_length);
    uint16_t received_crc = (uint16_t)transport->frame[crc_length] |
                            (uint16_t)transport->frame[crc_length + 1U] << 8U;
    int frame_count = 0;

    if (calculated_crc != received_crc) {
        transport->crc_errors++;
        lappro_transport_reset_frame(transport);
        return 0;
    }

    transport->frames_ok++;
    if (handler != NULL) {
        handler(transport->frame[2], transport->frame + 5U,
                transport->payload_length, context);
        frame_count = 1;
    }
    lappro_transport_reset_frame(transport);
    return frame_count;
}

static int lappro_transport_feed_byte(
    LapproTransport *transport, uint8_t value,
    LapproTransportFrameHandler handler, void *context)
{
    switch (transport->state) {
    case LAPPRO_TRANSPORT_WAIT_AA:
        if (value == LAPPRO_TRANSPORT_MAGIC_AA) {
            transport->frame_length = 0;
            lappro_transport_append(transport, value);
            transport->state = LAPPRO_TRANSPORT_WAIT_55;
        }
        break;

    case LAPPRO_TRANSPORT_WAIT_55:
        if (value == LAPPRO_TRANSPORT_MAGIC_55) {
            lappro_transport_append(transport, value);
            transport->state = LAPPRO_TRANSPORT_READ_TYPE;
        } else if (value == LAPPRO_TRANSPORT_MAGIC_AA) {
            transport->frame_length = 0;
            lappro_transport_append(transport, value);
        } else {
            transport->malformed_frames++;
            lappro_transport_reset_frame(transport);
        }
        break;

    case LAPPRO_TRANSPORT_READ_TYPE:
        if (lappro_transport_append(transport, value)) {
            transport->state = LAPPRO_TRANSPORT_READ_LEN_HI;
        }
        break;

    case LAPPRO_TRANSPORT_READ_LEN_HI:
        if (lappro_transport_append(transport, value)) {
            transport->state = LAPPRO_TRANSPORT_READ_LEN_LO;
        }
        break;

    case LAPPRO_TRANSPORT_READ_LEN_LO:
        if (!lappro_transport_append(transport, value)) {
            break;
        }
        transport->payload_length =
            ((size_t)transport->frame[3] << 8U) | transport->frame[4];
        transport->expected_length = LAPPRO_TRANSPORT_HEADER_LENGTH +
                                     transport->payload_length +
                                     LAPPRO_TRANSPORT_CRC_LENGTH;
        if (transport->payload_length > LAPPRO_TRANSPORT_MAX_PAYLOAD ||
            transport->expected_length > LAPPRO_TRANSPORT_MAX_FRAME) {
            transport->malformed_frames++;
            lappro_transport_reset_frame(transport);
        } else {
            transport->state = LAPPRO_TRANSPORT_READ_BODY;
        }
        break;

    case LAPPRO_TRANSPORT_READ_BODY:
        if (!lappro_transport_append(transport, value)) {
            break;
        }
        if (transport->frame_length == transport->expected_length) {
            return lappro_transport_complete_frame(transport, handler,
                                                   context);
        }
        break;

    default:
        lappro_transport_reset_frame(transport);
        break;
    }

    return 0;
}

int lappro_transport_feed(LapproTransport *transport, const uint8_t *data,
                          size_t length,
                          LapproTransportFrameHandler handler,
                          void *context)
{
    size_t index;
    int frame_count = 0;

    if (transport == NULL || (data == NULL && length != 0U)) {
        return -1;
    }

    for (index = 0; index < length; ++index) {
        frame_count += lappro_transport_feed_byte(transport, data[index],
                                                   handler, context);
    }

    return frame_count;
}

static void lappro_transport_test_capture_frame(uint8_t type,
                                                const uint8_t *payload,
                                                size_t payload_length,
                                                void *context)
{
    lappro_transport_test_capture *capture = context;

    if (capture->frames == 0U && payload_length <= sizeof(capture->payload)) {
        capture->type = type;
        capture->payload_length = payload_length;
        memcpy(capture->payload, payload, payload_length);
    }
    capture->frames++;
}

int lappro_transport_encode(uint8_t type, const uint8_t *payload,
                            size_t payload_length, uint8_t *frame,
                            size_t frame_capacity, size_t *length)
{
    uint16_t crc;

    if (payload_length > LAPPRO_TRANSPORT_MAX_PAYLOAD ||
        payload_length + LAPPRO_TRANSPORT_MIN_FRAME_LENGTH > frame_capacity) {
        return -1;
    }

    frame[0] = LAPPRO_TRANSPORT_MAGIC_AA;
    frame[1] = LAPPRO_TRANSPORT_MAGIC_55;
    frame[2] = type;
    frame[3] = (uint8_t)(payload_length >> 8U);
    frame[4] = (uint8_t)payload_length;
    memcpy(frame + 5U, payload, payload_length);
    crc = lappro_crc(frame, 5U + payload_length);
    frame[5U + payload_length] = (uint8_t)crc;
    frame[6U + payload_length] = (uint8_t)(crc >> 8U);
    *length = 7U + payload_length;
    return 0;
}

static int lappro_transport_test_split(const uint8_t *frame, size_t length,
                                       const size_t *chunks,
                                       size_t chunk_count)
{
    LapproTransport transport;
    lappro_transport_test_capture capture;
    size_t offset = 0;
    size_t index;

    memset(&capture, 0, sizeof(capture));
    lappro_transport_init(&transport);
    for (index = 0; index < chunk_count; ++index) {
        if (chunks[index] > length - offset ||
            lappro_transport_feed(&transport, frame + offset, chunks[index],
                                  lappro_transport_test_capture_frame,
                                  &capture) < 0) {
            return -1;
        }
        offset += chunks[index];
    }

    if (offset != length || capture.frames != 1U || capture.type != 0U ||
        capture.payload_length != 38U) {
        return -1;
    }
    for (index = 0; index < capture.payload_length; ++index) {
        if (capture.payload[index] != (uint8_t)index) {
            return -1;
        }
    }

    return 0;
}

int lappro_transport_self_test(void)
{
    static const uint8_t vector_one[] = {
        0xAA, 0x55, 0x00, 0x00, 0x01, 0x00, 0xDD, 0x1C
    };
    static const uint8_t vector_two[] = {
        0xAA, 0x55, 0x01, 0x00, 0x03, 0x61, 0x62, 0x63, 0x5D, 0x5F
    };
    uint8_t fragmented_frame[7U + 38U];
    uint8_t combined_frames[sizeof(vector_one) + sizeof(vector_two)];
    uint8_t oversized_header[] = {0xAA, 0x55, 0x00, 0x10, 0x01};
    uint8_t invalid_crc[sizeof(vector_one)];
    uint8_t payload[38];
    LapproTransport transport;
    lappro_transport_test_capture capture;
    size_t fragmented_length;
    size_t index;
    static const size_t split_20_20_5[] = {20U, 20U, 5U};
    static const size_t split_1_44[] = {1U, 44U};
    static const size_t split_10_10_25[] = {10U, 10U, 25U};

    if (lappro_crc(vector_one, 6U) != 0x1CDDU ||
        lappro_crc(vector_two, 8U) != 0x5F5DU ||
        lappro_transport_encode(0x00U, (const uint8_t[]){0x00}, 1U,
                                invalid_crc, sizeof(invalid_crc), &index) < 0 ||
        memcmp(invalid_crc, vector_one, sizeof(vector_one)) != 0) {
        return -1;
    }

    for (index = 0; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)index;
    }
    if (lappro_transport_encode(0x00U, payload, sizeof(payload),
                                fragmented_frame,
                                sizeof(fragmented_frame),
                                &fragmented_length) < 0 ||
        fragmented_length != sizeof(fragmented_frame)) {
        return -1;
    }

    if (lappro_transport_test_split(fragmented_frame, fragmented_length,
                                    split_20_20_5,
                                    sizeof(split_20_20_5) /
                                        sizeof(split_20_20_5[0])) < 0 ||
        lappro_transport_test_split(fragmented_frame, fragmented_length,
                                    split_1_44,
                                    sizeof(split_1_44) / sizeof(split_1_44[0])) <
            0 ||
        lappro_transport_test_split(fragmented_frame, fragmented_length,
                                    split_10_10_25,
                                    sizeof(split_10_10_25) /
                                        sizeof(split_10_10_25[0])) < 0) {
        return -1;
    }

    memset(&capture, 0, sizeof(capture));
    lappro_transport_init(&transport);
    memcpy(combined_frames, vector_one, sizeof(vector_one));
    memcpy(combined_frames + sizeof(vector_one), vector_two,
           sizeof(vector_two));
    if (lappro_transport_feed(&transport, combined_frames,
                              sizeof(combined_frames),
                              lappro_transport_test_capture_frame,
                              &capture) != 2 ||
        capture.frames != 2U || capture.type != 0x00U ||
        capture.payload_length != 1U || capture.payload[0] != 0x00U) {
        return -1;
    }

    memcpy(invalid_crc, vector_one, sizeof(invalid_crc));
    invalid_crc[6] ^= 0x01U;
    lappro_transport_init(&transport);
    if (lappro_transport_feed(&transport, invalid_crc, sizeof(invalid_crc),
                              NULL, NULL) != 0 || transport.crc_errors != 1U) {
        return -1;
    }

    lappro_transport_init(&transport);
    if (lappro_transport_feed(&transport, oversized_header,
                              sizeof(oversized_header), NULL, NULL) != 0 ||
        transport.malformed_frames != 1U) {
        return -1;
    }

    return 0;
}
