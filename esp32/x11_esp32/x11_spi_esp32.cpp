// SPI2 slave adapter for the Woodland SPI link (see x11_spi_esp32.h). Test builds only.
#include "x11_spi_esp32.h"

#ifdef X11_SPI_ECHO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_timer.h"
#include "src/spilink.h"
#ifdef X11_SPI_X11
#include "src/x11spi.h"
#endif
#ifdef X11_SPI_SHOT
#include "src/shot_msg.h"
#endif

namespace {

spl_link sl;  // not `link`: that is the POSIX function
spl_config cfg;
uint8_t *txq, *rxq, *inflight;        // PSRAM: message queues and in-flight copies
uint8_t *dma_tx, *dma_rx;             // internal DMA-capable RAM, 4-byte aligned
uint8_t *msg;                         // one message being echoed (PSRAM)
size_t msg_len = 0;                   // > 0: a received message still waiting to be sent back
bool on = false, queued = false, ok = false;
bool pending_level = false;
uint32_t cs_since = 0;
int cs_last = -1;
spi_slave_transaction_t trans;
uint32_t transactions = 0, starts = 0, stops = 0, echoed = 0, echo_waits = 0, queue_errors = 0, short_frames = 0;
uint32_t pending_asserts = 0, pending_clears = 0;
#ifdef X11_SPI_X11
int (*x11_message)(uint8_t, uint32_t, const uint8_t *, size_t) = nullptr;
void (*x11_reset)() = nullptr;
uint8_t x11_packet[SPI_FRAME];
uint8_t x11_peer_epoch = 0;
bool x11_peer_seen = false;
#endif

#ifdef X11_SPI_SHOT
void shot_abort();
#endif

void IRAM_ATTR ready_high(spi_slave_transaction_t *) { gpio_set_level((gpio_num_t)SPI_PIN_READY, 1); }
void IRAM_ATTR ready_low(spi_slave_transaction_t *) { gpio_set_level((gpio_num_t)SPI_PIN_READY, 0); }

// GPIO20/J9-1 is the separate DATA_PENDING line. It is active-low open-drain: released HIGH
// means no unacknowledged panel output; LOW means the host should clock frames. Queue changes
// happen in the Arduino loop/X11 callbacks, never in the SPI driver's ISR callbacks.
constexpr int PIN_DATA_PENDING = 20;
void data_pending_init() {
  gpio_reset_pin((gpio_num_t)PIN_DATA_PENDING);
  gpio_set_direction((gpio_num_t)PIN_DATA_PENDING, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_pull_mode((gpio_num_t)PIN_DATA_PENDING, GPIO_FLOATING);
  gpio_set_level((gpio_num_t)PIN_DATA_PENDING, 1);  // open-drain release; the host's pull-up defines idle
  pending_level = false;
}
void data_pending_update() {
  bool pending = on && spl_pending_out(&sl) != 0;
  if (pending == pending_level) return;
  gpio_set_level((gpio_num_t)PIN_DATA_PENDING, pending ? 0 : 1);
  pending_level = pending;
  if (pending) ++pending_asserts; else ++pending_clears;
}

// Every SPI pin as a plain input (high impedance); CS with the pull-down that makes a dead
// or absent host read LOW.
void pins_idle() {
  const int pins[] = {SPI_PIN_SCLK, SPI_PIN_MOSI, SPI_PIN_MISO, SPI_PIN_READY};
  for (int p : pins) {
    gpio_reset_pin((gpio_num_t)p);
    gpio_set_direction((gpio_num_t)p, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)p, GPIO_FLOATING);
  }
  gpio_reset_pin((gpio_num_t)SPI_PIN_CS);
  gpio_set_direction((gpio_num_t)SPI_PIN_CS, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)SPI_PIN_CS, GPIO_PULLDOWN_ONLY);
}

void arm() {  // the next frame out, and READY (post_setup_cb) once the hardware is armed
  spl_build(&sl, dma_tx);
  memset(dma_rx, 0, SPI_FRAME);
  memset(&trans, 0, sizeof(trans));
  trans.length = SPI_FRAME * 8;
  trans.tx_buffer = dma_tx;
  trans.rx_buffer = dma_rx;
  queued = spi_slave_queue_trans(SPI2_HOST, &trans, 0) == ESP_OK;
  if (!queued) ++queue_errors;
}

void start() {
  gpio_reset_pin((gpio_num_t)SPI_PIN_READY);
  gpio_set_direction((gpio_num_t)SPI_PIN_READY, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)SPI_PIN_READY, 0);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = SPI_PIN_MOSI;
  bus.miso_io_num = SPI_PIN_MISO;
  bus.sclk_io_num = SPI_PIN_SCLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = SPI_FRAME;
  spi_slave_interface_config_t slv = {};
  slv.spics_io_num = SPI_PIN_CS;
  slv.queue_size = 1;
  slv.mode = 0;
  slv.post_setup_cb = ready_high;
  slv.post_trans_cb = ready_low;
  if (spi_slave_initialize(SPI2_HOST, &bus, &slv, SPI_DMA_CH_AUTO) != ESP_OK) {
    pins_idle();
    ++queue_errors;
    return;
  }
  gpio_set_pull_mode((gpio_num_t)SPI_PIN_CS, GPIO_PULLDOWN_ONLY);  // keep the absent-host reading
  on = true;
  ++starts;
  arm();
  data_pending_update();
}

