/* SPI link (spilink/): codec, go-back-N, credit, READY handling and recovery, through a
 * simulated host (master) / panel (slave) pair with fault injection. Run with ASan/UBSan.
 *
 * Every message carries a counter; a checker per direction asserts delivery exactly once and
 * in order (with the documented exceptions around restarts: a restarted sender's queue is
 * lost, a restarted receiver starts again at the peer's base). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../spilink/spilink.h"

static int failures = 0;
#define CHECK(c)                                                             \
  do {                                                                       \
    if (!(c)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static uint32_t rng = 2463534242u;
static uint32_t rnd(void) {
  rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
  return rng;
}
static int chance(double p) { return p > 0 && (double)(rnd() % 1000000) < p * 1000000.0; }

/* ---- one endpoint: a link, its buffers, a message generator and a checker ------------------ */

#define FS_MAX 4096
typedef struct {
  spl_link l;
  spl_config cfg;
  uint8_t tx[3 * FS_MAX], rx[3 * FS_MAX], fl[SPL_WINDOW * FS_MAX];
  uint8_t frame[FS_MAX];      /* built for the current transaction (slave: armed) */
  int armed;
  uint32_t next_msg;          /* next counter to queue */
  uint32_t want;              /* next counter expected from the peer */
  int want_any;               /* after a restart: accept any counter once */
  uint32_t delivered, errors_order;
  int consume_every;          /* read messages only every N steps (slow consumer) */
  uint32_t max_msg;
} end;

static void end_init(end *e, size_t fs, uint8_t epoch, uint32_t max_msg) {
  memset(e, 0, sizeof(*e));
  e->cfg.frame_size = fs, e->cfg.retx_after = 4, e->cfg.epoch = epoch;
  CHECK(spl_init(&e->l, &e->cfg, e->tx, sizeof(e->tx), e->rx, sizeof(e->rx), e->fl) == 0);
  e->max_msg = max_msg;
  e->consume_every = 1;
}

/* a restart: link state and queued messages gone (the process restarted); counters continue */
static void end_restart(end *e, uint8_t epoch) {
  uint32_t next = e->next_msg, consume = (uint32_t)e->consume_every, maxm = e->max_msg;
  size_t fs = e->cfg.frame_size;
  end_init(e, fs, epoch, maxm);
  e->next_msg = next, e->consume_every = (int)consume, e->want_any = 1;
}

static size_t msg_len(uint32_t k, uint32_t max_msg) { return 4 + (size_t)((k * 2654435761u) >> 7) % (max_msg - 4 + 1); }

static void produce(end *e) {
  for (int i = 0; i < 4; ++i) {
    uint8_t m[FS_MAX];
    size_t n = msg_len(e->next_msg, e->max_msg);
    memcpy(m, &e->next_msg, 4);
    for (size_t j = 4; j < n; ++j) m[j] = (uint8_t)(e->next_msg * 31 + j);
    if (spl_send(&e->l, m, n) != 0) return;
    ++e->next_msg;
  }
}

static void consume(end *e, int step) {
  if (step % e->consume_every) return;
  uint8_t m[FS_MAX];
  size_t n;
  while ((n = spl_recv(&e->l, m, sizeof(m))) > 0) {
    uint32_t k;
    memcpy(&k, m, 4);
    int body_ok = n == msg_len(k, e->max_msg);
    for (size_t j = 4; j < n && body_ok; ++j) body_ok = m[j] == (uint8_t)(k * 31 + j);
    if (!body_ok || (!e->want_any && k != e->want)) {
      if (e->errors_order < 5) fprintf(stderr, "  delivery error: got %u want %u len %zu body %d\n", k, e->want, n, body_ok);
      ++e->errors_order;
    }
    e->want_any = 0;
    e->want = k + 1;
    ++e->delivered;
  }
}

/* ---- the channel ---------------------------------------------------------------------------- */

