/* WLOTA2 protocol engine; see x11_ota.h. */
#include "x11_ota.h"

#include <stdio.h>
#include <string.h>

#define HEAD_LEN 512                 /* image header + first segment header + app descriptor (256) */
#define DESC_OFF 32
#define DESC_MAGIC 0xABCD5432u
#define DESC_IDF_OFF (DESC_OFF + 112)
#define ESP32S3_CHIP_ID 9

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

const char *x11_ota_status_name(uint32_t s) {
  static const char *const N[] = {"ok", "authentication failed", "locked out", "no inactive slot", "image too large",
                                  "incompatible image", "bad request", "busy", "flash error", "hash mismatch",
                                  "invalid image", "image cannot receive OTA (marker missing)", "timeout",
                                  "not possible"};
  return s < sizeof(N) / sizeof(N[0]) ? N[s] : "unknown";
}

/* ---- image format ---------------------------------------------------------------------- */

int x11_ota_image_check(const uint8_t *img, size_t len) {
  if (len < HEAD_LEN || img[0] != 0xE9) return -1;
  unsigned nseg = img[1];
  if (nseg < 1 || nseg > 16 || rd16(img + 12) != ESP32S3_CHIP_ID) return -1;
  if (rd32(img + DESC_OFF) != DESC_MAGIC) return -1;
  size_t off = 24;
  uint8_t ck = 0xEF;
  for (unsigned i = 0; i < nseg; ++i) {
    if (off + 8 > len) return -1;
    size_t n = rd32(img + off + 4);
    off += 8;
    if (n > len - off) return -1;
    for (size_t k = 0; k < n; ++k) ck ^= img[off + k];
    off += n;
  }
  size_t ck_at = ((off + 1 + 15) & ~(size_t)15) - 1;
  if (ck_at >= len || img[ck_at] != ck) return -1;
  size_t end = ck_at + 1;
  if (img[23]) {
    uint8_t h[32];
    if (end + 32 > len) return -1;
    x11_sha256(img, end, h);
    if (memcmp(h, img + end, 32)) return -1;
    end += 32;
  }
  return end == len ? 0 : -1;
}

/* ---- endpoint ---------------------------------------------------------------------------- */

void x11_ota_endpoint_init(x11_ota_endpoint *ep, const uint8_t key[X11_OTA_KEY_LEN]) {
  memset(ep, 0, sizeof(*ep));
  memcpy(ep->key, key, X11_OTA_KEY_LEN);
}

static uint32_t now(const x11_ota_session *s) { return s->ops->now_ms ? s->ops->now_ms(s->ops->ctx) : 0; }
static uint64_t now_us(const x11_ota_session *s) {
  return s->ops->now_us ? s->ops->now_us(s->ops->ctx) : (uint64_t)now(s) * 1000u;
}

static int locked(x11_ota_session *s) {
  x11_ota_endpoint *ep = s->ep;
  if (ep->locked && (int32_t)(now(s) - ep->locked_until) < 0) return 1;
  ep->locked = 0;
  return 0;
}

static void auth_failed(x11_ota_session *s) {
  x11_ota_endpoint *ep = s->ep;
  uint32_t t = now(s);
  ++ep->auth_failures;
  ep->fail_times[ep->fails % X11_OTA_FAIL_LIMIT] = t;
  ++ep->fails;
  if (ep->fails >= X11_OTA_FAIL_LIMIT && t - ep->fail_times[ep->fails % X11_OTA_FAIL_LIMIT] <= X11_OTA_FAIL_WINDOW_MS) {
    ep->locked = 1;
    ep->locked_until = t + X11_OTA_LOCKOUT_MS;
    ++ep->lockouts;
    ep->fails = 0;
  }
}

/* ---- messages from the panel: REPLY and ACK, both MAC'd with their index ---------------- */

static size_t message(x11_ota_session *s, const char *magic, uint8_t *out, size_t cap) {
  if (cap < X11_OTA_REPLY_LEN) return 0;
  memcpy(out, magic, 8);
  out[63] = s->replies++;
  if (s->authed) x11_hmac_sha256(s->ep->key, X11_OTA_KEY_LEN, s->client_nonce, 16, s->server_nonce, 16, out, 64, out + 64);
  else memset(out + 64, 0, 32);
  return X11_OTA_REPLY_LEN;
}

