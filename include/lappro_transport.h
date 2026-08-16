#ifndef T113_LAPPRO_TRANSPORT_H
#define T113_LAPPRO_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define LAPPRO_TRANSPORT_MAX_PAYLOAD 4096U
#define LAPPRO_TRANSPORT_MAX_FRAME 8192U

typedef struct {
    uint8_t frame[LAPPRO_TRANSPORT_MAX_FRAME];
    size_t frame_length;
    size_t expected_length;
    size_t payload_length;
    uint64_t frames_ok;
    uint64_t crc_errors;
    uint64_t malformed_frames;
    uint8_t state;
} LapproTransport;

typedef void (*LapproTransportFrameHandler)(uint8_t type,
                                            const uint8_t *payload,
                                            size_t payload_length,
                                            void *context);

uint16_t lappro_crc(const uint8_t *data, size_t length);

void lappro_transport_init(LapproTransport *transport);

int lappro_transport_encode(uint8_t type, const uint8_t *payload,
                            size_t payload_length, uint8_t *frame,
                            size_t frame_capacity, size_t *frame_length);

int lappro_transport_feed(LapproTransport *transport, const uint8_t *data,
                          size_t length,
                          LapproTransportFrameHandler handler,
                          void *context);

int lappro_transport_self_test(void);

#endif