typedef struct {
  double flip_mosi, flip_miso;   /* a random bit flipped in that direction */
  double slave_miss;             /* CS glitch: the slave sees nothing, the master reads zeros */
  double ready_low;              /* READY held low this step (slave busy) */
  double truncate;               /* the frame's second half arrives as zeros */
  double replay;                 /* the master receives the slave's previous frame again */
} faults;

typedef struct {
  end m, s;                      /* master (host), slave (panel) */
  spl_master mst;
  faults f;
  uint32_t now_ms, transactions;
  uint8_t prev_s[FS_MAX];
} sim;

static void corrupt(uint8_t *f, size_t fs, const faults *fl, double flip) {
  if (chance(flip)) f[rnd() % fs] ^= (uint8_t)(1u << (rnd() % 8));
  if (chance(fl->truncate)) memset(f + fs / 2, 0, fs - fs / 2);
}

static void sim_init(sim *x, size_t fs, uint32_t max_msg) {
  memset(x, 0, sizeof(*x));
  end_init(&x->m, fs, 11, max_msg);
  end_init(&x->s, fs, 22, max_msg);
  spl_master_init(&x->mst, 1000, 0);
}

static void sim_step(sim *x, int step) {
  size_t fs = x->m.cfg.frame_size;
  produce(&x->m);
  produce(&x->s);
  x->now_ms += 1;
  if (!x->s.armed) {  /* the slave arms a transaction: its frame is fixed from now on */
    spl_build(&x->s.l, x->s.frame);
    x->s.armed = 1;
  }
  int ready = !chance(x->f.ready_low);
  if (spl_master_ready(&x->mst, ready, x->now_ms)) {
    ++x->transactions;
    uint8_t mf[FS_MAX], to_s[FS_MAX], to_m[FS_MAX];
    spl_build(&x->m.l, mf);
    memcpy(to_s, mf, fs);
    memcpy(to_m, x->s.frame, fs);
    if (chance(x->f.replay)) memcpy(to_m, x->prev_s, fs);
    memcpy(x->prev_s, x->s.frame, fs);
    corrupt(to_s, fs, &x->f, x->f.flip_mosi);
    corrupt(to_m, fs, &x->f, x->f.flip_miso);
    if (chance(x->f.slave_miss)) {  /* the slave's DMA never ran: it keeps its armed frame */
      memset(to_m, 0, fs);
      spl_accept(&x->m.l, to_m);
      spl_master_done(&x->mst, 0, x->now_ms);
    } else {
      spl_accept(&x->s.l, to_s);
      x->s.armed = 0;
      spl_accept(&x->m.l, to_m);
      spl_master_done(&x->mst, spl_frame_ok(to_m, fs), x->now_ms);
    }
  }
  consume(&x->m, step);
  consume(&x->s, step);
}

static void run(sim *x, int steps) {
  for (int i = 0; i < steps; ++i) sim_step(x, i);
}

/* ---- tests ---------------------------------------------------------------------------------- */

