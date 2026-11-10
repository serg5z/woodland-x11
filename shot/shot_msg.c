/* Screenshot messages over the Woodland SPI link: see shot_msg.h. */
#include "shot_msg.h"

#include <stdio.h>
#include <string.h>

static const char TAG_RQ[] = "WLSHOTRQ", TAG_IN[] = "WLSHOTIN", TAG_DT[] = "WLSHOTDT", TAG_EN[] = "WLSHOTEN";

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v), w16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return r16(p) | (uint32_t)r16(p + 2) << 16; }

uint32_t shot_crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
  static uint32_t table[256];
  static int ready = 0;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = 1;
  }
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

int shot_kind(const uint8_t *m, size_t n, uint32_t *id) {
  if (n < SHOT_TAG_LEN + 4 || memcmp(m, "WLSHOT", 6)) return SHOT_NONE;
  int k = !memcmp(m, TAG_RQ, 8) ? SHOT_RQ : !memcmp(m, TAG_IN, 8) ? SHOT_IN : !memcmp(m, TAG_DT, 8) ? SHOT_DT
          : !memcmp(m, TAG_EN, 8) ? SHOT_EN : SHOT_NONE;
  if (k != SHOT_NONE && id) *id = r32(m + 8);
  return k;
}

size_t shot_build_rq(uint8_t *out, uint32_t id, uint32_t flags) {
  memcpy(out, TAG_RQ, 8);
  w32(out + 8, id);
  w32(out + 12, flags);
  return SHOT_RQ_LEN;
}

/* ---- panel side ---- */

void shot_tx_start(shot_tx *t, const shot_info *info, const uint8_t *img) {
  memset(t, 0, sizeof(*t));
  t->active = 1;
  t->info = *info;
  t->img = info->status == SHOT_ST_OK ? img : NULL;
  if (!t->img) t->info.chunks = 0, t->info.total = 0;
}

size_t shot_tx_peek(shot_tx *t, uint8_t *out, size_t cap) {
  if (!t->active) return 0;
  const shot_info *i = &t->info;
  if (t->next == 0) {
    if (cap < SHOT_IN_LEN) return 0;
    memcpy(out, TAG_IN, 8);
    w32(out + 8, i->id);
    w16(out + 12, i->status), w16(out + 14, i->format), w16(out + 16, i->width), w16(out + 18, i->height);
    w32(out + 20, i->stride), w32(out + 24, i->total), w32(out + 28, i->chunk), w32(out + 32, i->chunks);
    w32(out + 36, i->capture_us), w32(out + 40, i->fb_rev);
    return SHOT_IN_LEN;
  }
  if (!t->img) return 0;  /* an error capture: IN only */
  if (t->next <= i->chunks) {
    uint32_t idx = t->next - 1, off = idx * i->chunk, len = i->total - off < i->chunk ? i->total - off : i->chunk;
    if (cap < SHOT_DT_HDR + len) return 0;
    memcpy(out, TAG_DT, 8);
    w32(out + 8, i->id), w32(out + 12, idx), w32(out + 16, off), w32(out + 20, len);
    memcpy(out + SHOT_DT_HDR, t->img + off, len);
    return SHOT_DT_HDR + len;
  }
  if (t->next == i->chunks + 1) {
    if (cap < SHOT_EN_LEN) return 0;
    memcpy(out, TAG_EN, 8);
    w32(out + 8, i->id), w32(out + 12, i->chunks), w32(out + 16, i->total), w32(out + 20, t->crc);
    return SHOT_EN_LEN;
  }
  return 0;
}

void shot_tx_commit(shot_tx *t) {
  if (!t->active) return;
  const shot_info *i = &t->info;
  if (t->next >= 1 && t->next <= i->chunks && t->img) {
    uint32_t off = (t->next - 1) * i->chunk, len = i->total - off < i->chunk ? i->total - off : i->chunk;
    t->crc = shot_crc32_update(t->crc, t->img + off, len);
  }
  ++t->next;
  if (!t->img || t->next > i->chunks + 1) t->active = 0;
}

/* ---- host side ---- */

void shot_rx_init(shot_rx *r, uint32_t id, uint8_t *buf, size_t cap) {
  memset(r, 0, sizeof(*r));
  r->id = id, r->buf = buf, r->cap = cap;
}

static int fail(shot_rx *r, const char *why, const char *fmt, uint32_t a, uint32_t b) {
  r->state = 3;
  r->error = why;
  snprintf(r->detail, sizeof(r->detail), fmt, (unsigned long)a, (unsigned long)b);
  return SHOT_RX_ERROR;
}

