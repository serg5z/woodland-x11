/* Woodland SPI link: framed, full-duplex, fixed-size transactions between a host computer
 * (SPI master) and the CrowPanel ESP32-S3 (SPI slave). Portable C99, no allocation, no I/O:
 * the same code runs on both ends, in the host simulator and in tests.
 * The panel side is in esp32/x11_esp32/x11_spi_esp32.cpp.
 *
 * One SPI transaction exchanges one frame in each direction at the same time. Before a
 * transaction each side calls spl_build() for its outgoing frame; after it, spl_accept() with
 * the frame it received. The slave raises READY only once its transaction is armed (driver),
 * so the master never clocks an unprepared slave.
 *
 * Frame (frame_size bytes, a multiple of 4; 4096 on hardware):
 *   0  'W' 'S'   magic
 *   2  version   SPL_VERSION
 *   3  flags     SPL_F_NAK (the last frame received was bad), SPL_F_RESYNC (this side resynced
 *                to a new peer epoch since its last frame: diagnostic)
 *   4  seq       u16 LE: this data frame's number (meaningless when len = 0)
 *   6  ack       u16 LE: the last data frame received in order
 *   8  credit    receive space free, in 64-byte units (saturating 255)
 *   9  epoch     the sender's epoch: changes when it (re)starts its sequence (each boot)
 *   10 len       u16 LE payload length (0: a control frame: acks, credit, NAK only)
 *   12 base      u16 LE: the sender's first unacknowledged data seq (where a receiver that
 *                (re)synchronises starts, so nothing still held by the sender is skipped)
 *   14 0 0
 *   16 payload   messages: [u16 LE length][bytes] ... (a message never spans frames)
 *   ...          zeros
 *   N-4 CRC-32 (IEEE 802.3) of bytes 0..N-5, LE
 *
 * Reliability: go-back-N with a window of SPL_WINDOW data frames per direction (the ack for a
 * frame can only come one transaction later, so a window of 2 keeps every transaction busy).
 * A bad frame (magic, version, length, CRC) is dropped and NAKed; the sender then resends
 * from the first unacknowledged frame; it also resends when no ack progress was seen for
 * cfg.retx_after transactions. Frames out of order or repeated are dropped (and re-acked).
 * Credit: a sender puts no more message bytes in flight than the peer last reported free.
 * Restarts: a receiver that sees a new peer epoch (or a long run of frames it cannot place,
 * e.g. an epoch collision) resynchronises to the peer's `base`. Delivery is exactly once and
 * in order while neither side restarts; a side that restarts loses what it had queued (its
 * process state is gone) and the peer continues without duplicates or gaps from its own
 * stream: the Woodland protocol resynchronises its application state after a restart. */
#ifndef SPILINK_H
#define SPILINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPL_VERSION 1
#define SPL_HDR 16
#define SPL_TRAILER 4
#define SPL_WINDOW 2
#define SPL_MIN_FRAME 64
#define SPL_MAX_FRAME 8192
#define SPL_CREDIT_UNIT 64
enum { SPL_F_NAK = 0x01, SPL_F_RESYNC = 0x02 };
#define SPL_RESYNC_AFTER 8  /* frames in a row that cannot be placed -> resync to the peer's base */

/* Byte ring of length-prefixed messages; storage given by the owner (bounded). */
typedef struct {
  uint8_t *buf;
  size_t cap, head, len;  /* bytes */
  uint32_t count;         /* messages */
} spl_ring;

typedef struct {
  size_t frame_size;      /* multiple of 4, SPL_MIN_FRAME..SPL_MAX_FRAME */
  uint32_t retx_after;    /* transactions without ack progress before resending (>= 2) */
  uint8_t epoch;          /* this side's epoch: different on every boot (random or a boot count) */
} spl_config;

