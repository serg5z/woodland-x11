/* Authenticated firmware update over the trusted LAN, protocol WLOTA2 (block-acknowledged),
 * portable C99: the engine shared by the ESP32 receiver and the host simulator. No I/O: the
 * transport feeds received bytes, sends what the engine returns, and calls x11_ota_step()
 * every loop pass; slots and flash are platform ops.
 *
 * Why blocks: flash erase/program disables the caches, and with
 * Wi-Fi/lwIP buffers allowed in PSRAM, data arriving during a flash operation can be lost,
 * collapsing TCP throughput (0.5 KB/s seen with WLOTA1). In WLOTA2 the client sends one block
 * and waits; the panel receives it completely into RAM, commits it one 4 KiB sector per loop
 * pass (erase, then program; touch and the LCD keep running), and only then acknowledges it.
 * No image data is in flight while flash is busy.
 *
 * Authentication and integrity (as in WLOTA1):
 *   - 32-byte device key; HELLO nonce; REQUEST MAC = HMAC(key, server_nonce || request[0:160]);
 *   - every message from the panel (REPLY and ACK) carries HMAC(key, client_nonce ||
 *     server_nonce || message[0:64]) and its index in the session (byte 63), so replies and
 *     ACKs are bound to the session, to their position, and (ACK) to the block sequence, the
 *     committed offset and the status;
 *   - lockout after 5 failures/min; the image must match the authenticated SHA-256, be a valid
 *     ESP32-S3 app with the same IDF version, carry the receiver marker, fit the inactive slot
 *     and pass the platform's validation before the boot slot is switched.
 *
 * Wire format (little-endian, fixed sizes). Every magic says "2": a WLOTA1 client stops at the
 * HELLO ("not a WLOTA1 endpoint"), a WLOTA1 request is refused as BAD_REQUEST, and a WLOTA2
 * client refuses a WLOTA1 panel.
 *   HELLO   64:  "WLOTA2\r\n", u16 version 2, u16 flags (bit 0: inactive slot exists),
 *                u32 inactive slot size, nonce[16], u8 running slot, 3 zero,
 *                u32 default block size, u32 largest block size, zero
 *   REQUEST 192: "WLOTA2RQ", u8 command, 3 zero, client nonce[16], u32 image size,
 *                u32 block size, sha256[32], marker[32], idf version[32], build id[28], hmac[32]
 *   REPLY   96:  "WLOTA2RS", u32 status, u32 detail, text[47], u8 index, hmac[32]
 *   BLOCK   16 + n: "WLB2", u32 sequence (0, 1, ...), u32 offset (= committed so far),
 *                u32 n (= min(block size, image size - offset)), then n image bytes
 *   ACK     96:  "WLOTA2AK", u32 status, u32 sequence, u32 committed offset, u32 receive us,
 *                u32 wait us, u32 erase us, u32 program us, zero, u8 index, hmac[32]
 *   UPLOAD: REQUEST, REPLY(OK, detail = block size), then for each block: BLOCK, ACK; after
 *   the last ACK the final REPLY (hash, marker, validation, boot slot). Any error ends the
 *   session with a REPLY (or an ACK with that status) and never selects the new slot. */
#ifndef X11_OTA_H
#define X11_OTA_H

#include <stddef.h>
#include <stdint.h>

#include "x11_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define X11_OTA_PORT 5232
#define X11_OTA_VERSION 2
#define X11_OTA_KEY_LEN 32
#define X11_OTA_MARKER "WLX11-OTA-RECEIVER-1"  /* firmware that contains this string can be updated again */
#define X11_OTA_HELLO_LEN 64
#define X11_OTA_REQUEST_LEN 192
#define X11_OTA_REPLY_LEN 96
#define X11_OTA_BLOCK_HEADER_LEN 16
#define X11_OTA_SECTOR 4096
#ifndef X11_OTA_MAX_BLOCK
#define X11_OTA_MAX_BLOCK 16384        /* largest block accepted (4096 and 16384; 32768 if raised) */
#endif
#define X11_OTA_DEFAULT_BLOCK 16384
#define X11_OTA_REQUEST_TIMEOUT_MS 10000
#define X11_OTA_DATA_TIMEOUT_MS 20000   /* silence inside a block, or before the next block */
#define X11_OTA_FAIL_LIMIT 5
#define X11_OTA_FAIL_WINDOW_MS 60000
#define X11_OTA_LOCKOUT_MS 60000