int shot_rx_feed(shot_rx *r, const uint8_t *m, size_t n) {
  if (r->state >= 2) return r->state == 2 ? SHOT_RX_DONE : SHOT_RX_ERROR;
  uint32_t id = 0;
  int k = shot_kind(m, n, &id);
  int mine = k != SHOT_NONE && k != SHOT_RQ && id == r->id;
  if (r->state == 0 && !(mine && k == SHOT_IN)) {
    if (mine) return fail(r, "data before this capture's info", "kind %lu, %lu bytes", (uint32_t)k, (uint32_t)n);
    /* a leftover of an earlier exchange: bounded; an earlier capture's chunks in their own order */
    if (r->leftover_bytes + n > SHOT_LEFTOVER_MAX)
      return fail(r, "more leftovers than a panel can hold", "%lu + %lu bytes", r->leftover_bytes, (uint32_t)n);
    if (k == SHOT_DT && n >= SHOT_DT_HDR) {
      uint32_t idx = r32(m + 12);
      if (r->lo_have && id == r->lo_id && idx != r->lo_index + 1)
        return fail(r, "an earlier capture's chunks out of order", "index %lu after %lu", idx, r->lo_index);
      r->lo_have = 1, r->lo_id = id, r->lo_index = idx;
    }
    ++r->leftovers, r->leftover_bytes += (uint32_t)n;
    return SHOT_RX_MORE;
  }
  if (r->state == 0) {  /* this capture's IN */
    if (n != SHOT_IN_LEN) return fail(r, "info: wrong length", "%lu bytes (want %lu)", (uint32_t)n, SHOT_IN_LEN);
    shot_info *i = &r->info;
    i->id = id;
    i->status = r16(m + 12), i->format = r16(m + 14), i->width = r16(m + 16), i->height = r16(m + 18);
    i->stride = r32(m + 20), i->total = r32(m + 24), i->chunk = r32(m + 28), i->chunks = r32(m + 32);
    i->capture_us = r32(m + 36), i->fb_rev = r32(m + 40);
    if (i->status != SHOT_ST_OK) return fail(r, "panel could not capture", "status %lu (%lu)", i->status, i->id);
    if (i->format != SHOT_FORMAT_RGB565LE || !i->width || !i->height || i->stride != (uint32_t)i->width * 2u)
      return fail(r, "info: unsupported format or geometry", "format %lu, stride %lu", i->format, i->stride);
    if (i->total != i->stride * i->height) return fail(r, "info: size mismatch", "total %lu, stride*height %lu", i->total, i->stride * i->height);
    if (i->total > r->cap) return fail(r, "info: image larger than the buffer", "%lu > %lu", i->total, (uint32_t)r->cap);
    if (!i->chunk || i->chunk > SHOT_CHUNK || i->chunks != (i->total + i->chunk - 1) / i->chunk)
      return fail(r, "info: chunk geometry", "chunk %lu, chunks %lu", i->chunk, i->chunks);
    r->state = 1;
    return SHOT_RX_MORE;
  }
  /* state 1: this capture's data, then its end */
  if (!mine) return fail(r, k == SHOT_NONE ? "a non-screenshot message during the capture" : "another capture's message during this one",
                         "kind %lu, %lu bytes", (uint32_t)k, (uint32_t)n);
  const shot_info *i = &r->info;
  if (k == SHOT_DT) {
    if (n < SHOT_DT_HDR) return fail(r, "data: short header", "%lu bytes (min %lu)", (uint32_t)n, SHOT_DT_HDR);
    uint32_t idx = r32(m + 12), off = r32(m + 16), len = r32(m + 20);
    if (idx < r->next) return fail(r, "data: duplicate or out of order", "index %lu, want %lu", idx, r->next);
    if (idx > r->next) return fail(r, "data: chunk(s) missing", "index %lu, want %lu", idx, r->next);
    if (idx >= i->chunks) return fail(r, "data: more chunks than announced", "index %lu of %lu", idx, i->chunks);
    uint32_t want_len = i->total - idx * i->chunk < i->chunk ? i->total - idx * i->chunk : i->chunk;
    if (off != idx * i->chunk) return fail(r, "data: wrong offset", "offset %lu, want %lu", off, idx * i->chunk);
    if (len != want_len || n != SHOT_DT_HDR + len) return fail(r, "data: wrong length", "len %lu, want %lu", len, want_len);
    memcpy(r->buf + off, m + SHOT_DT_HDR, len);
    r->crc = shot_crc32_update(r->crc, m + SHOT_DT_HDR, len);
    ++r->next, ++r->data_msgs;
    return SHOT_RX_MORE;
  }
  if (k == SHOT_EN) {
    if (n != SHOT_EN_LEN) return fail(r, "end: wrong length", "%lu bytes (want %lu)", (uint32_t)n, SHOT_EN_LEN);
    uint32_t chunks = r32(m + 12), total = r32(m + 16);
    r->panel_crc = r32(m + 20);
    if (r->next != i->chunks) return fail(r, "end: truncated", "%lu of %lu chunks", r->next, i->chunks);
    if (chunks != i->chunks || total != i->total) return fail(r, "end: count or size mismatch", "chunks %lu, total %lu", chunks, total);
    if (r->panel_crc != r->crc) return fail(r, "end: CRC-32 mismatch", "panel %08lx, received %08lx", r->panel_crc, r->crc);
    r->state = 2;
    return SHOT_RX_DONE;
  }
  return fail(r, "a second info for this capture", "kind %lu, %lu bytes", (uint32_t)k, (uint32_t)n);
}