typedef struct {
  /* counters (diagnostics) */
  uint32_t frames_in, frames_out, data_in, data_out, bad_magic, bad_crc, bad_len, naks_in, naks_out;
  uint32_t retransmits, duplicates, out_of_order, resyncs, msgs_in, msgs_out;
  uint32_t rx_overflow, credit_waits, bad_payload;
} spl_stats;

typedef struct {
  spl_config cfg;
  size_t max_payload;
  /* outgoing */
  spl_ring txq;                        /* messages not yet in a frame */
  uint8_t *inflight;                   /* SPL_WINDOW payload copies (max_payload each) */
  uint16_t inflight_len[SPL_WINDOW];
  uint16_t base;                       /* first unacknowledged data seq */
  uint16_t next;                       /* next new data seq */
  uint16_t resend;                     /* next seq to (re)send within [base, next) */
  uint32_t since_progress;             /* transactions since the ack last advanced */
  size_t inflight_total;               /* bytes in inflight frames (still count against txq) */
  /* incoming */
  spl_ring rxq;                        /* delivered messages, waiting for spl_recv */
  uint16_t expected;                   /* next data seq expected from the peer */
  int peer_synced;                     /* `expected` follows a known peer epoch */
  uint8_t peer_epoch;
  uint32_t unplaced;                   /* data frames in a row neither expected nor duplicate */
  int nak_pending, resynced;
  uint32_t peer_credit;                /* bytes the peer can take, from its last frame */
  int peer_credit_known;
  spl_stats st;
} spl_link;

/* Storage: tx_buf/rx_buf for the message rings, inflight_buf of SPL_WINDOW * max_payload
 * bytes (max_payload = frame_size - SPL_HDR - SPL_TRAILER). Returns 0, or -1 on a bad config. */
int spl_init(spl_link *l, const spl_config *cfg, uint8_t *tx_buf, size_t tx_cap, uint8_t *rx_buf, size_t rx_cap,
             uint8_t *inflight_buf);
size_t spl_inflight_bytes(const spl_config *cfg);

/* Queue one message (1 .. max_payload - 2 bytes). 0, or -1: too large or the queue is full. */
int spl_send(spl_link *l, const uint8_t *msg, size_t n);
/* Take the next delivered message: its length (copied up to cap), or 0 when none. A message
 * longer than cap is truncated and still consumed (cap should be max_payload). */
size_t spl_recv(spl_link *l, uint8_t *out, size_t cap);
uint32_t spl_pending_out(const spl_link *l);  /* messages queued or in flight */

/* Build the next outgoing frame (frame_size bytes). */
void spl_build(spl_link *l, uint8_t *frame);
/* Process the frame received in the same transaction. */
void spl_accept(spl_link *l, const uint8_t *frame);

uint32_t spl_crc32(const uint8_t *p, size_t n);
/* Frame check only (magic, version, length, CRC): 1 if acceptable. */
int spl_frame_ok(const uint8_t *frame, size_t frame_size);

/* ---- master side: READY and link state (the host) -----------------------------------------
 * The master clocks a transaction only while READY is high (the slave armed it). The link is
 * "up" while good frames arrive; after down_after_ms without one it is "down" (the host then
 * stops driving the bus between polls: unpowered-panel protection). */
typedef struct {
  uint32_t down_after_ms;
  int up;
  uint32_t last_good_ms;
  uint32_t transfers, ready_waits, downs, ups;
} spl_master;
void spl_master_init(spl_master *m, uint32_t down_after_ms, uint32_t now_ms);
/* 1: clock a transaction now (READY is high); 0: wait (counted). Updates up/down. */
int spl_master_ready(spl_master *m, int ready, uint32_t now_ms);
/* After the transaction: was the received frame good (spl_frame_ok)? */
void spl_master_done(spl_master *m, int frame_good, uint32_t now_ms);

/* Diagnostics text "<prefix>.<key>=<n>\n" lines (e.g. prefix "spi"). */
size_t spl_stats_text(const spl_link *l, const char *prefix, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