void stop() {
  if (queued) {  // the queued transaction never ran: take it back
    spi_slave_transaction_t *r;
    spi_slave_get_trans_result(SPI2_HOST, &r, 0);
  }
  spi_slave_free(SPI2_HOST);
  queued = false;
  on = false;
  data_pending_update();  // releases the line when the SPI slave is stopped
  ++stops;
#ifdef X11_SPI_X11
  if (x11_reset) x11_reset();
#endif
#ifdef X11_SPI_SHOT
  shot_abort();  // the host is gone: do not keep queueing its screenshot
#endif
  pins_idle();  // MISO and READY high impedance again
}

#ifdef X11_SPI_SHOT
const uint16_t *shot_fb;
const uint32_t *shot_rev;
uint32_t shot_w, shot_h;
uint8_t *snap;            // the snapshot (PSRAM), allocated on the first request and kept
uint8_t *smsg;            // the message being queued (PSRAM)
size_t smsg_len = 0;
shot_tx stx;
uint32_t shot_resyncs_seen;
uint32_t shot_requests, shot_captures, shot_failed, shot_capture_us, shot_chunks, shot_ends, shot_aborted, shot_waits;

void shot_abort() {
  if (stx.active) ++shot_aborted;
  stx.active = 0, smsg_len = 0;
}

void shot_request(uint32_t id) {
  shot_abort();
  ++shot_requests;
  uint32_t total = shot_w * shot_h * 2;
  shot_info i = {id, SHOT_ST_OK, SHOT_FORMAT_RGB565LE, (uint16_t)shot_w, (uint16_t)shot_h, shot_w * 2, total,
                 SHOT_CHUNK, (total + SHOT_CHUNK - 1) / SHOT_CHUNK, 0, 0};
  if (!shot_fb || !smsg) {
    i.status = SHOT_ST_NO_SCREEN;
  } else {
    if (!snap) snap = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) i.status = SHOT_ST_NO_MEMORY;
  }
  if (i.status == SHOT_ST_OK) {
    // One pass: nothing else in loop() runs during the copy, so the image cannot tear.
    int64_t t = esp_timer_get_time();
    memcpy(snap, shot_fb, total);
    i.capture_us = shot_capture_us = (uint32_t)(esp_timer_get_time() - t);
    i.fb_rev = *shot_rev;
    ++shot_captures;
  } else {
    ++shot_failed;
  }
  shot_tx_start(&stx, &i, snap);
  shot_resyncs_seen = sl.st.resyncs;
}

void shot_pump() {
  if (stx.active && sl.st.resyncs != shot_resyncs_seen) shot_abort();  // a new master: it did not ask
  for (;;) {
    if (!smsg_len) {
      if (!stx.active) return;
      smsg_len = shot_tx_peek(&stx, smsg, SPI_FRAME);
      if (!smsg_len) return;
    }
    if (spl_send(&sl, smsg, smsg_len) != 0) {
      ++shot_waits;
      return;
    }
    if (stx.next >= 1 && stx.next <= stx.info.chunks) ++shot_chunks;
    else if (stx.next == stx.info.chunks + 1) ++shot_ends;
    shot_tx_commit(&stx);
    smsg_len = 0;
  }
}
#endif

}  // namespace