static void test_codec(void) {
  CHECK(spl_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);  /* the CRC-32 check value */
  spl_link l;
  spl_config c = {256, 4, 7};
  uint8_t tx[1024], rx[1024], fl[SPL_WINDOW * 256], f[256];
  CHECK(spl_init(&l, &c, tx, sizeof(tx), rx, sizeof(rx), fl) == 0);
  spl_build(&l, f);
  CHECK(spl_frame_ok(f, 256) && f[0] == 'W' && f[1] == 'S' && f[2] == SPL_VERSION && f[9] == 7);
  uint8_t g[256];
  memcpy(g, f, 256); g[0] = 'X'; CHECK(!spl_frame_ok(g, 256));
  memcpy(g, f, 256); g[2] = 9; CHECK(!spl_frame_ok(g, 256));
  memcpy(g, f, 256); g[100] ^= 1; CHECK(!spl_frame_ok(g, 256));
  memcpy(g, f, 256); g[10] = 0xFF; g[11] = 0xFF; CHECK(!spl_frame_ok(g, 256));  /* len beyond the frame */
  memset(g, 0, 256); CHECK(!spl_frame_ok(g, 256));
  /* bad configurations */
  spl_config bad = {250, 4, 0};  /* not a multiple of 4 */
  CHECK(spl_init(&l, &bad, tx, sizeof(tx), rx, sizeof(rx), fl) != 0);
  bad.frame_size = 32; CHECK(spl_init(&l, &bad, tx, sizeof(tx), rx, sizeof(rx), fl) != 0);
  bad.frame_size = 256, bad.retx_after = 1; CHECK(spl_init(&l, &bad, tx, sizeof(tx), rx, sizeof(rx), fl) != 0);
  bad.retx_after = 4; CHECK(spl_init(&l, &bad, tx, 100, rx, sizeof(rx), fl) != 0);  /* queue smaller than a frame */
  /* bounded queue and message limits */
  CHECK(spl_init(&l, &c, tx, sizeof(tx), rx, sizeof(rx), fl) == 0);
  uint8_t m[300] = {0};
  CHECK(spl_send(&l, m, 0) != 0);
  CHECK(spl_send(&l, m, l.max_payload - 1) != 0);       /* + 2-byte length: too large */
  CHECK(spl_send(&l, m, l.max_payload - 2) == 0);
  int queued = 1;
  while (spl_send(&l, m, 10) == 0) ++queued;
  CHECK(l.txq.len <= l.txq.cap && queued > 1 && spl_pending_out(&l) == (uint32_t)queued);
  char text[2048];
  CHECK(spl_stats_text(&l, "spi", text, sizeof(text)) > 0 && strstr(text, "spi.bad_crc=0\n"));
}

/* A frame with a valid CRC but a malformed payload is not delivered and not acknowledged. */
static void test_malformed_payload(void) {
  spl_link l;
  spl_config c = {256, 4, 1};
  uint8_t tx[1024], rx[1024], fl[SPL_WINDOW * 256], f[256];
  CHECK(spl_init(&l, &c, tx, sizeof(tx), rx, sizeof(rx), fl) == 0);
  memset(f, 0, sizeof(f));
  f[0] = 'W', f[1] = 'S', f[2] = SPL_VERSION, f[8] = 10, f[9] = 5;
  f[10] = 6;              /* len 6 */
  f[16] = 9, f[17] = 0;   /* a message claiming 9 bytes in a 6-byte payload */
  uint32_t crc = spl_crc32(f, 252);
  f[252] = (uint8_t)crc, f[253] = (uint8_t)(crc >> 8), f[254] = (uint8_t)(crc >> 16), f[255] = (uint8_t)(crc >> 24);
  spl_accept(&l, f);
  uint8_t out[256];
  CHECK(l.st.bad_payload == 1 && spl_recv(&l, out, sizeof(out)) == 0 && l.expected == 0);
}

static void report(const char *name, const sim *x) {
  printf("  %-34s tx %6u: msgs m->s %7u s->m %7u; crc %4u naks %4u retx %5u dup %4u ooo %4u resync %u credit-waits %u\n",
         name, x->transactions, x->s.delivered, x->m.delivered, x->m.l.st.bad_crc + x->s.l.st.bad_crc,
         x->m.l.st.naks_in + x->s.l.st.naks_in, x->m.l.st.retransmits + x->s.l.st.retransmits,
         x->m.l.st.duplicates + x->s.l.st.duplicates, x->m.l.st.out_of_order + x->s.l.st.out_of_order,
         x->m.l.st.resyncs + x->s.l.st.resyncs, x->m.l.st.credit_waits + x->s.l.st.credit_waits);
}

static void test_clean(void) {
  sim x;
  sim_init(&x, 4096, 600);
  run(&x, 20000);
  CHECK(x.m.errors_order == 0 && x.s.errors_order == 0);
  CHECK(x.m.l.st.retransmits == 0 && x.s.l.st.retransmits == 0);
  /* a window of 2 keeps every transaction carrying data when there is data */
  CHECK(x.m.l.st.data_out > x.transactions * 95 / 100 && x.s.l.st.data_out > x.transactions * 95 / 100);
  CHECK(x.mst.up && x.mst.downs == 0);
  report("clean, 4096-byte frames", &x);
}

