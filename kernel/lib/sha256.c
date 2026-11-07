#include "sha256.h"
#include "string.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const u32 k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_transform(struct sha256_ctx *ctx, const u8 block[64])
{
    u32 w[64];
    u32 a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++) {
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        u32 t1 = h + EP1(e) + CH(e, f, g) + k[i] + w[i];
        u32 t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;

    while (len--) {
        ctx->buffer[ctx->buflen++] = *p++;
        if (ctx->buflen == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->buffer);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *ctx, u8 out[SHA256_DIGEST_SIZE])
{
    u32 i = ctx->buflen;

    ctx->bitlen += (u64)ctx->buflen * 8;

    ctx->buffer[i++] = 0x80;
    if (i > 56) {
        while (i < SHA256_BLOCK_SIZE) {
            ctx->buffer[i++] = 0;
        }
        sha256_transform(ctx, ctx->buffer);
        i = 0;
    }
    while (i < 56) {
        ctx->buffer[i++] = 0;
    }
    for (int b = 7; b >= 0; b--) {
        ctx->buffer[i++] = (u8)(ctx->bitlen >> (b * 8));
    }
    sha256_transform(ctx, ctx->buffer);

    for (int b = 0; b < 8; b++) {
        out[b * 4 + 0] = (u8)(ctx->state[b] >> 24);
        out[b * 4 + 1] = (u8)(ctx->state[b] >> 16);
        out[b * 4 + 2] = (u8)(ctx->state[b] >> 8);
        out[b * 4 + 3] = (u8)(ctx->state[b]);
    }
}

void sha256(const void *data, u32 len, u8 out[SHA256_DIGEST_SIZE])
{
    struct sha256_ctx ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void sha256_hex(const u8 digest[SHA256_DIGEST_SIZE], char *out)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[SHA256_DIGEST_SIZE * 2] = '\0';
}
