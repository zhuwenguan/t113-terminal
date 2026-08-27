#ifndef T113_LAPPRO_CRYPTO_H
#define T113_LAPPRO_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define LAPPRO_SHA256_DIGEST_SIZE 32U
#define LAPPRO_AES256_KEY_SIZE 32U
#define LAPPRO_AES_BLOCK_SIZE 16U

void lappro_sha256(const uint8_t *data, size_t length,
                   uint8_t digest[LAPPRO_SHA256_DIGEST_SIZE]);

void lappro_hmac_sha256(const uint8_t *key, size_t key_length,
                        const uint8_t *message, size_t message_length,
                        uint8_t digest[LAPPRO_SHA256_DIGEST_SIZE]);

void lappro_aes256_ctr(const uint8_t key[LAPPRO_AES256_KEY_SIZE],
                       const uint8_t counter[LAPPRO_AES_BLOCK_SIZE],
                       const uint8_t *input, size_t length, uint8_t *output);

int lappro_crypto_self_test(void);

#endif