static size_t reply(x11_ota_session *s, uint32_t status, uint32_t detail, const char *text, uint8_t *out, size_t cap) {
  if (cap < X11_OTA_REPLY_LEN) return 0;
  memset(out, 0, X11_OTA_REPLY_LEN);
  wr32(out + 8, status);
  wr32(out + 12, detail);
  if (text) strncpy((char *)out + 16, text, 46);
  s->status = status;
  s->ep->last_status = status;
  return message(s, "WLOTA2RS", out, cap);
}

static size_t ack(x11_ota_session *s, uint32_t status, uint8_t *out, size_t cap) {
  if (cap < X11_OTA_REPLY_LEN) return 0;
  memset(out, 0, X11_OTA_REPLY_LEN);
  wr32(out + 8, status);
  wr32(out + 12, s->seq);           /* the block just committed */
  wr32(out + 16, s->committed);     /* bytes durably written so far */
  wr32(out + 20, (uint32_t)(s->block_done_us - s->first_byte_us));
  wr32(out + 24, (uint32_t)(s->first_byte_us - s->ack_sent_us));
  wr32(out + 28, s->b_erase_us);
  wr32(out + 32, s->b_program_us);
  return message(s, "WLOTA2AK", out, cap);
}

static void upload_done(x11_ota_session *s) {
  s->ep->up.elapsed_ms = now(s) - s->ep->up.started_ms;
}

static size_t finish(x11_ota_session *s, uint32_t status, uint32_t detail, const char *text, uint8_t *out, size_t cap) {
  if (s->writing) {
    s->ops->abort(s->ops->ctx);
    s->writing = 0;
    ++s->ep->uploads_failed;
    upload_done(s);
  }
  if (status == X11_OTA_BAD_REQUEST && s->authed) ++s->ep->protocol_errors;
  s->state = X11_OTA_S_DONE;
  return reply(s, status, detail, text ? text : x11_ota_status_name(status), out, cap);
}

/* ---- session ----------------------------------------------------------------------------- */

size_t x11_ota_open(x11_ota_session *s, x11_ota_endpoint *ep, const x11_ota_ops *ops, uint8_t *out, size_t cap) {
  memset(s, 0, sizeof(*s));
  s->ep = ep, s->ops = ops;
  s->state = X11_OTA_S_REQUEST;
  ops->random(ops->ctx, s->server_nonce, sizeof(s->server_nonce));
  s->last_ms = now(s);
  ++ep->sessions;
  if (cap < X11_OTA_HELLO_LEN) return 0;
  x11_ota_slots sl;
  memset(&sl, 0, sizeof(sl));
  ops->slots(ops->ctx, &sl);
  memset(out, 0, X11_OTA_HELLO_LEN);
  memcpy(out, "WLOTA2\r\n", 8);
  out[8] = X11_OTA_VERSION;
  out[10] = (uint8_t)(sl.has_inactive ? 1 : 0);
  wr32(out + 12, sl.has_inactive ? sl.inactive_size : 0);
  memcpy(out + 16, s->server_nonce, 16);
  out[32] = sl.running;
  wr32(out + 36, X11_OTA_DEFAULT_BLOCK);
  wr32(out + 40, X11_OTA_MAX_BLOCK);
  return X11_OTA_HELLO_LEN;
}

static int block_size_ok(uint32_t b) {
  return b >= X11_OTA_SECTOR && b <= X11_OTA_MAX_BLOCK && b % X11_OTA_SECTOR == 0 &&
         (b == 4096 || b == 16384 || b == 32768);
}

