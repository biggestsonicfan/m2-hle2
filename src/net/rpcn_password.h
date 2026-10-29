/*
 * rpcn_password.h — the key RPCS3 sends to RPCN in place of the password.
 *
 * RPCS3 NEVER SENDS WHAT THE PLAYER TYPED. Its RPCN dialogs run the typed
 * password through PBKDF2-HMAC-SHA3-256 (200,000 rounds, salt "No matter where
 * you go, everybody's connected.") and send the 32-byte result as 64 UPPERCASE
 * hex characters (`derive_password`, rpcs3qt/rpcn_settings_dialog.cpp). Both
 * account creation and login do it, so the server only ever stores the key: an
 * account made in RPCS3 refuses its own password typed here as ErrorType=8,
 * "wrong password for that account" (issue #548). rpcs3.yml holds the key, not
 * the password, which is why a password copied out of it works as is.
 *
 * SHA-3 is here because no backend of tls.h offers it everywhere (Schannel has
 * none before Windows 11, the web build has no TLS library at all). It costs
 * two Keccak permutations a round: the HMAC pads are a whole 136-byte block
 * each, so they are absorbed once and their states reused.
 */
#ifndef RPCN_PASSWORD_H
#define RPCN_PASSWORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RPCN_KEY_ROUNDS   200000u
#define RPCN_KEY_SALT     "No matter where you go, everybody's connected."
#define RPCN_KEY_HEX_LEN  64            /* what goes over the wire */

#define SHA3_256_RATE     136u          /* bytes absorbed per permutation */
#define SHA3_256_BYTES    32u

typedef struct {
    uint64_t a[25];
    uint8_t  buf[SHA3_256_RATE];
    uint32_t n;                          /* bytes in buf */
} sha3_256_t;

static inline uint64_t sha3_rotl(uint64_t x, unsigned n) {
    return n ? (x << n) | (x >> (64 - n)) : x;
}

static inline void keccak_f1600(uint64_t a[25]) {
    static const uint64_t rc[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808Aull, 0x8000000080008000ull,
        0x000000000000808Bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
        0x000000000000008Aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000Aull,
        0x000000008000808Bull, 0x800000000000008Bull, 0x8000000000008089ull, 0x8000000000008003ull,
        0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800Aull, 0x800000008000000Aull,
        0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull,
    };
    static const uint8_t rho[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
                                     27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
    static const uint8_t pi[24]  = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
                                     15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };
    for (int r = 0; r < 24; r++) {
        uint64_t c[5];
        for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (int x = 0; x < 5; x++) {
            uint64_t d = c[(x + 4) % 5] ^ sha3_rotl(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) a[y + x] ^= d;
        }
        uint64_t t = a[1];
        for (int i = 0; i < 24; i++) {
            uint64_t u = a[pi[i]];
            a[pi[i]] = sha3_rotl(t, rho[i]);
            t = u;
        }
        for (int y = 0; y < 25; y += 5) {
            uint64_t row[5];
            memcpy(row, &a[y], sizeof(row));
            for (int x = 0; x < 5; x++) a[y + x] = row[x] ^ (~row[(x + 1) % 5] & row[(x + 2) % 5]);
        }
        a[0] ^= rc[r];
    }
}

/* Lanes are little-endian whatever the host is. */
static inline void sha3_absorb_block(uint64_t a[25], const uint8_t *p) {
    for (unsigned i = 0; i < SHA3_256_RATE / 8; i++) {
        uint64_t v = 0;
        for (unsigned b = 0; b < 8; b++) v |= (uint64_t)p[i * 8 + b] << (8 * b);
        a[i] ^= v;
    }
    keccak_f1600(a);
}

static inline void sha3_256_init(sha3_256_t *h) { memset(h, 0, sizeof(*h)); }

static inline void sha3_256_update(sha3_256_t *h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        size_t take = SHA3_256_RATE - h->n;
        if (take > len) take = len;
        memcpy(h->buf + h->n, p, take);
        h->n += (uint32_t)take;
        p += take;
        len -= take;
        if (h->n == SHA3_256_RATE) {
            sha3_absorb_block(h->a, h->buf);
            h->n = 0;
        }
    }
}

