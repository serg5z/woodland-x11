/* Woodland SPI link, see spilink.h. */
#include "spilink.h"

#include <stdio.h>
#include <string.h>

/* ---- little helpers ------------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static int16_t sdiff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }  /* a - b, wrapping */

uint32_t spl_crc32(const uint8_t *p, size_t n) {
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
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* ---- message ring: [u16 LE len][bytes] records, bounded ------------------------------------- */

static void ring_put(spl_ring *r, const uint8_t *p, size_t n) {
  size_t tail = (r->head + r->len) % r->cap;
  for (size_t i = 0; i < n; ++i) r->buf[(tail + i) % r->cap] = p[i];
  r->len += n;
}

static void ring_get(spl_ring *r, uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p) p[i] = r->buf[(r->head + i) % r->cap];
  }
  r->head = (r->head + n) % r->cap;
  r->len -= n;
}

static size_t ring_peek_len(const spl_ring *r) {
  if (r->len < 2) return 0;
  return (size_t)(r->buf[r->head] | r->buf[(r->head + 1) % r->cap] << 8);
}

static size_t ring_free(const spl_ring *r) { return r->cap - r->len; }

/* ---- setup --------------------------------------------------------------------------------- */

size_t spl_inflight_bytes(const spl_config *cfg) {
  return SPL_WINDOW * (cfg->frame_size - SPL_HDR - SPL_TRAILER);
}

int spl_init(spl_link *l, const spl_config *cfg, uint8_t *tx_buf, size_t tx_cap, uint8_t *rx_buf, size_t rx_cap,
             uint8_t *inflight_buf) {
  memset(l, 0, sizeof(*l));
  if (!cfg || cfg->frame_size < SPL_MIN_FRAME || cfg->frame_size > SPL_MAX_FRAME || cfg->frame_size % 4 ||
      cfg->retx_after < 2 || !tx_buf || !rx_buf || !inflight_buf)
    return -1;
  l->cfg = *cfg;
  l->max_payload = cfg->frame_size - SPL_HDR - SPL_TRAILER;
  if (tx_cap < l->max_payload || rx_cap < l->max_payload) return -1;  /* one full frame must fit */
  l->txq.buf = tx_buf, l->txq.cap = tx_cap;
  l->rxq.buf = rx_buf, l->rxq.cap = rx_cap;
  l->inflight = inflight_buf;
  return 0;
}

/* ---- application side ---------------------------------------------------------------------- */

int spl_send(spl_link *l, const uint8_t *msg, size_t n) {
  if (!n || n + 2 > l->max_payload) return -1;
  /* bytes in flight still count against the queue: they return to it if the link restarts */
  if (l->txq.len + l->inflight_total + n + 2 > l->txq.cap) return -1;
  uint8_t h[2];
  wr16(h, (uint16_t)n);
  ring_put(&l->txq, h, 2);
  ring_put(&l->txq, msg, n);
  ++l->txq.count;
  return 0;
}

size_t spl_recv(spl_link *l, uint8_t *out, size_t cap) {
  size_t n = ring_peek_len(&l->rxq);
  if (!l->rxq.count || !n) return 0;
  ring_get(&l->rxq, 0, 2);
  size_t take = n < cap ? n : cap;
  ring_get(&l->rxq, out, take);
  if (n > take) ring_get(&l->rxq, 0, n - take);
  --l->rxq.count;
  return n;
}

uint32_t spl_pending_out(const spl_link *l) {
  uint32_t inflight = 0;
  for (uint16_t s = l->base; s != l->next; ++s) {
    const uint8_t *p = l->inflight + (size_t)(s % SPL_WINDOW) * l->max_payload;
    for (size_t o = 0; o + 2 <= l->inflight_len[s % SPL_WINDOW]; o += 2 + rd16(p + o)) ++inflight;
  }
  return l->txq.count + inflight;
}

/* ---- frames ------------------------------------------------------------------------------- */

int spl_frame_ok(const uint8_t *f, size_t fs) {
  if (f[0] != 'W' || f[1] != 'S' || f[2] != SPL_VERSION) return 0;
  if (rd16(f + 10) > fs - SPL_HDR - SPL_TRAILER) return 0;
  uint32_t c = (uint32_t)f[fs - 4] | (uint32_t)f[fs - 3] << 8 | (uint32_t)f[fs - 2] << 16 | (uint32_t)f[fs - 1] << 24;
  return c == spl_crc32(f, fs - 4);
}

/* Move whole messages from the queue into a new data frame payload, within `room` bytes. */
static size_t fill_payload(spl_link *l, uint8_t *p, size_t room) {
  size_t used = 0;
  while (l->txq.count) {
    size_t n = ring_peek_len(&l->txq);
    if (used + 2 + n > room) break;
    ring_get(&l->txq, p + used, 2 + n);
    used += 2 + n;
    --l->txq.count;
    ++l->st.msgs_out;
  }
  return used;
}

