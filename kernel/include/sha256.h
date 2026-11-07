/* ---------------------------------------------------------------
 * SHA-256 (FIPS 180-4)
 * --------------------------------------------------------------- */
#ifndef ZH_SHA256_H
#define ZH_SHA256_H

#include "types.h"

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

struct sha256_ctx {
    u32 state[8];
    u64 bitlen;
    u8  buffer[SHA256_BLOCK_SIZE];
    u32 buflen;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, u32 len);
void sha256_final(struct sha256_ctx *ctx, u8 out[SHA256_DIGEST_SIZE]);
void sha256(const void *data, u32 len, u8 out[SHA256_DIGEST_SIZE]);

void sha256_hex(const u8 digest[SHA256_DIGEST_SIZE], char *out);

#endif