static inline void sha3_256_final(sha3_256_t *h, uint8_t out[SHA3_256_BYTES]) {
    memset(h->buf + h->n, 0, SHA3_256_RATE - h->n);
    h->buf[h->n] ^= 0x06;                    /* SHA-3 domain bits + pad10*1 */
    h->buf[SHA3_256_RATE - 1] ^= 0x80;
    sha3_absorb_block(h->a, h->buf);
    for (unsigned i = 0; i < SHA3_256_BYTES; i++) out[i] = (uint8_t)(h->a[i / 8] >> (8 * (i % 8)));
}

static inline void sha3_256(const void *data, size_t len, uint8_t out[SHA3_256_BYTES]) {
    sha3_256_t h;
    sha3_256_init(&h);
    sha3_256_update(&h, data, len);
    sha3_256_final(&h, out);
}

/* PBKDF2-HMAC-SHA3-256 for one output block (dk_len <= 32, all RPCS3 asks for). */
static inline void pbkdf2_sha3_256(const void *pass, size_t pass_len, const void *salt, size_t salt_len,
                                   uint32_t rounds, uint8_t out[SHA3_256_BYTES]) {
    uint8_t key[SHA3_256_RATE] = {0};
    if (pass_len > SHA3_256_RATE) sha3_256(pass, pass_len, key);
    else memcpy(key, pass, pass_len);

    /* The pads fill exactly one block: absorb each once, start every HMAC from it. */
    sha3_256_t inner, outer;
    uint8_t pad[SHA3_256_RATE];
    sha3_256_init(&inner);
    for (unsigned i = 0; i < SHA3_256_RATE; i++) pad[i] = key[i] ^ 0x36;
    sha3_256_update(&inner, pad, sizeof(pad));
    sha3_256_init(&outer);
    for (unsigned i = 0; i < SHA3_256_RATE; i++) pad[i] = key[i] ^ 0x5C;
    sha3_256_update(&outer, pad, sizeof(pad));

    static const uint8_t block_index[4] = { 0, 0, 0, 1 };
    uint8_t u[SHA3_256_BYTES];
    sha3_256_t h = inner;
    sha3_256_update(&h, salt, salt_len);
    sha3_256_update(&h, block_index, sizeof(block_index));
    sha3_256_final(&h, u);
    h = outer;
    sha3_256_update(&h, u, sizeof(u));
    sha3_256_final(&h, u);
    memcpy(out, u, sizeof(u));

    for (uint32_t r = 1; r < rounds; r++) {
        h = inner;
        sha3_256_update(&h, u, sizeof(u));
        sha3_256_final(&h, u);
        h = outer;
        sha3_256_update(&h, u, sizeof(u));
        sha3_256_final(&h, u);
        for (unsigned i = 0; i < SHA3_256_BYTES; i++) out[i] ^= u[i];
    }
}

/* RPCS3's key for a typed password, as the 64 hex characters it sends. */
static inline void rpcn_password_key(const char *typed, char out[RPCN_KEY_HEX_LEN + 1]) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t dk[SHA3_256_BYTES];
    pbkdf2_sha3_256(typed, strlen(typed), RPCN_KEY_SALT, sizeof(RPCN_KEY_SALT) - 1, RPCN_KEY_ROUNDS, dk);
    for (unsigned i = 0; i < SHA3_256_BYTES; i++) {
        out[i * 2]     = hex[dk[i] >> 4];
        out[i * 2 + 1] = hex[dk[i] & 15];
    }
    out[RPCN_KEY_HEX_LEN] = '\0';
}

/* Already a key: 64 uppercase hex characters, as rpcs3.yml stores it. */
static inline bool rpcn_password_is_key(const char *s) {
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'A' && s[n] <= 'F'))) return false;
    return n == RPCN_KEY_HEX_LEN;
}

#endif /* RPCN_PASSWORD_H */
