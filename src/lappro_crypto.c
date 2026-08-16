#include "lappro_crypto.h"

#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t block[64];
    size_t block_length;
} lappro_sha256_context;

static const uint32_t lappro_sha256_constants[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};

static const uint8_t lappro_aes_sbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5,
    0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0,
    0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC,
    0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A,
    0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0,
    0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B,
    0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85,
    0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5,
    0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17,
    0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88,
    0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C,
    0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9,
    0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6,
    0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E,
    0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94,
    0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68,
    0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16
};

static uint32_t lappro_rotr32(uint32_t value, unsigned int amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t lappro_sha256_big_endian(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

static void lappro_sha256_store_big_endian(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static void lappro_sha256_transform(lappro_sha256_context *context,
                                    const uint8_t block[64])
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned int index;

    for (index = 0; index < 16U; ++index) {
        words[index] = lappro_sha256_big_endian(block + index * 4U);
    }
    for (index = 16U; index < 64U; ++index) {
        uint32_t s0 = lappro_rotr32(words[index - 15U], 7U) ^
                      lappro_rotr32(words[index - 15U], 18U) ^
                      (words[index - 15U] >> 3U);
        uint32_t s1 = lappro_rotr32(words[index - 2U], 17U) ^
                      lappro_rotr32(words[index - 2U], 19U) ^
                      (words[index - 2U] >> 10U);

        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (index = 0; index < 64U; ++index) {
        uint32_t s1 = lappro_rotr32(e, 6U) ^ lappro_rotr32(e, 11U) ^
                      lappro_rotr32(e, 25U);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temporary1 = h + s1 + choice +
                              lappro_sha256_constants[index] + words[index];
        uint32_t s0 = lappro_rotr32(a, 2U) ^ lappro_rotr32(a, 13U) ^
                      lappro_rotr32(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void lappro_sha256_init(lappro_sha256_context *context)
{
    static const uint32_t initial_state[8] = {
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U
    };

    memcpy(context->state, initial_state, sizeof(initial_state));
    context->bit_length = 0;
    context->block_length = 0;
}

static void lappro_sha256_update(lappro_sha256_context *context,
                                 const uint8_t *data, size_t length)
{
    while (length > 0U) {
        size_t copy_length = sizeof(context->block) - context->block_length;

        if (copy_length > length) {
            copy_length = length;
        }
        memcpy(context->block + context->block_length, data, copy_length);
        context->block_length += copy_length;
        context->bit_length += (uint64_t)copy_length * 8ULL;
        data += copy_length;
        length -= copy_length;

        if (context->block_length == sizeof(context->block)) {
            lappro_sha256_transform(context, context->block);
            context->block_length = 0;
        }
    }
}

static void lappro_sha256_final(lappro_sha256_context *context,
                                uint8_t digest[LAPPRO_SHA256_DIGEST_SIZE])
{
    size_t index;
    uint64_t bit_length = context->bit_length;

    context->block[context->block_length++] = 0x80U;
    if (context->block_length > 56U) {
        memset(context->block + context->block_length, 0,
               sizeof(context->block) - context->block_length);
        lappro_sha256_transform(context, context->block);
        context->block_length = 0;
    }
    memset(context->block + context->block_length, 0,
           56U - context->block_length);
    for (index = 0; index < 8U; ++index) {
        context->block[56U + index] =
            (uint8_t)(bit_length >> (56U - index * 8U));
    }
    lappro_sha256_transform(context, context->block);

    for (index = 0; index < 8U; ++index) {
        lappro_sha256_store_big_endian(digest + index * 4U,
                                       context->state[index]);
    }
}

void lappro_sha256(const uint8_t *data, size_t length,
                   uint8_t digest[LAPPRO_SHA256_DIGEST_SIZE])
{
    lappro_sha256_context context;

    lappro_sha256_init(&context);
    lappro_sha256_update(&context, data, length);
    lappro_sha256_final(&context, digest);
}

void lappro_hmac_sha256(const uint8_t *key, size_t key_length,
                        const uint8_t *message, size_t message_length,
                        uint8_t digest[LAPPRO_SHA256_DIGEST_SIZE])
{
    uint8_t key_block[64];
    uint8_t inner_digest[LAPPRO_SHA256_DIGEST_SIZE];
    uint8_t inner_pad[64];
    uint8_t outer_pad[64];
    lappro_sha256_context context;
    size_t index;

    memset(key_block, 0, sizeof(key_block));
    if (key_length > sizeof(key_block)) {
        lappro_sha256(key, key_length, key_block);
    } else {
        memcpy(key_block, key, key_length);
    }

    for (index = 0; index < sizeof(key_block); ++index) {
        inner_pad[index] = key_block[index] ^ 0x36U;
        outer_pad[index] = key_block[index] ^ 0x5CU;
    }

    lappro_sha256_init(&context);
    lappro_sha256_update(&context, inner_pad, sizeof(inner_pad));
    lappro_sha256_update(&context, message, message_length);
    lappro_sha256_final(&context, inner_digest);

    lappro_sha256_init(&context);
    lappro_sha256_update(&context, outer_pad, sizeof(outer_pad));
    lappro_sha256_update(&context, inner_digest, sizeof(inner_digest));
    lappro_sha256_final(&context, digest);
}

static uint8_t lappro_aes_xtime(uint8_t value)
{
    return (uint8_t)((value << 1U) ^ ((value >> 7U) * 0x1BU));
}

static void lappro_aes_key_expand(const uint8_t key[LAPPRO_AES256_KEY_SIZE],
                                  uint8_t round_keys[240])
{
    unsigned int generated = LAPPRO_AES256_KEY_SIZE;
    uint8_t rcon = 0x01U;

    memcpy(round_keys, key, LAPPRO_AES256_KEY_SIZE);
    while (generated < 240U) {
        uint8_t temporary[4];
        unsigned int index;

        memcpy(temporary, round_keys + generated - 4U, sizeof(temporary));
        if (generated % LAPPRO_AES256_KEY_SIZE == 0U) {
            uint8_t first = temporary[0];

            temporary[0] = lappro_aes_sbox[temporary[1]] ^ rcon;
            temporary[1] = lappro_aes_sbox[temporary[2]];
            temporary[2] = lappro_aes_sbox[temporary[3]];
            temporary[3] = lappro_aes_sbox[first];
            rcon = lappro_aes_xtime(rcon);
        } else if (generated % LAPPRO_AES256_KEY_SIZE == 16U) {
            temporary[0] = lappro_aes_sbox[temporary[0]];
            temporary[1] = lappro_aes_sbox[temporary[1]];
            temporary[2] = lappro_aes_sbox[temporary[2]];
            temporary[3] = lappro_aes_sbox[temporary[3]];
        }

        for (index = 0; index < sizeof(temporary); ++index) {
            round_keys[generated] =
                round_keys[generated - LAPPRO_AES256_KEY_SIZE] ^
                temporary[index];
            generated++;
            temporary[index] = round_keys[generated - 4U];
        }
    }
}

static void lappro_aes_add_round_key(uint8_t state[16],
                                     const uint8_t round_keys[240],
                                     unsigned int round)
{
    unsigned int index;

    for (index = 0; index < 16U; ++index) {
        state[index] ^= round_keys[round * 16U + index];
    }
}

static void lappro_aes_sub_bytes(uint8_t state[16])
{
    unsigned int index;

    for (index = 0; index < 16U; ++index) {
        state[index] = lappro_aes_sbox[state[index]];
    }
}

static void lappro_aes_shift_rows(uint8_t state[16])
{
    uint8_t shifted[16];
    unsigned int row;
    unsigned int column;

    for (row = 0; row < 4U; ++row) {
        for (column = 0; column < 4U; ++column) {
            shifted[4U * column + row] =
                state[4U * ((column + row) % 4U) + row];
        }
    }
    memcpy(state, shifted, sizeof(shifted));
}

static void lappro_aes_mix_columns(uint8_t state[16])
{
    unsigned int column;

    for (column = 0; column < 4U; ++column) {
        uint8_t *value = state + column * 4U;
        uint8_t first = value[0];
        uint8_t second = value[1];
        uint8_t third = value[2];
        uint8_t fourth = value[3];

        value[0] = lappro_aes_xtime(first) ^
                   (lappro_aes_xtime(second) ^ second) ^ third ^ fourth;
        value[1] = first ^ lappro_aes_xtime(second) ^
                   (lappro_aes_xtime(third) ^ third) ^ fourth;
        value[2] = first ^ second ^ lappro_aes_xtime(third) ^
                   (lappro_aes_xtime(fourth) ^ fourth);
        value[3] = (lappro_aes_xtime(first) ^ first) ^ second ^ third ^
                   lappro_aes_xtime(fourth);
    }
}

static void lappro_aes_encrypt_block(const uint8_t input[16],
                                     const uint8_t round_keys[240],
                                     uint8_t output[16])
{
    unsigned int round;

    memcpy(output, input, 16U);
    lappro_aes_add_round_key(output, round_keys, 0U);
    for (round = 1U; round < 14U; ++round) {
        lappro_aes_sub_bytes(output);
        lappro_aes_shift_rows(output);
        lappro_aes_mix_columns(output);
        lappro_aes_add_round_key(output, round_keys, round);
    }
    lappro_aes_sub_bytes(output);
    lappro_aes_shift_rows(output);
    lappro_aes_add_round_key(output, round_keys, 14U);
}

void lappro_aes256_ctr(const uint8_t key[LAPPRO_AES256_KEY_SIZE],
                       const uint8_t counter[LAPPRO_AES_BLOCK_SIZE],
                       const uint8_t *input, size_t length, uint8_t *output)
{
    uint8_t round_keys[240];
    uint8_t counter_block[LAPPRO_AES_BLOCK_SIZE];
    uint8_t stream[LAPPRO_AES_BLOCK_SIZE];

    lappro_aes_key_expand(key, round_keys);
    memcpy(counter_block, counter, sizeof(counter_block));
    while (length > 0U) {
        size_t block_length = length < sizeof(stream) ? length : sizeof(stream);
        unsigned int index;

        lappro_aes_encrypt_block(counter_block, round_keys, stream);
        for (index = 0; index < block_length; ++index) {
            output[index] = input[index] ^ stream[index];
        }
        input += block_length;
        output += block_length;
        length -= block_length;

        for (index = sizeof(counter_block); index > 0U; --index) {
            counter_block[index - 1U]++;
            if (counter_block[index - 1U] != 0U) {
                break;
            }
        }
    }
}

static int lappro_crypto_compare(const uint8_t *actual, const uint8_t *expected,
                                 size_t length)
{
    return memcmp(actual, expected, length) == 0 ? 0 : -1;
}

int lappro_crypto_self_test(void)
{
    static const uint8_t sha_expected[] = {
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
    };
    static const uint8_t hmac_expected[] = {
        0xF7, 0xBC, 0x83, 0xF4, 0x30, 0x53, 0x84, 0x24,
        0xB1, 0x32, 0x98, 0xE6, 0xAA, 0x6F, 0xB1, 0x43,
        0xEF, 0x4D, 0x59, 0xA1, 0x49, 0x46, 0x17, 0x59,
        0x97, 0x47, 0x9D, 0xBC, 0x2D, 0x1A, 0x3C, 0xD8
    };
    static const uint8_t aes_key[] = {
        0x60, 0x3D, 0xEB, 0x10, 0x15, 0xCA, 0x71, 0xBE,
        0x2B, 0x73, 0xAE, 0xF0, 0x85, 0x7D, 0x77, 0x81,
        0x1F, 0x35, 0x2C, 0x07, 0x3B, 0x61, 0x08, 0xD7,
        0x2D, 0x98, 0x10, 0xA3, 0x09, 0x14, 0xDF, 0xF4
    };
    static const uint8_t aes_counter[] = {
        0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
        0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
    };
    static const uint8_t aes_plaintext[] = {
        0x6B, 0xC1, 0xBE, 0xE2, 0x2E, 0x40, 0x9F, 0x96,
        0xE9, 0x3D, 0x7E, 0x11, 0x73, 0x93, 0x17, 0x2A,
        0xAE, 0x2D, 0x8A, 0x57, 0x1E, 0x03, 0xAC, 0x9C,
        0x9E, 0xB7, 0x6F, 0xAC, 0x45, 0xAF, 0x8E, 0x51
    };
    static const uint8_t aes_expected[] = {
        0x60, 0x1E, 0xC3, 0x13, 0x77, 0x57, 0x89, 0xA5,
        0xB7, 0xA7, 0xF5, 0x04, 0xBB, 0xF3, 0xD2, 0x28,
        0xF4, 0x43, 0xE3, 0xCA, 0x4D, 0x62, 0xB5, 0x9A,
        0xCA, 0x84, 0xE9, 0x90, 0xCA, 0xCA, 0xF5, 0xC5
    };
    static const uint8_t hmac_key[] = "key";
    static const uint8_t hmac_message[] =
        "The quick brown fox jumps over the lazy dog";
    uint8_t actual[LAPPRO_SHA256_DIGEST_SIZE];
    uint8_t aes_actual[sizeof(aes_plaintext)];

    lappro_sha256((const uint8_t *)"abc", 3U, actual);
    if (lappro_crypto_compare(actual, sha_expected, sizeof(sha_expected)) < 0) {
        return -1;
    }
    lappro_hmac_sha256(hmac_key, sizeof(hmac_key) - 1U, hmac_message,
                       sizeof(hmac_message) - 1U, actual);
    if (lappro_crypto_compare(actual, hmac_expected, sizeof(hmac_expected)) <
        0) {
        return -1;
    }
    lappro_aes256_ctr(aes_key, aes_counter, aes_plaintext,
                      sizeof(aes_plaintext), aes_actual);
    return lappro_crypto_compare(aes_actual, aes_expected,
                                 sizeof(aes_expected));
}