#ifdef X11_SPI_SHOT
void spi_shot_source(const uint16_t *fb, const uint32_t *rev, int w, int h) {
  shot_fb = fb, shot_rev = rev, shot_w = (uint32_t)w, shot_h = (uint32_t)h;
  if (!smsg) smsg = (uint8_t *)heap_caps_malloc(SPI_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
#endif

void spi_echo_init() {
  cfg.frame_size = SPI_FRAME;
  cfg.retx_after = 4;
  cfg.epoch = (uint8_t)(esp_random() & 0xFF);
  size_t maxp = SPI_FRAME - SPL_HDR - SPL_TRAILER;
  txq = (uint8_t *)heap_caps_malloc(3 * maxp, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rxq = (uint8_t *)heap_caps_malloc(3 * maxp, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  inflight = (uint8_t *)heap_caps_malloc(spl_inflight_bytes(&cfg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  msg = (uint8_t *)heap_caps_malloc(SPI_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  dma_tx = (uint8_t *)heap_caps_aligned_alloc(4, SPI_FRAME, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  dma_rx = (uint8_t *)heap_caps_aligned_alloc(4, SPI_FRAME, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  ok = txq && rxq && inflight && msg && dma_tx && dma_rx &&
       spl_init(&sl, &cfg, txq, 3 * maxp, rxq, 3 * maxp, inflight) == 0;
  data_pending_init();
  pins_idle();
  Serial.printf("SPI echo (test build): %s; epoch %u; slave starts when the host holds CS high\n",
                ok ? "ready" : "NO MEMORY", (unsigned)cfg.epoch);
}

void spi_echo_poll() {
  if (!ok) return;
  uint32_t now = millis();
  int cs = gpio_get_level((gpio_num_t)SPI_PIN_CS);
  if (cs != cs_last) cs_last = cs, cs_since = now;
  if (!on && cs == 1 && now - cs_since >= SPI_ALIVE_MS) start();
  else if (on && cs == 0 && now - cs_since >= SPI_DEAD_MS) stop();
  if (!on) return;
  spi_slave_transaction_t *done;
  if (queued && spi_slave_get_trans_result(SPI2_HOST, &done, 0) == ESP_OK) {
    queued = false;
    ++transactions;
    if (done->trans_len != SPI_FRAME * 8) ++short_frames;  // fails the frame checks below: NAKed
    spl_accept(&sl, dma_rx);
#ifdef X11_SPI_X11
    if (sl.peer_synced) {
      if (x11_peer_seen && x11_peer_epoch != sl.peer_epoch && x11_reset) x11_reset();
      x11_peer_epoch = sl.peer_epoch;
      x11_peer_seen = true;
    }
#endif
  }
  // echo: every message received goes back unchanged (bounded: one waits if the queue is full)
  for (;;) {
    if (!msg_len) msg_len = spl_recv(&sl, msg, SPI_FRAME);
    if (!msg_len) break;
#ifdef X11_SPI_X11
    uint8_t op;
    uint32_t session;
    const uint8_t *data;
    size_t len;
    if (xspi_parse(msg, msg_len, &op, &session, &data, &len) == 0) {
      // Keep the one pending message until the X11 adapter can accept it.
      if (x11_message && x11_message(op, session, data, len) != 0) break;
      msg_len = 0;
      continue;
    }
#endif
#ifdef X11_SPI_SHOT
    uint32_t rid;
    if (shot_kind(msg, msg_len, &rid) == SHOT_RQ) {  // a screenshot request: handled, not echoed
      shot_request(rid);
      msg_len = 0;
      continue;
    }
#endif
    if (spl_send(&sl, msg, msg_len) != 0) {
      ++echo_waits;
      break;
    }
    msg_len = 0;
    ++echoed;
  }
#ifdef X11_SPI_SHOT
  shot_pump();
#endif
  if (!queued) arm();
  data_pending_update();
}

#ifdef X11_SPI_X11
void spi_x11_init(int (*message)(uint8_t, uint32_t, const uint8_t *, size_t), void (*reset)()) {
  x11_message = message;
  x11_reset = reset;
}
int spi_x11_send(uint8_t op, uint32_t session, const uint8_t *data, size_t n) {
  if (!ok || !on) return -1;
  size_t len = xspi_make(x11_packet, op, session, data, n);
  if (!len || spl_send(&sl, x11_packet, len) != 0) return -1;
  data_pending_update();
  return 0;
}
#endif

#ifdef X11_SPI_SHOT
static const char TEST_BUILD[] = "echo+shot";
#else
static const char TEST_BUILD[] = "echo";
#endif

size_t spi_echo_diag(char *out, size_t cap) {
  int n = snprintf(out, cap,
                   "spi.test_build=%s\nspi.init=%s\nspi.cs=%d\nspi.state=%s\nspi.transactions=%lu\nspi.starts=%lu\nspi.stops=%lu\n"
                   "spi.echoed=%lu\nspi.echo_waits=%lu\nspi.queue_errors=%lu\nspi.short_frames=%lu\n"
                   "spi.data_pending_gpio=%d\nspi.data_pending_level=%s\nspi.data_pending_asserts=%lu\nspi.data_pending_clears=%lu\n",
                   TEST_BUILD, ok ? "ok" : "failed", gpio_get_level((gpio_num_t)SPI_PIN_CS), on ? "on" : "off", (unsigned long)transactions, (unsigned long)starts, (unsigned long)stops,
                   (unsigned long)echoed, (unsigned long)echo_waits, (unsigned long)queue_errors,
                   (unsigned long)short_frames, PIN_DATA_PENDING, pending_level ? "low" : "released",
                   (unsigned long)pending_asserts, (unsigned long)pending_clears);
  if (n < 0) return 0;
  size_t used = (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
#ifdef X11_SPI_SHOT
  n = snprintf(out + used, cap - used,
               "shot.state=%s\nshot.requests=%lu\nshot.captures=%lu\nshot.failed=%lu\nshot.capture_us=%lu\n"
               "shot.chunks_queued=%lu\nshot.ends=%lu\nshot.aborted=%lu\nshot.waits=%lu\nshot.snapshot=%s\n",
               stx.active ? "sending" : "idle", (unsigned long)shot_requests, (unsigned long)shot_captures,
               (unsigned long)shot_failed, (unsigned long)shot_capture_us, (unsigned long)shot_chunks,
               (unsigned long)shot_ends, (unsigned long)shot_aborted, (unsigned long)shot_waits, snap ? "allocated" : "none");
  if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
#endif
  return used + spl_stats_text(&sl, "spi", out + used, cap - used);
}
#endif