static size_t request(x11_ota_session *s, uint8_t *out, size_t cap) {
  const uint8_t *q = s->req;
  if (!memcmp(q, "WLOTA1RQ", 8))
    return finish(s, X11_OTA_BAD_REQUEST, 1, "protocol 1 client: use a WLOTA2 client", out, cap);
  if (memcmp(q, "WLOTA2RQ", 8)) return finish(s, X11_OTA_BAD_REQUEST, 0, 0, out, cap);
  if (locked(s)) return finish(s, X11_OTA_LOCKED_OUT, 0, 0, out, cap);
  uint8_t mac[32];
  x11_hmac_sha256(s->ep->key, X11_OTA_KEY_LEN, s->server_nonce, 16, q, 160, 0, 0, mac);
  if (!x11_ct_equal(mac, q + 160, 32)) {
    auth_failed(s);
    return finish(s, X11_OTA_AUTH_FAILED, 0, 0, out, cap);
  }
  s->authed = 1;
  s->ep->fails = 0;
  memcpy(s->client_nonce, q + 12, 16);
  const x11_ota_ops *o = s->ops;
  x11_ota_slots sl;
  memset(&sl, 0, sizeof(sl));
  o->slots(o->ctx, &sl);
  switch (q[8]) {
    case X11_OTA_CMD_STATUS:
      return finish(s, X11_OTA_OK, sl.running, sl.text, out, cap);
    case X11_OTA_CMD_REBOOT:
      s->reboot_after = 1;
      return finish(s, X11_OTA_OK, 0, "rebooting", out, cap);
    case X11_OTA_CMD_BOOT_OTHER:
      if (!sl.has_inactive || o->boot_other(o->ctx) != 0) return finish(s, X11_OTA_NOT_POSSIBLE, 0, 0, out, cap);
      s->reboot_after = 1;
      return finish(s, X11_OTA_OK, 0, "the other slot boots next; rebooting", out, cap);
    case X11_OTA_CMD_UPLOAD:
      break;
    default:
      return finish(s, X11_OTA_BAD_REQUEST, q[8], 0, out, cap);
  }
  s->size = rd32(q + 28);
  s->block_size = rd32(q + 32);
  memcpy(s->want_sha, q + 36, 32);
  memcpy(s->marker, q + 68, 32);
  memcpy(s->idf, q + 100, 32);
  s->marker[31] = s->idf[31] = 0;
  if (!block_size_ok(s->block_size)) return finish(s, X11_OTA_BAD_REQUEST, X11_OTA_MAX_BLOCK, "unsupported block size", out, cap);
  if (strcmp(s->marker, X11_OTA_MARKER)) return finish(s, X11_OTA_NOT_OTA_CAPABLE, 0, 0, out, cap);
  const char *idf = o->running_idf ? o->running_idf(o->ctx) : "";
  if (strcmp(s->idf, idf)) return finish(s, X11_OTA_INCOMPATIBLE, 0, "IDF version differs from the running firmware", out, cap);
  if (!sl.has_inactive) return finish(s, X11_OTA_NO_SLOT, 0, 0, out, cap);
  if (s->size < HEAD_LEN) return finish(s, X11_OTA_IMAGE_INVALID, s->size, 0, out, cap);
  if (s->size > sl.inactive_size) return finish(s, X11_OTA_TOO_LARGE, sl.inactive_size, 0, out, cap);
  if (!o->block_buf) return finish(s, X11_OTA_BUSY, 0, "no block buffer", out, cap);
  if (o->begin(o->ctx, s->size) != 0) return finish(s, X11_OTA_FLASH_ERROR, 0, "cannot open the inactive slot", out, cap);
  s->writing = 1;
  s->state = X11_OTA_S_HEADER;
  x11_sha256_init(&s->sha);
  memset(&s->ep->up, 0, sizeof(s->ep->up));
  s->ep->up.size = s->size;
  s->ep->up.block_size = s->block_size;
  s->ep->up.started_ms = now(s);
  s->last_ms = now(s);
  s->ack_sent_us = now_us(s);
  return reply(s, X11_OTA_OK, s->block_size, "send blocks", out, cap);
}

static void marker_scan(x11_ota_session *s, const uint8_t *p, size_t n) {
  static const char M[] = X11_OTA_MARKER;
  const size_t m = sizeof(M) - 1;
  if (s->marker_found) return;
  size_t pi[sizeof(M)];
  pi[0] = 0;
  for (size_t i = 1; i < m; ++i) {
    size_t k = pi[i - 1];
    while (k > 0 && M[i] != M[k]) k = pi[k - 1];
    if (M[i] == M[k]) ++k;
    pi[i] = k;
  }
  size_t q = s->marker_q;
  for (size_t i = 0; i < n; ++i) {
    while (q > 0 && p[i] != (uint8_t)M[q]) q = pi[q - 1];
    if (p[i] == (uint8_t)M[q]) ++q;
    if (q == m) {
      s->marker_found = 1;
      return;
    }
  }
  s->marker_q = q;
}

