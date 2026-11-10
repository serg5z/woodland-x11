/* shot/shot_msg.c: the screenshot messages, panel side to host side, with faults. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shot/shot_msg.h"

#define W 800
#define H 480
#define TOTAL (W * H * 2)

static uint8_t img[TOTAL], got[TOTAL];
static uint8_t msgs[200][SHOT_DT_HDR + SHOT_CHUNK];
static size_t lens[200];
static int nmsg;

static shot_info info_for(uint32_t id) {
  shot_info i = {id, SHOT_ST_OK, SHOT_FORMAT_RGB565LE, W, H, W * 2, TOTAL, SHOT_CHUNK,
                 (TOTAL + SHOT_CHUNK - 1) / SHOT_CHUNK, 31000, 77};
  return i;
}

static void make_capture(uint32_t id) {  /* every message of one capture */
  shot_tx t;
  shot_info i = info_for(id);
  shot_tx_start(&t, &i, img);
  nmsg = 0;
  size_t n;
  while ((n = shot_tx_peek(&t, msgs[nmsg], sizeof(msgs[0]))) > 0) {
    lens[nmsg++] = n;
    shot_tx_commit(&t);
  }
}

/* feeds messages [0, count) with an optional skip/dup/swap; returns the last result */
static int feed(shot_rx *r, int skip, int dup, int swap) {
  int res = SHOT_RX_MORE;
  for (int k = 0; k < nmsg && res == SHOT_RX_MORE; ++k) {
    int j = k;
    if (k == skip) continue;
    if (swap >= 0 && k == swap) j = k + 1;
    else if (swap >= 0 && k == swap + 1) j = k - 1;
    res = shot_rx_feed(r, msgs[j], lens[j]);
    if (k == dup && res == SHOT_RX_MORE) res = shot_rx_feed(r, msgs[j], lens[j]);
  }
  return res;
}

static void expect_error(shot_rx *r, int res, const char *want) {
  if (res != SHOT_RX_ERROR || !r->error || !strstr(r->error, want)) {
    fprintf(stderr, "want error '%s', got %d '%s' (%s)\n", want, res, r->error ? r->error : "-", r->detail);
    exit(1);
  }
}

