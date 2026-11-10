/* Screenshot messages over the Woodland SPI link (test builds).
 *
 * Application messages carried unchanged by spilink (no transport change). All integers are
 * little-endian. Every message starts with an 8-byte tag and the capture id:
 *
 *   WLSHOTRQ  host -> panel  id, flags u32                                           16 B
 *   WLSHOTIN  panel -> host  id, status u16, format u16, width u16, height u16,
 *                          stride u32, total u32, chunk u32, chunks u32,
 *                          capture_us u32, fb_rev u32                              44 B
 *   WLSHOTDT  panel -> host  id, index u32, offset u32, len u32, data[len]       24 + len
 *   WLSHOTEN  panel -> host  id, chunks u32, total u32, crc32 u32                    24 B
 *
 * Format 1 = RGB565 little-endian, rows top to bottom. crc32 is IEEE (zlib.crc32) over the
 * whole image. The panel sends IN, then DT index 0.. in order, then EN.
 *
 * Receiver rules (shot_rx): before this capture's IN, other messages (an earlier capture's,
 * echo leftovers) are accepted as bounded leftovers; this capture's DT/EN before its IN is an
 * error. After IN, every message must be this capture's next DT (index, offset, length) or,
 * after the last, its EN (count, size, CRC); anything else is an error. */
#ifndef SHOT_MSG_H
#define SHOT_MSG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOT_TAG_LEN 8
#define SHOT_RQ_LEN 16
#define SHOT_IN_LEN 44
#define SHOT_DT_HDR 24
#define SHOT_EN_LEN 24
#define SHOT_CHUNK 4000           /* one data message per 4 KiB spilink frame */
#define SHOT_FORMAT_RGB565LE 1
#define SHOT_LEFTOVER_MAX (7u * 4096u)  /* what a panel can still hold of an earlier exchange */

enum { SHOT_NONE, SHOT_RQ, SHOT_IN, SHOT_DT, SHOT_EN };
enum { SHOT_ST_OK = 0, SHOT_ST_NO_MEMORY = 1, SHOT_ST_NO_SCREEN = 2 };

uint32_t shot_crc32_update(uint32_t crc, const uint8_t *p, size_t n); /* zlib.crc32(data, crc) */

/* Which message this is (SHOT_NONE: not a screenshot message); *id when it is one. */
int shot_kind(const uint8_t *m, size_t n, uint32_t *id);

size_t shot_build_rq(uint8_t *out, uint32_t id, uint32_t flags);

typedef struct {
  uint32_t id;
  uint16_t status, format, width, height;
  uint32_t stride, total, chunk, chunks, capture_us, fb_rev;
} shot_info;

/* ---- panel side: the messages of one capture, built one at a time ---- */
typedef struct {
  int active;
  shot_info info;
  const uint8_t *img;  /* the snapshot (info.total bytes); NULL when status != OK */
  uint32_t next;       /* 0: IN, 1..chunks: DT index next-1, chunks+1: EN */
  uint32_t crc;
} shot_tx;

/* Starts a capture of `img` (or an error capture: img NULL, status != OK: IN only). */
void shot_tx_start(shot_tx *t, const shot_info *info, const uint8_t *img);
/* The next message into out (cap >= SHOT_DT_HDR + chunk); 0 when the capture is complete. */
size_t shot_tx_peek(shot_tx *t, uint8_t *out, size_t cap);
/* The peeked message was queued: move on (call only after a successful send). */
void shot_tx_commit(shot_tx *t);

/* ---- host side ---- */
enum { SHOT_RX_MORE = 0, SHOT_RX_DONE = 1, SHOT_RX_ERROR = -1 };
typedef struct {
  uint32_t id;
  uint8_t *buf;
  size_t cap;
  int state;  /* 0 waiting for IN, 1 data, 2 done, 3 failed */
  shot_info info;
  uint32_t next, crc, panel_crc;
  uint32_t leftovers, leftover_bytes, data_msgs;
  uint32_t lo_id, lo_index;  /* the leftover data stream being followed */
  int lo_have;
  const char *error;
  char detail[96];
} shot_rx;

void shot_rx_init(shot_rx *r, uint32_t id, uint8_t *buf, size_t cap);
int shot_rx_feed(shot_rx *r, const uint8_t *m, size_t n);

#ifdef __cplusplus
}
#endif
#endif