void spl_build(spl_link *l, uint8_t *f) {
  size_t fs = l->cfg.frame_size;
  memset(f, 0, fs);
  f[0] = 'W', f[1] = 'S', f[2] = SPL_VERSION;
  if (l->nak_pending) f[3] |= SPL_F_NAK, ++l->st.naks_out, l->nak_pending = 0;
  if (l->resynced) f[3] |= SPL_F_RESYNC, l->resynced = 0;
  wr16(f + 6, (uint16_t)(l->expected - 1));
  size_t free_rx = ring_free(&l->rxq) / SPL_CREDIT_UNIT;
  f[8] = (uint8_t)(free_rx > 255 ? 255 : free_rx);
  f[9] = l->cfg.epoch;
  wr16(f + 12, l->base);
  /* no ack progress for a while: go back to the first unacknowledged frame */
  if (l->base != l->next && ++l->since_progress >= l->cfg.retx_after) {
    l->resend = l->base;
    l->since_progress = 0;
  }
  uint16_t len = 0, seq = l->next;
  if (sdiff(l->next, l->resend) > 0) {  /* resend */
    seq = l->resend++;
    len = l->inflight_len[seq % SPL_WINDOW];
    memcpy(f + SPL_HDR, l->inflight + (size_t)(seq % SPL_WINDOW) * l->max_payload, len);
    ++l->st.retransmits;
  } else if (sdiff(l->next, l->base) < SPL_WINDOW && l->txq.count && l->peer_credit_known) {
    size_t room = l->max_payload;
    size_t credit = l->peer_credit > l->inflight_total ? l->peer_credit - l->inflight_total : 0;
    if (credit < room) room = credit;
    size_t used = fill_payload(l, l->inflight + (size_t)(l->next % SPL_WINDOW) * l->max_payload, room);
    if (used) {
      seq = l->next++;
      l->resend = l->next;
      len = (uint16_t)used;
      l->inflight_len[seq % SPL_WINDOW] = len;
      l->inflight_total += used;
      memcpy(f + SPL_HDR, l->inflight + (size_t)(seq % SPL_WINDOW) * l->max_payload, len);
    } else {
      ++l->st.credit_waits;
    }
  }
  wr16(f + 4, seq);
  wr16(f + 10, len);
  if (len) ++l->st.data_out;
  uint32_t c = spl_crc32(f, fs - 4);
  f[fs - 4] = (uint8_t)c, f[fs - 3] = (uint8_t)(c >> 8), f[fs - 2] = (uint8_t)(c >> 16), f[fs - 1] = (uint8_t)(c >> 24);
  ++l->st.frames_out;
}

/* The payload as messages: all lengths valid and the total exact. */
static int payload_ok(const uint8_t *p, size_t len, size_t *bytes, uint32_t *msgs) {
  size_t o = 0;
  uint32_t m = 0;
  while (o < len) {
    if (o + 2 > len) return 0;
    size_t n = rd16(p + o);
    if (!n || o + 2 + n > len) return 0;
    o += 2 + n;
    ++m;
  }
  *bytes = len, *msgs = m;
  return 1;
}

static void resync(spl_link *l, uint8_t epoch, uint16_t base) {
  l->expected = base;
  l->peer_epoch = epoch;
  l->peer_synced = 1;
  l->unplaced = 0;
  l->resynced = 1;
  ++l->st.resyncs;
}

