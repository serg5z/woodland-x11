/* Unit tests of ota/: SHA-256 (FIPS 180-2 vectors), HMAC-SHA256 (RFC 4231), the image check,
 * and the engine's request authentication and marker search with a fake platform. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ota/x11_ota.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(const uint8_t *h, char *out) {
  for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

static void test_sha256(void) {
  uint8_t h[32];
  char s[65];
  x11_sha256("abc", 3, h);
  hex(h, s);
  CHECK(!strcmp(s, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  x11_sha256("", 0, h);
  hex(h, s);
  CHECK(!strcmp(s, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  x11_sha256(m, strlen(m), h);
  hex(h, s);
  CHECK(!strcmp(s, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  /* one million 'a', fed in odd-sized pieces */
  x11_sha256_ctx c;
  x11_sha256_init(&c);
  static char a[1000];
  memset(a, 'a', sizeof(a));
  for (int i = 0; i < 1000; ++i) {
    x11_sha256_update(&c, a, 337);
    x11_sha256_update(&c, a, 663);
  }
  x11_sha256_final(&c, h);
  hex(h, s);
  CHECK(!strcmp(s, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_hmac(void) {
  uint8_t key[20], h[32];
  char s[65];
  memset(key, 0x0b, sizeof(key));  /* RFC 4231 test case 1 */
  x11_hmac_sha256(key, 20, "Hi ", 3, "There", 5, NULL, 0, h);
  hex(h, s);
  CHECK(!strcmp(s, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
  x11_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want ", 16, "for nothing?", 12, NULL, 0, h);  /* case 2 */
  hex(h, s);
  CHECK(!strcmp(s, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
  uint8_t big[131];
  memset(big, 0xaa, sizeof(big));  /* case 6: key longer than the block */
  const char *m = "Test Using Larger Than Block-Size Key - Hash Key First";
  x11_hmac_sha256(big, sizeof(big), m, strlen(m), NULL, 0, NULL, 0, h);
  hex(h, s);
  CHECK(!strcmp(s, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
  CHECK(x11_ct_equal((const uint8_t *)"abcd", (const uint8_t *)"abcd", 4));
  CHECK(!x11_ct_equal((const uint8_t *)"abcd", (const uint8_t *)"abce", 4));
}

/* a minimal valid image: header, one segment holding an app descriptor, padding, checksum, hash */
static size_t make_image(uint8_t *img, size_t payload, const char *idf, int marker) {
  memset(img, 0, 24);
  img[0] = 0xE9, img[1] = 1, img[12] = 9, img[23] = 1;
  size_t seg = 256 + payload;
  img[24 + 4] = (uint8_t)seg, img[24 + 5] = (uint8_t)(seg >> 8), img[24 + 6] = (uint8_t)(seg >> 16);
  uint8_t *d = img + 32;
  memset(d, 0, seg);
  d[0] = 0x32, d[1] = 0x54, d[2] = 0xCD, d[3] = 0xAB;
  strcpy((char *)d + 112, idf);
  for (size_t i = 256; i < seg; ++i) d[i] = (uint8_t)(i * 7);
  if (marker) memcpy(d + 256 + payload / 2, X11_OTA_MARKER, strlen(X11_OTA_MARKER));
  size_t off = 32 + seg;
  uint8_t ck = 0xEF;
  for (size_t i = 0; i < seg; ++i) ck ^= d[i];
  size_t ck_at = ((off + 1 + 15) & ~(size_t)15) - 1;
  memset(img + off, 0, ck_at - off);
  img[ck_at] = ck;
  x11_sha256(img, ck_at + 1, img + ck_at + 1);
  return ck_at + 1 + 32;
}

static void test_image_check(void) {
  static uint8_t img[8192];
  size_t n = make_image(img, 4000, "v5.5.5", 1);
  CHECK(x11_ota_image_check(img, n) == 0);
  CHECK(x11_ota_image_check(img, n - 1) != 0);  /* truncated */
  img[1000] ^= 1;
  CHECK(x11_ota_image_check(img, n) != 0);       /* data changed: checksum and hash */
  img[1000] ^= 1;
  img[12] = 2;
  CHECK(x11_ota_image_check(img, n) != 0);       /* another chip */
  img[12] = 9;
  img[0] = 0xE8;
  CHECK(x11_ota_image_check(img, n) != 0);       /* magic */
}

/* ---- the engine with a fake platform ---- */

static uint8_t flash_slot[65536], block_buf[X11_OTA_MAX_BLOCK];
static size_t began, aborted, activated, sectors_written, erased[16];
static int fail_sector = -1;
static void f_slots(void *c, x11_ota_slots *o) { (void)c; o->has_inactive = 1, o->inactive_size = 65536, o->running = 0; strcpy(o->text, "fake"); }
static int f_begin(void *c, uint32_t n) { (void)c; (void)n; ++began; memset(erased, 0, sizeof(erased)); memset(flash_slot, 0xA5, sizeof(flash_slot)); return 0; }
static int f_write_sector(void *c, uint32_t off, const uint8_t *d, size_t n, uint32_t *e, uint32_t *p) {
  (void)c;
  if ((int)(off / 4096) == fail_sector) return -1;
  if (off % 4096 || n > 4096) return -1;
  ++erased[off / 4096];
  memset(flash_slot + off, 0xFF, 4096);
  memcpy(flash_slot + off, d, n);
  ++sectors_written;
  *e = 1000, *p = 500;
  return 0;
}
static int f_finish(void *c, uint32_t n) { (void)c; return x11_ota_image_check(flash_slot, n); }
static void f_abort(void *c) { (void)c; ++aborted; }
static int f_activate(void *c) { (void)c; ++activated; return 0; }
static int f_other(void *c) { (void)c; return -1; }
static void f_reboot(void *c) { (void)c; }
static void f_random(void *c, uint8_t *o, size_t n) { (void)c; memset(o, 0x5a, n); }
static uint32_t t_now = 1000;
static uint32_t f_now(void *c) { (void)c; return t_now; }
static const char *f_idf(void *c) { (void)c; return "v5.5.5"; }
static const x11_ota_ops OPS = {f_slots, f_begin, f_write_sector, f_finish, f_abort, f_activate, f_other, f_reboot,
                                f_random, f_now, NULL, f_idf, block_buf, NULL};

static void make_request(uint8_t *q, const uint8_t key[32], const uint8_t nonce[16], uint8_t cmd, const uint8_t *img,
                         uint32_t n, uint32_t block) {
  memset(q, 0, X11_OTA_REQUEST_LEN);
  memcpy(q, "WLOTA2RQ", 8);
  q[8] = cmd;
  memset(q + 12, 0x11, 16);
  q[28] = (uint8_t)n, q[29] = (uint8_t)(n >> 8), q[30] = (uint8_t)(n >> 16);
  q[32] = (uint8_t)block, q[33] = (uint8_t)(block >> 8), q[34] = (uint8_t)(block >> 16);
  if (img) x11_sha256(img, n, q + 36);
  strcpy((char *)q + 68, X11_OTA_MARKER);
  strcpy((char *)q + 100, "v5.5.5");
  x11_hmac_sha256(key, 32, nonce, 16, q, 160, NULL, 0, q + 160);
}

static size_t block_header(uint8_t *h, uint32_t seq, uint32_t off, uint32_t len) {
  memcpy(h, "WLB2", 4);
  for (int i = 0; i < 4; ++i) h[4 + i] = (uint8_t)(seq >> 8 * i), h[8 + i] = (uint8_t)(off >> 8 * i), h[12 + i] = (uint8_t)(len >> 8 * i);
  return 16;
}

static uint32_t rd(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Runs the engine until it wants input again; returns the replies produced. */
static size_t steps(x11_ota_session *s, uint8_t *out, size_t cap) {
  size_t got = 0;
  while (x11_ota_busy(s) && got == 0) got = x11_ota_step(s, out, cap);
  return got;
}

static void test_engine(void) {
  uint8_t key[32], bad[32], out[512], q[X11_OTA_REQUEST_LEN], h[16], cn[16], mac[32];
  static uint8_t img[40000];
  memset(key, 7, 32), memset(bad, 8, 32), memset(cn, 0x11, 16);
  size_t n = make_image(img, 30000, "v5.5.5", 1);  /* ~30 KB: 2 blocks of 16 KiB */
  x11_ota_endpoint ep;
  x11_ota_endpoint_init(&ep, key);
  x11_ota_session s;
  size_t used;
  /* a good upload, 16 KiB blocks, every byte fed alone */
  CHECK(x11_ota_open(&s, &ep, &OPS, out, sizeof(out)) == X11_OTA_HELLO_LEN && !memcmp(out, "WLOTA2\r\n", 8));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  CHECK(x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out)) == X11_OTA_REPLY_LEN && out[8] == X11_OTA_OK && out[63] == 0);
  uint32_t off = 0, seq = 0;
  while (off < n) {
    uint32_t len = n - off < 16384 ? (uint32_t)(n - off) : 16384;
    block_header(h, seq, off, len);
    for (int i = 0; i < 16; ++i) CHECK(x11_ota_input(&s, h + i, 1, &used, out, sizeof(out)) == 0 && used == 1);
    for (uint32_t i = 0; i < len; ++i) CHECK(x11_ota_input(&s, img + off + i, 1, &used, out, sizeof(out)) == 0 && used == 1);
    CHECK(x11_ota_busy(&s));                              /* committing: no input taken now */
    size_t before = sectors_written;
    size_t r = steps(&s, out, sizeof(out));
    CHECK(sectors_written - before == (len + 4095) / 4096);  /* one step per sector */
    CHECK(r >= X11_OTA_REPLY_LEN && !memcmp(out, "WLOTA2AK", 8));
    CHECK(rd(out + 8) == X11_OTA_OK && rd(out + 12) == seq && rd(out + 16) == off + len && out[63] == seq + 1);
    CHECK(rd(out + 28) == 1000u * ((len + 4095) / 4096) && rd(out + 32) == 500u * ((len + 4095) / 4096));
    x11_hmac_sha256(key, 32, cn, 16, s.server_nonce, 16, out, 64, mac);
    CHECK(!memcmp(mac, out + 64, 32));                    /* the ACK is bound to the session and its fields */
    off += len, ++seq;
    if (off == n) CHECK(r == 2 * X11_OTA_REPLY_LEN && !memcmp(out + 96, "WLOTA2RS", 8) && rd(out + 104) == X11_OTA_OK);
  }
  CHECK(!memcmp(flash_slot, img, n) && activated == 1 && aborted == 0);
  for (int i = 0; i < 8; ++i) CHECK(erased[i] == (i < (int)((n + 4095) / 4096) ? 1u : 0u));  /* each sector erased once */
  CHECK(ep.up.blocks == 2 && ep.up.committed == n && ep.up.sectors == (n + 4095) / 4096);

  /* wrong key: refused before anything is written */
  began = 0;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, bad, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  CHECK(rd(out + 8) == X11_OTA_AUTH_FAILED && began == 0);
  /* a protocol-1 request is refused without looking at its MAC */
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_STATUS, NULL, 0, 16384);
  memcpy(q, "WLOTA1RQ", 8);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  CHECK(rd(out + 8) == X11_OTA_BAD_REQUEST && rd(out + 12) == 1);
  /* unsupported block size */
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 8192);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  CHECK(rd(out + 8) == X11_OTA_BAD_REQUEST);

  /* block errors: each ends the session without activating, the upload aborted */
  struct { uint32_t seq, off, len; const char *magic; } cases[] = {
      {1, 0, 16384, "WLB2"},       /* out of order */
      {0, 4096, 16384, "WLB2"},    /* wrong offset */
      {0, 0, 4096, "WLB2"},        /* wrong length */
      {0, 0, 16384, "XLB2"},       /* malformed */
  };
  for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    activated = aborted = 0;
    x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
    make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
    x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
    block_header(h, cases[c].seq, cases[c].off, cases[c].len);
    memcpy(h, cases[c].magic, 4);
    x11_ota_input(&s, h, 16, &used, out, sizeof(out));
    CHECK(x11_ota_done(&s) && rd(out + 8) == X11_OTA_BAD_REQUEST && aborted == 1 && activated == 0);
  }
  /* a duplicate block (block 0 again after its ACK) */
  activated = aborted = 0;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  block_header(h, 0, 0, 16384);
  x11_ota_input(&s, h, 16, &used, out, sizeof(out));
  x11_ota_input(&s, img, 16384, &used, out, sizeof(out));
  steps(&s, out, sizeof(out));
  x11_ota_input(&s, h, 16, &used, out, sizeof(out));
  CHECK(rd(out + 8) == X11_OTA_BAD_REQUEST && !strcmp((char *)out + 16, "duplicate block") && aborted == 1 && activated == 0);
  /* data sent before the ACK (in the same chunk as the block's end) */
  aborted = 0;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 4096);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  block_header(h, 0, 0, 4096);
  x11_ota_input(&s, h, 16, &used, out, sizeof(out));
  x11_ota_input(&s, img, 4096 + 16, &used, out, sizeof(out));
  CHECK(rd(out + 8) == X11_OTA_BAD_REQUEST && aborted == 1);
  /* a failed flash write */
  activated = aborted = 0;
  fail_sector = 1;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  block_header(h, 0, 0, 16384);
  x11_ota_input(&s, h, 16, &used, out, sizeof(out));
  x11_ota_input(&s, img, 16384, &used, out, sizeof(out));
  steps(&s, out, sizeof(out));
  CHECK(!memcmp(out, "WLOTA2RS", 8) && rd(out + 8) == X11_OTA_FLASH_ERROR && aborted == 1 && activated == 0);
  fail_sector = -1;
  /* a disconnect in the middle of a block */
  aborted = 0;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  x11_ota_input(&s, h, 16, &used, out, sizeof(out));
  x11_ota_input(&s, img, 1000, &used, out, sizeof(out));
  x11_ota_closed(&s);
  CHECK(aborted == 1 && ep.aborted >= 1);
  /* silence before the next block: timeout, abort */
  aborted = 0;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  t_now += X11_OTA_DATA_TIMEOUT_MS + 1;
  CHECK(x11_ota_step(&s, out, sizeof(out)) == X11_OTA_REPLY_LEN && rd(out + 8) == X11_OTA_TIMEOUT && aborted == 1);
  /* a wrong image hash: everything committed, then refused, nothing activated */
  activated = aborted = 0;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  q[40] ^= 1;  /* the manifest's hash (re-MAC'd: an authenticated but wrong hash) */
  x11_hmac_sha256(key, 32, s.server_nonce, 16, q, 160, NULL, 0, q + 160);
  x11_ota_input(&s, q, sizeof(q), &used, out, sizeof(out));
  for (uint32_t o = 0, sq = 0; o < n; o += 16384, ++sq) {
    uint32_t len = n - o < 16384 ? (uint32_t)(n - o) : 16384;
    block_header(h, sq, o, len);
    x11_ota_input(&s, h, 16, &used, out, sizeof(out));
    x11_ota_input(&s, img + o, len, &used, out, sizeof(out));
    steps(&s, out, sizeof(out));
  }
  CHECK(!memcmp(out + 96, "WLOTA2RS", 8) && rd(out + 104) == X11_OTA_HASH_MISMATCH && activated == 0 && aborted == 1);
}

/* ---- the shared transport loop (the M3 regression: 4-byte reads) ---- */

static uint8_t wire[70000];
static size_t wire_len, wire_pos, max_read, min_read = (size_t)-1, reads, reads_while_busy;
static x11_ota_session *pump_sess;
static uint8_t sent[4096];
static size_t sent_len;
static long p_recv(void *c, uint8_t *buf, size_t n) {
  (void)c;
  if (x11_ota_busy(pump_sess)) ++reads_while_busy;
  if (wire_pos == wire_len) return -1;
  ++reads;
  if (n > max_read) max_read = n;
  if (n < min_read) min_read = n;
  size_t k = wire_len - wire_pos < n ? wire_len - wire_pos : n;
  memcpy(buf, wire + wire_pos, k);
  wire_pos += k;
  return (long)k;
}
static long p_send(void *c, const uint8_t *b, size_t n) {
  (void)c;
  if (sent_len + n <= sizeof(sent)) memcpy(sent + sent_len, b, n);
  sent_len += n;
  return (long)n;
}

static void test_pump(void) {
  uint8_t key[32], out[512], q[X11_OTA_REQUEST_LEN], h[16];
  static uint8_t img[20000], rbuf[4096];
  memset(key, 7, 32);
  size_t n = make_image(img, 15000, "v5.5.5", 1);  /* one 16 KiB block */
  x11_ota_endpoint ep;
  x11_ota_endpoint_init(&ep, key);
  x11_ota_session s;
  pump_sess = &s;
  x11_ota_open(&s, &ep, &OPS, out, sizeof(out));
  make_request(q, key, s.server_nonce, X11_OTA_CMD_UPLOAD, img, (uint32_t)n, 16384);
  memcpy(wire, q, sizeof(q));
  wire_len = sizeof(q);
  x11_ota_io io = {p_recv, p_send, NULL, rbuf, sizeof(rbuf), 4};
  CHECK(x11_ota_pump(&s, &io) == 1 && sent_len == X11_OTA_REPLY_LEN);  /* the OK */
  block_header(h, 0, 0, (uint32_t)n);
  memcpy(wire + wire_len, h, 16);
  memcpy(wire + wire_len + 16, img, n);
  wire_len += 16 + n;
  int passes = 0;
  while (s.state != X11_OTA_S_DONE && passes < 100) {
    x11_ota_pump(&s, &io);
    ++passes;
  }
  CHECK(min_read == sizeof(rbuf) && max_read == sizeof(rbuf));  /* always the whole buffer, never 4 bytes */
  CHECK(reads <= 2 + (16 + n + sizeof(rbuf) - 1) / sizeof(rbuf));
  CHECK(reads_while_busy == 0);                                  /* never read during a commit */
  CHECK(passes <= 2 + (int)((n + 4095) / 4096));                 /* receive in one pass, one pass per sector */
  CHECK(sent_len == 3 * X11_OTA_REPLY_LEN && !memcmp(sent + 96, "WLOTA2AK", 8) && !memcmp(sent + 192, "WLOTA2RS", 8));
  CHECK(sent[200] == X11_OTA_OK);
}

/* The trial decision (M4): confirm after 60 s of continuous health, roll back at the 180 s
 * deadline when a previous image exists; the no-confirm test build never confirms. */
static void test_trial_step(void) {
  uint32_t hs = 0;
  enum { W = X11_OTA_TRIAL_WAIT, C = X11_OTA_TRIAL_CONFIRM, R = X11_OTA_TRIAL_ROLLBACK };
  /* normal build: healthy from 5 s */
  CHECK(x11_ota_trial_step(0, 1000, &hs, 1, 0) == W && hs == 0);
  CHECK(x11_ota_trial_step(1, 5000, &hs, 1, 0) == W && hs == 5000);
  CHECK(x11_ota_trial_step(1, 64999, &hs, 1, 0) == W);
  CHECK(x11_ota_trial_step(1, 65000, &hs, 1, 0) == C);
  /* a health interruption restarts the 60 s */
  hs = 0;
  CHECK(x11_ota_trial_step(1, 10000, &hs, 1, 0) == W);
  CHECK(x11_ota_trial_step(0, 50000, &hs, 1, 0) == W && hs == 0);
  CHECK(x11_ota_trial_step(1, 60000, &hs, 1, 0) == W && hs == 60000);
  CHECK(x11_ota_trial_step(1, 119999, &hs, 1, 0) == W);
  CHECK(x11_ota_trial_step(1, 120000, &hs, 1, 0) == C);
  /* healthy too late: the deadline wins */
  hs = 0;
  CHECK(x11_ota_trial_step(1, 150000, &hs, 1, 0) == W);
  CHECK(x11_ota_trial_step(1, 180000, &hs, 1, 0) == R);
  /* never healthy: rollback at the deadline, but only if a previous image can take over */
  hs = 0;
  CHECK(x11_ota_trial_step(0, 179999, &hs, 1, 0) == W);
  CHECK(x11_ota_trial_step(0, 180000, &hs, 1, 0) == R);
  CHECK(x11_ota_trial_step(0, 180000, &hs, 0, 0) == W);
  /* the M4 test build: healthy the whole time, never confirms, leaves at the deadline */
  hs = 0;
  int confirmed = 0;
  for (uint32_t t = 1000; t < 180000; t += 250) confirmed |= x11_ota_trial_step(1, t, &hs, 1, 1) == C;
  CHECK(!confirmed);
  CHECK(x11_ota_trial_step(1, 180000, &hs, 1, 1) == R);
  CHECK(x11_ota_trial_step(1, 180000, &hs, 0, 1) == W);  /* no fallback: it stays (and stays unconfirmed) */
  /* millis() wrap is not reached in a trial (49 days); timestamps are monotonic here */
}

int main(void) {
  test_sha256();
  test_hmac();
  test_image_check();
  test_engine();
  test_pump();
  test_trial_step();
  if (failures) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  puts("x11 OTA tests OK");
  return 0;
}