static void test_faults(void) {
  struct {
    const char *name;
    faults f;
  } cases[] = {
      {"bit flips 1% each way", {0.01, 0.01, 0, 0, 0, 0}},
      {"bit flips 20% each way", {0.2, 0.2, 0, 0, 0, 0}},
      {"slave misses 5% (master reads 0s)", {0, 0, 0.05, 0, 0, 0}},
      {"READY low 50%", {0, 0, 0, 0.5, 0, 0}},
      {"truncated frames 5%", {0, 0, 0, 0, 0.05, 0}},
      {"stale replays 5%", {0, 0, 0, 0, 0, 0.05}},
      {"everything at once", {0.1, 0.1, 0.05, 0.3, 0.05, 0.05}},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    sim x;
    sim_init(&x, 512, 120);
    x.f = cases[i].f;
    run(&x, 30000);
    if (x.m.errors_order || x.s.errors_order) fprintf(stderr, "case: %s\n", cases[i].name);
    CHECK(x.m.errors_order == 0 && x.s.errors_order == 0);  /* exactly once, in order */
    CHECK(x.m.delivered > 2000 && x.s.delivered > 2000);     /* and it keeps moving */
    CHECK(x.m.l.st.rx_overflow == 0 && x.s.l.st.rx_overflow == 0);  /* credit honoured */
    /* after the faults stop, everything queued gets through */
    memset(&x.f, 0, sizeof(x.f));
    uint32_t m_goal = x.m.next_msg, s_goal = x.s.next_msg;
    for (int k = 0; k < 4000 && (x.s.want < m_goal || x.m.want < s_goal); ++k) sim_step(&x, k);
    CHECK(x.s.want >= m_goal && x.m.want >= s_goal);
    report(cases[i].name, &x);
  }
}

static void test_slow_consumer(void) {
  sim x;
  sim_init(&x, 512, 120);
  x.s.consume_every = 50;  /* the panel reads its messages rarely (e.g. a long LCD flush) */
  run(&x, 20000);
  CHECK(x.s.errors_order == 0 && x.m.errors_order == 0);
  CHECK(x.m.l.st.credit_waits > 0);                               /* the host waited for credit */
  CHECK(x.s.l.st.rx_overflow == 0 && x.m.l.st.rx_overflow == 0);  /* never more than the space */
  CHECK(x.s.delivered > 1000);
  report("slow panel consumer (credit)", &x);
}

static void test_restarts(void) {
  /* the panel restarts (new epoch) in the middle of traffic, three times */
  sim x;
  sim_init(&x, 512, 120);
  x.f.flip_miso = 0.02;
  uint8_t epoch = 22;
  for (int r = 0; r < 3; ++r) {
    run(&x, 3000);
    end_restart(&x.s, ++epoch);
    x.m.want_any = 1;  /* the panel's queue is gone: the host's checker accepts its next stream start */
  }
  run(&x, 3000);
  CHECK(x.m.errors_order == 0 && x.s.errors_order == 0);
  CHECK(x.m.l.st.resyncs >= 4);  /* first contact + 3 restarts */
  report("panel restarts x3 (new epochs)", &x);

  /* the host restarts */
  sim y;
  sim_init(&y, 512, 120);
  run(&y, 3000);
  end_restart(&y.m, 99);
  y.s.want_any = 1;
  run(&y, 3000);
  CHECK(y.m.errors_order == 0 && y.s.errors_order == 0 && y.s.delivered > 1000);
  report("host restart", &y);

  /* an epoch collision: the panel restarts with the SAME epoch (sequence back at 0) */
  sim z;
  sim_init(&z, 512, 120);
  run(&z, 3000);
  uint32_t before = z.m.delivered;
  end_restart(&z.s, 22);
  z.m.want_any = 1;
  run(&z, 3000);
  CHECK(z.m.errors_order == 0 && z.s.errors_order == 0);
  CHECK(z.m.delivered > before + 1000);  /* recovered: the stream continues */
  report("panel restart, same epoch", &z);
}