static int head_check(x11_ota_session *s, const uint8_t *h) {
  if (h[0] != 0xE9 || h[1] < 1 || h[1] > 16) return X11_OTA_IMAGE_INVALID;
  if (rd16(h + 12) != ESP32S3_CHIP_ID) return X11_OTA_INCOMPATIBLE;
  if (rd32(h + DESC_OFF) != DESC_MAGIC) return X11_OTA_IMAGE_INVALID;
  char idf[33];
  memcpy(idf, h + DESC_IDF_OFF, 32);
  idf[32] = 0;
  return strcmp(idf, s->idf) ? X11_OTA_INCOMPATIBLE : X11_OTA_OK;
}

size_t x11_ota_input(x11_ota_session *s, const uint8_t *in, size_t n, size_t *consumed, uint8_t *out, size_t cap) {
  *consumed = 0;
  if (!n) return 0;
  switch (s->state) {
    case X11_OTA_S_DONE:
      *consumed = n;  /* ignored */
      return 0;
    case X11_OTA_S_REQUEST: {
      size_t k = X11_OTA_REQUEST_LEN - s->have < n ? X11_OTA_REQUEST_LEN - s->have : n;
      memcpy(s->req + s->have, in, k);
      s->have += k;
      *consumed = k;
      s->last_ms = now(s);
      return s->have < X11_OTA_REQUEST_LEN ? 0 : request(s, out, cap);
    }
    case X11_OTA_S_COMMIT:  /* the client must wait for the ACK */
      return finish(s, X11_OTA_BAD_REQUEST, s->seq, "data before the ACK", out, cap);
    case X11_OTA_S_HEADER: {
      if (!s->hdr_have) s->first_byte_us = now_us(s);
      size_t k = X11_OTA_BLOCK_HEADER_LEN - s->hdr_have < n ? X11_OTA_BLOCK_HEADER_LEN - s->hdr_have : n;
      memcpy(s->hdr + s->hdr_have, in, k);
      s->hdr_have += k;
      *consumed = k;
      s->last_ms = now(s);
      if (s->hdr_have < X11_OTA_BLOCK_HEADER_LEN) return 0;
      uint32_t seq = rd32(s->hdr + 4), off = rd32(s->hdr + 8), len = rd32(s->hdr + 12);
      uint32_t want = s->size - s->committed < s->block_size ? s->size - s->committed : s->block_size;
      if (memcmp(s->hdr, "WLB2", 4)) return finish(s, X11_OTA_BAD_REQUEST, s->seq, "malformed block header", out, cap);
      if (seq != s->seq) return finish(s, X11_OTA_BAD_REQUEST, seq, seq < s->seq ? "duplicate block" : "block out of order", out, cap);
      if (off != s->committed) return finish(s, X11_OTA_BAD_REQUEST, off, "block offset differs from committed", out, cap);
      if (len != want) return finish(s, X11_OTA_BAD_REQUEST, len, "wrong block length", out, cap);
      s->blen = len, s->bhave = 0;
      s->state = X11_OTA_S_BLOCK;
      return 0;
    }
    case X11_OTA_S_BLOCK: {
      size_t k = s->blen - s->bhave < n ? s->blen - s->bhave : n;
      memcpy(s->ops->block_buf + s->bhave, in, k);
      s->bhave += (uint32_t)k;
      *consumed = k;
      s->last_ms = now(s);
      if (s->bhave < s->blen) return 0;
      s->block_done_us = now_us(s);
      if (k < n) return finish(s, X11_OTA_BAD_REQUEST, s->seq, "data before the ACK", out, cap);
      x11_sha256_update(&s->sha, s->ops->block_buf, s->blen);
      marker_scan(s, s->ops->block_buf, s->blen);
      s->bcommitted = 0, s->b_erase_us = s->b_program_us = 0;
      s->state = X11_OTA_S_COMMIT;
      return 0;
    }
  }
  return 0;
}

