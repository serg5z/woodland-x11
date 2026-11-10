/* SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), portable C99. One implementation for the
 * ESP32 OTA receiver and its host simulator, checked against Python's hashlib/hmac. */
#ifndef X11_SHA256_H
#define X11_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t h[8];
  uint64_t len;        /* bytes hashed */
  uint8_t buf[64];
  size_t n;            /* bytes in buf */
} x11_sha256_ctx;

void x11_sha256_init(x11_sha256_ctx *c);
void x11_sha256_update(x11_sha256_ctx *c, const void *data, size_t n);
void x11_sha256_final(x11_sha256_ctx *c, uint8_t out[32]);
void x11_sha256(const void *data, size_t n, uint8_t out[32]);

/* HMAC-SHA256 over the concatenation of up to three parts (NULL/0 for unused). */
void x11_hmac_sha256(const uint8_t *key, size_t key_len, const void *a, size_t an, const void *b, size_t bn,
                     const void *c, size_t cn, uint8_t out[32]);

/* Constant-time equality (the time does not depend on where the inputs differ). */
int x11_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

#ifdef __cplusplus
}
#endif
#endif