enum { X11_OTA_CMD_UPLOAD = 1, X11_OTA_CMD_STATUS = 2, X11_OTA_CMD_BOOT_OTHER = 3, X11_OTA_CMD_REBOOT = 4 };

enum {
  X11_OTA_OK = 0,
  X11_OTA_AUTH_FAILED = 1,
  X11_OTA_LOCKED_OUT = 2,
  X11_OTA_NO_SLOT = 3,
  X11_OTA_TOO_LARGE = 4,
  X11_OTA_INCOMPATIBLE = 5,     /* chip, app descriptor or IDF version */
  X11_OTA_BAD_REQUEST = 6,      /* also: malformed, duplicate or out-of-order block, data before the ACK */
  X11_OTA_BUSY = 7,
  X11_OTA_FLASH_ERROR = 8,
  X11_OTA_HASH_MISMATCH = 9,
  X11_OTA_IMAGE_INVALID = 10,
  X11_OTA_NOT_OTA_CAPABLE = 11,
  X11_OTA_TIMEOUT = 12,
  X11_OTA_NOT_POSSIBLE = 13
};

typedef struct {
  int has_inactive;
  uint32_t inactive_size;
  uint8_t running;
  char text[48];
} x11_ota_slots;

/* Platform: slots and flash. All return 0 on success. */
typedef struct {
  void (*slots)(void *ctx, x11_ota_slots *out);
  int (*begin)(void *ctx, uint32_t size);        /* the inactive slot, never the running one; erases nothing */
  /* One sector (or the image's last partial sector) at a 4 KiB-aligned offset: erase it, then
   * program it; report both times. Called once per sector, in order. */
  int (*write_sector)(void *ctx, uint32_t offset, const uint8_t *data, size_t n, uint32_t *erase_us, uint32_t *program_us);
  int (*finish)(void *ctx, uint32_t size);        /* validates the written image */
  void (*abort)(void *ctx);
  int (*activate)(void *ctx);
  int (*boot_other)(void *ctx);
  void (*reboot)(void *ctx);
  void (*random)(void *ctx, uint8_t *out, size_t n);
  uint32_t (*now_ms)(void *ctx);
  uint64_t (*now_us)(void *ctx);                  /* timing (NULL: now_ms * 1000) */
  const char *(*running_idf)(void *ctx);
  uint8_t *block_buf;                             /* X11_OTA_MAX_BLOCK bytes (PSRAM on the ESP32) */
  void *ctx;
} x11_ota_ops;

/* Upload statistics: the current/last upload (reset at each UPLOAD) and totals. */
typedef struct {
  uint32_t size, committed, blocks, block_size;
  uint32_t started_ms, elapsed_ms;
  uint64_t recv_us, wait_us, erase_us, program_us;  /* sums for the upload */
  uint32_t recv_us_max, wait_us_max, erase_us_max, program_us_max;
  uint32_t sectors;
  uint32_t send_stalls, send_stall_us_max;           /* filled by the transport (TCP backpressure) */
} x11_ota_upload_stats;

typedef struct {
  uint8_t key[X11_OTA_KEY_LEN];
  uint32_t fail_times[X11_OTA_FAIL_LIMIT];
  unsigned fails;
  uint32_t locked_until;
  int locked;
  uint32_t sessions, auth_failures, lockouts, uploads_ok, uploads_failed, aborted, protocol_errors;
  uint32_t last_status;
  x11_ota_upload_stats up;
} x11_ota_endpoint;

enum { X11_OTA_S_REQUEST, X11_OTA_S_HEADER, X11_OTA_S_BLOCK, X11_OTA_S_COMMIT, X11_OTA_S_DONE };

typedef struct {
  x11_ota_endpoint *ep;
  const x11_ota_ops *ops;
  int state;
  uint8_t server_nonce[16], client_nonce[16];
  uint8_t req[X11_OTA_REQUEST_LEN];
  size_t have;
  int authed;
  uint32_t size, block_size, committed, seq;
  uint8_t hdr[X11_OTA_BLOCK_HEADER_LEN];
  size_t hdr_have;
  uint32_t blen, bhave, bcommitted;     /* current block: length, bytes received, bytes committed */
  uint32_t last_ms;
  uint64_t ack_sent_us, first_byte_us, block_done_us;
  uint32_t b_erase_us, b_program_us;
  uint8_t want_sha[32];
  char marker[32], idf[32];
  x11_sha256_ctx sha;
  size_t marker_q;
  int marker_found, head_checked;
  int writing;
  int reboot_after;
  uint32_t status;
  uint8_t replies;
} x11_ota_session;

