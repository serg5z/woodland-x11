/* X11 connection setup (X Window System Protocol, X Version 11, Release 6.x, section 8
 * "Connection Setup"). Portable C99: no allocation, no I/O, no OS calls, so the same code
 * runs in the host test server and, later, on the ESP32-S3 behind lwIP sockets.
 *
 * Milestone 0 scope: the client's setup request is parsed in either byte order and
 * answered with Success (one 800x480 screen), or Failed with a reason. Authentication:
 * MIT-MAGIC-COOKIE-1 is checked against a configured cookie; a server without a cookie
 * accepts only an empty authorization (loopback testing); every other protocol name, or
 * any credentials the server cannot check, is rejected - never silently accepted.
 */
#ifndef X11_SETUP_H
#define X11_SETUP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X11_PROTOCOL_MAJOR 11
#define X11_PROTOCOL_MINOR 0
#define X11_SETUP_HEADER_LEN 12   /* fixed part of the client's setup request */
#define X11_MAX_AUTH_NAME 64      /* larger auth names / data are refused without reading them */
#define X11_MAX_AUTH_DATA 256
#define X11_COOKIE_LEN 16         /* MIT-MAGIC-COOKIE-1 */

/* Server-owned resource IDs: all below X11_CLIENT_ID_UNIT, so they never collide with an
 * ID a client allocates in its own range (base = (index + 1) * X11_CLIENT_ID_UNIT). */
#define X11_ID_ROOT_WINDOW 0x00000026u
#define X11_ID_DEFAULT_COLORMAP 0x00000020u
#define X11_ID_VISUAL_TRUECOLOR16 0x00000021u
#define X11_CLIENT_ID_UNIT 0x00200000u  /* 2^21 */
#define X11_CLIENT_ID_MASK 0x001FFFFFu  /* 21 contiguous bits (the protocol requires >= 18) */
#define X11_MAX_CLIENTS 255     /* ID ranges (index + 1) * 2^21 stay below 2^29 for index 0..254 */

typedef struct {
  uint16_t width_px, height_px;
  uint16_t width_mm, height_mm;
  uint32_t release;            /* vendor release number */
  const char *vendor;
  uint16_t max_request_units;  /* maximum-request-length in 4-byte units, >= 4096 */
  const uint8_t *cookie;       /* MIT-MAGIC-COOKIE-1, X11_COOKIE_LEN bytes; NULL: none */
  /* Limits (used by x11_server; 0 = the compile-time maximum / no per-client quota). */
  uint16_t max_clients;        /* authenticated clients at once, <= X11_SERVER_CLIENTS */
  uint16_t max_resources;      /* resource table entries (all clients), <= X11_MAX_RESOURCES_LIMIT */
  uint16_t max_resources_per_client;  /* quota: BadAlloc beyond it */
  uint8_t reset_on_last_close;        /* server reset when the last client leaves (protocol,
                                         "Connection Close"); 0: never, like X.Org's -noreset.
                                         Off by default: the panel's tools leave requests in
                                         root properties for the firmware (_WOODLAND_PCLK_REQUEST) */
} x11_server_config;

/* Woodland CrowPanel 4.3": 800x480 RGB565 panel, active area about 95 x 54 mm. */
void x11_default_config(x11_server_config *cfg);

typedef enum {
  X11_SETUP_NEED_MORE = 0,  /* not enough bytes yet: read until `needed` bytes are buffered */
  X11_SETUP_SUCCESS = 1,    /* reply holds Setup Success; `consumed` bytes of input used */
  X11_SETUP_FAILED = 2      /* reply holds Setup Failed; close after sending it */
} x11_setup_status;

typedef struct {
  x11_setup_status status;
  size_t needed;            /* NEED_MORE: total bytes required */
  size_t consumed;          /* SUCCESS/FAILED: bytes of the request used */
  uint8_t msb_first;        /* client byte order: 1 'B' (MSB first), 0 'l' (LSB first) */
  uint16_t client_major, client_minor;
  uint32_t resource_id_base, resource_id_mask;
  const char *reason;       /* FAILED: the reason sent to the client */
} x11_setup_result;

/* Handles the buffered start of a connection. Returns the reply length written to `out`
 * (0 when more input is needed or `out_cap` is too small; check result->status).
 * client_index (0 .. X11_MAX_CLIENTS-1) selects the client's resource-ID range. */
size_t x11_setup_handle(const x11_server_config *cfg, const uint8_t *in, size_t in_len, unsigned client_index,
                        uint8_t *out, size_t out_cap, x11_setup_result *result);

/* Setup Failed with `reason`, for a connection the server cannot take (no free slot, memory
 * budget exhausted), before any authentication. Needs only the byte-order byte (in_len >= 1)
 * so it can answer a client that has sent little; returns 0 when in_len is 0, the byte is
 * not 'B'/'l', or out_cap is too small. The reply carries no server information. */
size_t x11_setup_refuse(const uint8_t *in, size_t in_len, const char *reason, uint8_t *out, size_t out_cap);

/* Length of the Success reply for cfg (for buffer sizing): always a multiple of 4. */
size_t x11_setup_success_len(const x11_server_config *cfg);

#ifdef __cplusplus
}
#endif
#endif