int main(void) {
  for (size_t i = 0; i < TOTAL; ++i) img[i] = (uint8_t)(i * 7 + (i >> 11));
  /* the CRC is zlib's */
  assert(shot_crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
  assert(shot_crc32_update(shot_crc32_update(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) == 0xCBF43926u);

  /* request */
  uint8_t rq[SHOT_RQ_LEN];
  uint32_t id = 0;
  assert(shot_build_rq(rq, 0xA1B2C3D4u, 0) == SHOT_RQ_LEN && shot_kind(rq, SHOT_RQ_LEN, &id) == SHOT_RQ && id == 0xA1B2C3D4u);
  assert(shot_kind((const uint8_t *)"\x05\0\0\0hello world", 15, &id) == SHOT_NONE);

  /* a clean capture: IN, 192 DT, EN; the image arrives exactly */
  make_capture(0x1111u);
  assert(nmsg == 1 + 192 + 1 && lens[0] == SHOT_IN_LEN && lens[1] == SHOT_DT_HDR + SHOT_CHUNK && lens[193] == SHOT_EN_LEN);
  shot_rx r;
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  assert(feed(&r, -1, -1, -1) == SHOT_RX_DONE && !memcmp(got, img, TOTAL) && r.crc == shot_crc32_update(0, img, TOTAL));
  assert(r.info.width == W && r.info.height == H && r.info.capture_us == 31000 && r.info.fb_rev == 77 && r.data_msgs == 192);

  /* faults after the info */
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, 50, -1, -1), "missing");
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, -1, 50, -1), "duplicate");
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, -1, -1, 50), "missing");  /* 51 arrives where 50 was due */
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, 192, -1, -1), "truncated");  /* the last chunk lost, then END */
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  assert(feed(&r, 193, -1, -1) == SHOT_RX_MORE && r.next == 192 && r.state == 1);  /* no END: incomplete */

  make_capture(0x1111u);  /* a wrong offset (a panel bug) */
  msgs[10][16] ^= 0x01;
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, -1, -1, -1), "offset");

  make_capture(0x1111u);  /* a wrong length */
  lens[10] -= 2;
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, -1, -1, -1), "length");

  make_capture(0x1111u);  /* a corrupted pixel byte (frame CRC passed: a panel or memory fault) */
  msgs[100][SHOT_DT_HDR + 123] ^= 0x40;
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, -1, -1, -1), "CRC-32");

  make_capture(0x1111u);  /* an END that claims another size */
  msgs[193][16] ^= 0x10;
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, feed(&r, -1, -1, -1), "size mismatch");

  /* leftovers before the info: an earlier capture's chunks (in their order) and echo messages */
  make_capture(0x2222u);
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  for (int k = 3; k < 7; ++k) assert(shot_rx_feed(&r, msgs[k], lens[k]) == SHOT_RX_MORE);
  uint8_t echo[40] = {9, 0, 0, 0};
  assert(shot_rx_feed(&r, echo, sizeof(echo)) == SHOT_RX_MORE && r.leftovers == 5);
  make_capture(0x1111u);
  assert(feed(&r, -1, -1, -1) == SHOT_RX_DONE && !memcmp(got, img, TOTAL));

  /* an earlier capture's chunks out of their order */
  make_capture(0x2222u);
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  assert(shot_rx_feed(&r, msgs[3], lens[3]) == SHOT_RX_MORE);
  expect_error(&r, shot_rx_feed(&r, msgs[5], lens[5]), "out of order");

  /* more leftovers than a panel can hold */
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  int res = SHOT_RX_MORE;
  for (int k = 1; k < 30 && res == SHOT_RX_MORE; ++k) res = shot_rx_feed(&r, msgs[k], lens[k]);
  expect_error(&r, res, "more leftovers");

  /* another capture's message after this one's info */
  make_capture(0x1111u);
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  for (int k = 0; k < 10; ++k) assert(shot_rx_feed(&r, msgs[k], lens[k]) == SHOT_RX_MORE);
  uint8_t other[SHOT_DT_HDR + SHOT_CHUNK];
  memcpy(other, msgs[10], lens[10]);
  other[8] ^= 0xFF;  /* another id */
  expect_error(&r, shot_rx_feed(&r, other, lens[10]), "another capture");
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  for (int k = 0; k < 10; ++k) assert(shot_rx_feed(&r, msgs[k], lens[k]) == SHOT_RX_MORE);
  expect_error(&r, shot_rx_feed(&r, echo, sizeof(echo)), "non-screenshot");

  /* this capture's data before its info */
  shot_rx_init(&r, 0x1111u, got, sizeof(got));
  expect_error(&r, shot_rx_feed(&r, msgs[1], lens[1]), "before this capture's info");

  /* the panel could not capture: IN only, with the status */
  {
    shot_tx t;
    shot_info i = info_for(0x3333u);
    i.status = SHOT_ST_NO_MEMORY;
    shot_tx_start(&t, &i, NULL);
    uint8_t m[64];
    size_t n = shot_tx_peek(&t, m, sizeof(m));
    assert(n == SHOT_IN_LEN);
    shot_tx_commit(&t);
    assert(!t.active && shot_tx_peek(&t, m, sizeof(m)) == 0);
    shot_rx_init(&r, 0x3333u, got, sizeof(got));
    expect_error(&r, shot_rx_feed(&r, m, n), "could not capture");
  }

  /* an image bigger than the receive buffer */
  make_capture(0x1111u);
  shot_rx_init(&r, 0x1111u, got, 1000);
  expect_error(&r, shot_rx_feed(&r, msgs[0], lens[0]), "larger than the buffer");

  /* a send that fails is retried: peek again without commit gives the same message */
  {
    shot_tx t;
    shot_info i = info_for(0x4444u);
    shot_tx_start(&t, &i, img);
    static uint8_t a[SHOT_DT_HDR + SHOT_CHUNK], b[SHOT_DT_HDR + SHOT_CHUNK];
    shot_tx_commit(&t);  /* IN sent */
    size_t na = shot_tx_peek(&t, a, sizeof(a)), nb = shot_tx_peek(&t, b, sizeof(b));
    assert(na == nb && !memcmp(a, b, na));
    assert(shot_tx_peek(&t, a, 100) == 0);  /* no room: nothing built */
  }
  printf("test_shot: all OK\n");
  return 0;
}