void spl_accept(spl_link *l, const uint8_t *f) {
  size_t fs = l->cfg.frame_size;
  ++l->st.frames_in;
  if (f[0] != 'W' || f[1] != 'S' || f[2] != SPL_VERSION) {
    ++l->st.bad_magic;
    l->nak_pending = 1;
    return;
  }
  if (!spl_frame_ok(f, fs)) {
    if (rd16(f + 10) > fs - SPL_HDR - SPL_TRAILER) ++l->st.bad_len;
    else ++l->st.bad_crc;
    l->nak_pending = 1;
    return;
  }
  uint8_t flags = f[3], epoch = f[9];
  uint16_t seq = rd16(f + 4), ack = rd16(f + 6), len = rd16(f + 10), base = rd16(f + 12);
  /* the peer's view of our stream */
  l->peer_credit = (uint32_t)f[8] * SPL_CREDIT_UNIT;
  l->peer_credit_known = 1;
  if (sdiff(ack, l->base) >= 0 && sdiff(ack, (uint16_t)(l->next - 1)) <= 0) {  /* progress */
    for (uint16_t s = l->base; s != (uint16_t)(ack + 1); ++s) {
      l->inflight_total -= l->inflight_len[s % SPL_WINDOW];
      l->inflight_len[s % SPL_WINDOW] = 0;
    }
    l->base = (uint16_t)(ack + 1);
    if (sdiff(l->resend, l->base) < 0) l->resend = l->base;
    l->since_progress = 0;
  }
  if (flags & SPL_F_NAK) {
    ++l->st.naks_in;
    l->resend = l->base;  /* go back */
  }
  /* the peer's stream */
  if (!l->peer_synced || epoch != l->peer_epoch) resync(l, epoch, base);
  if (!len) return;
  ++l->st.data_in;
  if (seq != l->expected) {
    int16_t d = sdiff(seq, l->expected);
    if (d < 0 && d >= -SPL_WINDOW) {
      ++l->st.duplicates;  /* a resend after a lost ack: acknowledged again by the ack field */
      l->unplaced = 0;
      return;
    }
    /* Ahead of `expected` (a frame before it was lost: the sender goes back) or far behind
     * (a peer that restarted with the same epoch). A long run of these means the two views
     * of the stream disagree: adopt the peer's base. Safe: a sender only sends seq in
     * [base, base + SPL_WINDOW), so frames ahead imply base >= expected, i.e. nothing is
     * delivered twice; frames far behind only come from a restarted peer (a new stream). */
    ++l->st.out_of_order;
    if (++l->unplaced >= SPL_RESYNC_AFTER) resync(l, epoch, base);
    if (l->expected != seq) return;
  }
  size_t bytes;
  uint32_t msgs;
  if (!payload_ok(f + SPL_HDR, len, &bytes, &msgs)) {
    ++l->st.bad_payload;  /* a sender bug (the CRC was right): not acknowledged */
    return;
  }
  if (bytes > ring_free(&l->rxq)) {
    ++l->st.rx_overflow;  /* only if the peer ignored our credit: resent later */
    return;
  }
  ring_put(&l->rxq, f + SPL_HDR, bytes);
  l->rxq.count += msgs;
  l->st.msgs_in += msgs;
  ++l->expected;
  l->unplaced = 0;
}

/* ---- master side --------------------------------------------------------------------------- */

void spl_master_init(spl_master *m, uint32_t down_after_ms, uint32_t now_ms) {
  memset(m, 0, sizeof(*m));
  m->down_after_ms = down_after_ms;
  m->last_good_ms = now_ms;
}

static void master_age(spl_master *m, uint32_t now_ms) {
  if (m->up && now_ms - m->last_good_ms >= m->down_after_ms) {
    m->up = 0;
    ++m->downs;
  }
}

int spl_master_ready(spl_master *m, int ready, uint32_t now_ms) {
  master_age(m, now_ms);
  if (!ready) {
    ++m->ready_waits;
    return 0;
  }
  ++m->transfers;
  return 1;
}

void spl_master_done(spl_master *m, int frame_good, uint32_t now_ms) {
  if (frame_good) {
    m->last_good_ms = now_ms;
    if (!m->up) {
      m->up = 1;
      ++m->ups;
    }
  }
  master_age(m, now_ms);
}

/* ---- diagnostics --------------------------------------------------------------------------- */

size_t spl_stats_text(const spl_link *l, const char *px, char *out, size_t cap) {
  const spl_stats *s = &l->st;
  int n = snprintf(out, cap,
                   "%s.frames_in=%lu\n%s.frames_out=%lu\n%s.data_in=%lu\n%s.data_out=%lu\n%s.msgs_in=%lu\n"
                   "%s.bad_magic=%lu\n%s.bad_crc=%lu\n%s.bad_len=%lu\n%s.bad_payload=%lu\n%s.naks_in=%lu\n"
                   "%s.naks_out=%lu\n%s.retransmits=%lu\n%s.duplicates=%lu\n%s.out_of_order=%lu\n"
                   "%s.resyncs=%lu\n%s.rx_overflow=%lu\n%s.credit_waits=%lu\n%s.queued_out=%lu\n",
                   px, (unsigned long)s->frames_in, px, (unsigned long)s->frames_out, px, (unsigned long)s->data_in,
                   px, (unsigned long)s->data_out, px, (unsigned long)s->msgs_in, px, (unsigned long)s->bad_magic, px,
                   (unsigned long)s->bad_crc, px, (unsigned long)s->bad_len, px, (unsigned long)s->bad_payload, px,
                   (unsigned long)s->naks_in, px, (unsigned long)s->naks_out, px, (unsigned long)s->retransmits, px,
                   (unsigned long)s->duplicates, px, (unsigned long)s->out_of_order, px, (unsigned long)s->resyncs,
                   px, (unsigned long)s->rx_overflow, px, (unsigned long)s->credit_waits, px,
                   (unsigned long)spl_pending_out(l));
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}