void x11_ota_endpoint_init(x11_ota_endpoint *ep, const uint8_t key[X11_OTA_KEY_LEN]);

/* New connection: writes the HELLO to out; returns its length. */
size_t x11_ota_open(x11_ota_session *s, x11_ota_endpoint *ep, const x11_ota_ops *ops, uint8_t *out, size_t cap);

/* Bytes from the client. Consumes what belongs to the current message (*consumed); while a
 * block is being committed it consumes nothing (the transport keeps the bytes) - and a client
 * that sends before the ACK is a protocol error. Returns reply bytes in out (cap >= 2 * 96). */
size_t x11_ota_input(x11_ota_session *s, const uint8_t *in, size_t n, size_t *consumed, uint8_t *out, size_t cap);

/* Every loop pass: commits at most one sector of a received block (then ACKs the block, or
 * after the last block finishes the image), and handles timeouts. Returns reply bytes. */
size_t x11_ota_step(x11_ota_session *s, uint8_t *out, size_t cap);

static inline int x11_ota_done(const x11_ota_session *s) { return s->state == X11_OTA_S_DONE; }
/* The engine has nothing to read right now (it is committing): do not feed input. */
static inline int x11_ota_busy(const x11_ota_session *s) { return s->state == X11_OTA_S_COMMIT; }

void x11_ota_closed(x11_ota_session *s);

/* The transport loop shared by the ESP32 and the host simulator (one call per loop pass), so
 * the code that reads the socket is the code the host tests exercise. Reads at most
 * reads_per_pass times buf_len bytes - never while a block is being committed - feeds them to
 * the engine, sends its replies and ACKs (timing any send that stalls: TCP backpressure),
 * then runs x11_ota_step. Returns 1 while the session goes on; 0 when it is over (the client
 * closed, a socket error, or the engine finished): the caller then shuts the socket down,
 * closes it and calls x11_ota_closed. */
typedef struct {
  long (*recv)(void *ctx, uint8_t *buf, size_t n);         /* >0 bytes, 0 closed, -1 nothing now, -2 error */
  long (*send)(void *ctx, const uint8_t *buf, size_t n);   /* bytes sent (may wait briefly), <0 error */
  void *ctx;
  uint8_t *buf;                                            /* receive buffer */
  size_t buf_len;                                          /* bytes per read (<= its size) */
  int reads_per_pass;
} x11_ota_io;
int x11_ota_pump(x11_ota_session *s, x11_ota_io *io);

const char *x11_ota_status_name(uint32_t status);

/* "ota.up.*" lines for diagnostics (no key). */
size_t x11_ota_stats_text(const x11_ota_endpoint *ep, char *out, size_t cap);

int x11_ota_image_check(const uint8_t *img, size_t len);

/* ---- trial health (the decision behind esp_ota_mark_app_valid / _invalid_rollback) ----------
 * Called once per loop pass while the running image is PENDING_VERIFY. `ok`: LCD, X11 core,
 * Wi-Fi and both listeners work now. *healthy_since (0: not healthy) is kept by the caller.
 * CONFIRM after X11_OTA_HEALTHY_MS of continuous health; ROLLBACK once uptime passes
 * X11_OTA_HEALTH_DEADLINE_MS unconfirmed (only when a previous image can take over).
 * no_confirm (the M4 rollback test build, X11_OTA_TEST_NO_CONFIRM): never CONFIRM; the
 * deadline still applies, so such an image always leaves the trial by itself. */
#define X11_OTA_HEALTHY_MS 60000u
#define X11_OTA_HEALTH_DEADLINE_MS 180000u
enum { X11_OTA_TRIAL_WAIT = 0, X11_OTA_TRIAL_CONFIRM = 1, X11_OTA_TRIAL_ROLLBACK = 2 };
int x11_ota_trial_step(int ok, uint32_t now_ms, uint32_t *healthy_since, int rollback_possible, int no_confirm);

#ifdef __cplusplus
}
#endif
#endif