static size_t final_reply(x11_ota_session *s, uint8_t *out, size_t cap) {
  const x11_ota_ops *o = s->ops;
  uint8_t h[32];
  x11_sha256_final(&s->sha, h);
  if (!x11_ct_equal(h, s->want_sha, 32)) return finish(s, X11_OTA_HASH_MISMATCH, 0, 0, out, cap);
  if (!s->marker_found) return finish(s, X11_OTA_NOT_OTA_CAPABLE, 0, 0, out, cap);
  s->writing = 0;
  upload_done(s);
  s->state = X11_OTA_S_DONE;
  if (o->finish(o->ctx, s->size) != 0) {
    ++s->ep->uploads_failed;
    return reply(s, X11_OTA_IMAGE_INVALID, 0, "image validation failed", out, cap);
  }
  if (o->activate(o->ctx) != 0) {
    ++s->ep->uploads_failed;
    return reply(s, X11_OTA_FLASH_ERROR, 0, "could not select the new slot", out, cap);
  }
  ++s->ep->uploads_ok;
  return reply(s, X11_OTA_OK, s->size, "written and selected; REBOOT to start it", out, cap);
}

size_t x11_ota_step(x11_ota_session *s, uint8_t *out, size_t cap) {
  x11_ota_upload_stats *u = &s->ep->up;
  if (s->state == X11_OTA_S_DONE) return 0;
  if (s->state != X11_OTA_S_COMMIT) {  /* timeouts */
    uint32_t limit = s->state == X11_OTA_S_REQUEST ? X11_OTA_REQUEST_TIMEOUT_MS : X11_OTA_DATA_TIMEOUT_MS;
    if (now(s) - s->last_ms < limit) return 0;
    return finish(s, X11_OTA_TIMEOUT, s->committed, 0, out, cap);
  }
  /* one sector per call: erase, then program (no image data is in flight meanwhile) */
  const x11_ota_ops *o = s->ops;
  uint32_t off = s->committed + s->bcommitted;
  uint32_t n = s->blen - s->bcommitted < X11_OTA_SECTOR ? s->blen - s->bcommitted : X11_OTA_SECTOR;
  if (off == 0 && !s->head_checked) {
    int st = head_check(s, o->block_buf);
    if (st != X11_OTA_OK) return finish(s, (uint32_t)st, 0, 0, out, cap);
    s->head_checked = 1;
  }
  uint32_t e = 0, p = 0;
  if (o->write_sector(o->ctx, off, o->block_buf + s->bcommitted, n, &e, &p) != 0)
    return finish(s, X11_OTA_FLASH_ERROR, off, "flash write failed", out, cap);
  s->b_erase_us += e, s->b_program_us += p;
  ++u->sectors;
  if (e > u->erase_us_max) u->erase_us_max = e;
  if (p > u->program_us_max) u->program_us_max = p;
  s->bcommitted += n;
  s->last_ms = now(s);
  if (s->bcommitted < s->blen) return 0;
  /* the block is durably written */
  s->committed += s->blen;
  uint32_t recv = (uint32_t)(s->block_done_us - s->first_byte_us), wait = (uint32_t)(s->first_byte_us - s->ack_sent_us);
  u->committed = s->committed;
  ++u->blocks;
  u->recv_us += recv, u->wait_us += wait, u->erase_us += s->b_erase_us, u->program_us += s->b_program_us;
  if (recv > u->recv_us_max) u->recv_us_max = recv;
  if (wait > u->wait_us_max) u->wait_us_max = wait;
  size_t len = ack(s, X11_OTA_OK, out, cap);
  ++s->seq;
  s->hdr_have = 0;
  s->ack_sent_us = now_us(s);
  if (s->committed < s->size) {
    s->state = X11_OTA_S_HEADER;
    return len;
  }
  return len + final_reply(s, out + len, cap - len);
}

void x11_ota_closed(x11_ota_session *s) {
  if (s->writing) {
    s->ops->abort(s->ops->ctx);
    s->writing = 0;
    ++s->ep->aborted;
    upload_done(s);
  }
  s->state = X11_OTA_S_DONE;
  if (s->reboot_after && s->ops->reboot) {
    s->reboot_after = 0;
    s->ops->reboot(s->ops->ctx);
  }
}