/* Each recovery mechanism on its own (so a backup mechanism cannot hide a broken one). */
static void test_mechanisms(void) {
  /* NAK-driven go-back alone: the no-progress resend effectively disabled */
  sim x;
  sim_init(&x, 512, 120);
  x.m.cfg.retx_after = x.s.cfg.retx_after = 1000000;
  x.m.l.cfg.retx_after = x.s.l.cfg.retx_after = 1000000;
  x.f.flip_mosi = x.f.flip_miso = 0.05;
  run(&x, 10000);
  CHECK(x.m.errors_order == 0 && x.s.errors_order == 0);
  CHECK(x.m.delivered > 20000 && x.s.delivered > 20000);  /* NAKs alone keep it moving */
  report("NAK go-back only (no timeout resend)", &x);

  /* the same data frame 20 times: delivered once, recognised as duplicates, no resync */
  spl_link a, b;
  spl_config c = {256, 4, 1}, d = {256, 4, 2};
  uint8_t atx[1024], arx[1024], afl[SPL_WINDOW * 256], btx[1024], brx[1024], bfl[SPL_WINDOW * 256];
  uint8_t fa[256], fb[256], m[8] = "message", out[256];
  CHECK(spl_init(&a, &c, atx, sizeof(atx), arx, sizeof(arx), afl) == 0);
  CHECK(spl_init(&b, &d, btx, sizeof(btx), brx, sizeof(brx), bfl) == 0);
  spl_build(&b, fb);
  spl_accept(&a, fb);  /* a learns b's credit */
  CHECK(spl_send(&a, m, sizeof(m)) == 0);
  spl_build(&a, fa);
  CHECK(fa[10] > 0);   /* a data frame */
  for (int i = 0; i < 20; ++i) spl_accept(&b, fa);
  CHECK(spl_recv(&b, out, sizeof(out)) == sizeof(m) && spl_recv(&b, out, sizeof(out)) == 0);
  CHECK(b.st.duplicates == 19 && b.st.resyncs == 1 && b.st.out_of_order == 0);

  /* a restart with a new epoch is recognised on the first frame (no unplaced frames) */
  sim y;
  sim_init(&y, 512, 120);
  run(&y, 2000);
  uint32_t ooo = y.m.l.st.out_of_order, rs = y.m.l.st.resyncs;
  end_restart(&y.s, 77);
  y.m.want_any = 1;
  run(&y, 2000);
  CHECK(y.m.errors_order == 0 && y.s.errors_order == 0);
  CHECK(y.m.l.st.resyncs == rs + 1 && y.m.l.st.out_of_order == ooo);
}

static void test_master_state(void) {
  spl_master m;
  spl_master_init(&m, 1000, 0);
  CHECK(!m.up);
  CHECK(spl_master_ready(&m, 0, 10) == 0 && m.ready_waits == 1 && m.transfers == 0);
  CHECK(spl_master_ready(&m, 1, 20) == 1);
  spl_master_done(&m, 1, 20);
  CHECK(m.up && m.ups == 1);
  spl_master_done(&m, 0, 900);  /* bad frames for a while */
  CHECK(m.up);
  CHECK(spl_master_ready(&m, 0, 1021) == 0 && !m.up && m.downs == 1);  /* 1 s without a good frame */
  spl_master_done(&m, 1, 1500);
  CHECK(m.up && m.ups == 2);
  /* in the simulator: READY held low for a long time -> down, then up again */
  sim x;
  sim_init(&x, 256, 60);
  run(&x, 500);
  CHECK(x.mst.up);
  x.f.ready_low = 1.0;
  run(&x, 1500);
  CHECK(!x.mst.up && x.mst.downs == 1);
  x.f.ready_low = 0;
  run(&x, 500);
  CHECK(x.mst.up && x.m.errors_order == 0 && x.s.errors_order == 0);
}

int main(void) {
  test_codec();
  test_malformed_payload();
  test_clean();
  test_faults();
  test_slow_consumer();
  test_restarts();
  test_mechanisms();
  test_master_state();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("SPI link tests OK");
  return 0;
}
