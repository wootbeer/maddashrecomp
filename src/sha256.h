#ifndef MDR_SHA256_H
#define MDR_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct mdr_sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t  block[64];
    size_t   used;
} mdr_sha256;

void mdr_sha256_init(mdr_sha256 *ctx);
void mdr_sha256_update(mdr_sha256 *ctx, const void *data, size_t size);
void mdr_sha256_final(mdr_sha256 *ctx, uint8_t out[32]);

/* Lower-case hex of a 32-byte digest into out (65 bytes incl. terminator). */
void mdr_sha256_hex(const uint8_t digest[32], char out[65]);

#endif