static int send_out(x11_ota_session *s, x11_ota_io *io, const uint8_t *p, size_t n) {
  uint64_t t0 = now_us(s);
  while (n) {
    long w = io->send(io->ctx, p, n);
    if (w <= 0) return -1;
    p += w, n -= (size_t)w;
  }
  uint32_t dt = (uint32_t)(now_us(s) - t0);
  if (dt > 1000) {  /* the send waited: the client is not reading (TCP backpressure) */
    ++s->ep->up.send_stalls;
    if (dt > s->ep->up.send_stall_us_max) s->ep->up.send_stall_us_max = dt;
  }
  return 0;
}

int x11_ota_pump(x11_ota_session *s, x11_ota_io *io) {
  uint8_t out[2 * X11_OTA_REPLY_LEN];
  for (int r = 0; r < io->reads_per_pass && !x11_ota_done(s) && !x11_ota_busy(s); ++r) {
    long got = io->recv(io->ctx, io->buf, io->buf_len);
    if (got == 0 || got < -1) return 0;  /* closed by the client, or a socket error */
    if (got < 0) break;                  /* nothing more right now */
    size_t off = 0;
    while (off < (size_t)got && !x11_ota_done(s)) {
      size_t used = 0, n = x11_ota_input(s, io->buf + off, (size_t)got - off, &used, out, sizeof(out));
      off += used;
      if (n && send_out(s, io, out, n) != 0) return 0;
      if (!used && !n) break;
    }
  }
  size_t n = x11_ota_step(s, out, sizeof(out));
  if (n && send_out(s, io, out, n) != 0) return 0;
  return !x11_ota_done(s);
}

size_t x11_ota_stats_text(const x11_ota_endpoint *ep, char *out, size_t cap) {
  const x11_ota_upload_stats *u = &ep->up;
  uint32_t rate = u->elapsed_ms ? (uint32_t)((uint64_t)u->committed * 1000u / u->elapsed_ms) : 0;
  int n = snprintf(out, cap,
                   "ota.up.size=%lu\nota.up.committed=%lu\nota.up.block_size=%lu\nota.up.blocks=%lu\n"
                   "ota.up.sectors=%lu\nota.up.elapsed_ms=%lu\nota.up.rate_bytes_per_s=%lu\n"
                   "ota.up.recv_ms=%lu\nota.up.wait_ms=%lu\nota.up.erase_ms=%lu\nota.up.program_ms=%lu\n"
                   "ota.up.recv_us_max=%lu\nota.up.wait_us_max=%lu\nota.up.erase_us_max=%lu\nota.up.program_us_max=%lu\n"
                   "ota.up.send_stalls=%lu\nota.up.send_stall_us_max=%lu\nota.protocol_errors=%lu\n",
                   (unsigned long)u->size, (unsigned long)u->committed, (unsigned long)u->block_size,
                   (unsigned long)u->blocks, (unsigned long)u->sectors, (unsigned long)u->elapsed_ms, (unsigned long)rate,
                   (unsigned long)(u->recv_us / 1000), (unsigned long)(u->wait_us / 1000),
                   (unsigned long)(u->erase_us / 1000), (unsigned long)(u->program_us / 1000),
                   (unsigned long)u->recv_us_max, (unsigned long)u->wait_us_max, (unsigned long)u->erase_us_max,
                   (unsigned long)u->program_us_max, (unsigned long)u->send_stalls,
                   (unsigned long)u->send_stall_us_max, (unsigned long)ep->protocol_errors);
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}

/* ---- trial health ------------------------------------------------------------------------- */

int x11_ota_trial_step(int ok, uint32_t now_ms, uint32_t *healthy_since, int rollback_possible, int no_confirm) {
  if (!ok) *healthy_since = 0;
  else if (!*healthy_since) *healthy_since = now_ms ? now_ms : 1;
  if (!no_confirm && ok && now_ms - *healthy_since >= X11_OTA_HEALTHY_MS) return X11_OTA_TRIAL_CONFIRM;
  if (now_ms >= X11_OTA_HEALTH_DEADLINE_MS && rollback_possible) return X11_OTA_TRIAL_ROLLBACK;
  return X11_OTA_TRIAL_WAIT;
}
