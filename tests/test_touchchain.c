/* The multi-touch chain (mt/x11_touchchain) with simulated GT9xx frames: 1 to 5 fingers by
 * hardware track ID, changing report order, missing and failed frames, malformed reports,
 * the coordinate transform (calibration, rotations, bounds), and through the real
 * single-touch filter and X server: single-finger behaviour, gestures, no stuck buttons.
 * Run with ASan/UBSan. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../core/x11_internal.h"
#include "../input/x11_touch.h"
#include "../mt/x11_touchchain.h"

static int failures = 0;
#define CHECK(c)                                                             \
  do {                                                                       \
    if (!(c)) {                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static x11_tc tc;
static x11_touch ptr;
static uint32_t now;
static int b1_press, b1_release, last_down, press_x, press_y;

static void fresh_cfg(const x11_tc_config *c) {
  x11_tc_init(&tc, c);
  x11_touch_config pc;
  x11_touch_default_config(&pc, c->width, c->height);  /* identity: the chain already mapped */
  x11_touch_init(&ptr, &pc);
  now = 1000, b1_press = b1_release = last_down = 0;
}
static void fresh(void) {
  x11_tc_config c;
  x11_tc_default_config(&c, 800, 480);
  fresh_cfg(&c);
}

/* One frame after dt ms: n points (id, x, y). n < 0: a failed read. */
static int frame_at(uint32_t dt, int n, ...) {
  x11_mt_point p[8];
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n && i < 8; ++i) {
    p[i].id = (uint8_t)va_arg(ap, int);
    p[i].x = (int16_t)va_arg(ap, int);
    p[i].y = (int16_t)va_arg(ap, int);
    p[i].size = 20;
  }
  va_end(ap);
  now += dt;
  int x = 0, y = 0;
  int pts = x11_tc_frame(&tc, now, n >= 0, p, n < 0 ? 0 : n, &x, &y);
  x11_touch_sample(&ptr, now, 1, pts, x, y);
  if (ptr.down && !last_down) ++b1_press, press_x = ptr.x, press_y = ptr.y;
  if (!ptr.down && last_down) ++b1_release;
  last_down = ptr.down;
  return x11_tc_count(&tc);
}
#define frame(...) frame_at(15, __VA_ARGS__)
static void idle(int n) {
  for (int i = 0; i < n; ++i) frame(0);
}
static int count_kind(int kind) {
  int k = 0;
  uint32_t first = tc.log_total > X11_TC_LOG ? tc.log_total - X11_TC_LOG : 0;
  for (uint32_t i = first; i < tc.log_total; ++i) k += tc.log[i % X11_TC_LOG].kind == kind;
  return k;
}

/* Controller -> screen: identity, calibration, the three rotations, out of range. */
static void test_map(void) {
  x11_tc_config c;
  int x, y;
  x11_tc_default_config(&c, 800, 480);
  CHECK(x11_tc_map(&c, 0, 0, &x, &y) && x == 0 && y == 0);
  CHECK(x11_tc_map(&c, 799, 479, &x, &y) && x == 799 && y == 479);
  CHECK(!x11_tc_map(&c, 800, 10, &x, &y) && !x11_tc_map(&c, -1, 10, &x, &y) && !x11_tc_map(&c, 5, 480, &x, &y));
  c.cal_x0 = 20, c.cal_x1 = 779, c.cal_y0 = 10, c.cal_y1 = 469;  /* raw 20..779 is the whole width */
  CHECK(x11_tc_map(&c, 20, 10, &x, &y) && x == 0 && y == 0);
  CHECK(x11_tc_map(&c, 779, 469, &x, &y) && x == 799 && y == 479);
  CHECK(x11_tc_map(&c, 5, 475, &x, &y) && x == 0 && y == 479);    /* beyond the calibrated edge: clamped */
  CHECK(x11_tc_map(&c, 400, 240, &x, &y) && x >= 399 && x <= 401 && y >= 239 && y <= 241);
  /* a portrait controller (480 x 800) on the landscape screen */
  x11_tc_default_config(&c, 800, 480);
  c.raw_w = 480, c.raw_h = 800;
  c.rotation = 1;  /* 90 clockwise: controller top-left -> screen top-right */
  CHECK(x11_tc_map(&c, 0, 0, &x, &y) && x == 799 && y == 0);
  CHECK(x11_tc_map(&c, 479, 799, &x, &y) && x == 0 && y == 479);
  CHECK(x11_tc_map(&c, 0, 799, &x, &y) && x == 0 && y == 0);
  c.rotation = 3;  /* 270: controller top-left -> screen bottom-left */
  CHECK(x11_tc_map(&c, 0, 0, &x, &y) && x == 0 && y == 479);
  CHECK(x11_tc_map(&c, 479, 0, &x, &y) && x == 0 && y == 0);
  x11_tc_default_config(&c, 800, 480);
  c.rotation = 2;
  CHECK(x11_tc_map(&c, 0, 0, &x, &y) && x == 799 && y == 479);
  CHECK(x11_tc_map(&c, 799, 479, &x, &y) && x == 0 && y == 0);
  /* through the chain: a rotated frame lands where the map says */
  x11_tc_config r;
  x11_tc_default_config(&r, 800, 480);
  r.raw_w = 480, r.raw_h = 800, r.rotation = 1;
  fresh_cfg(&r);
  frame(1, 3, 100, 200);
  CHECK(tc.mt.c[0].x == 800 - 1 - 200 && tc.mt.c[0].y == 100);
  frame(1, 3, 600, 200);  /* x 600 is outside the portrait controller's 0..479 */
  CHECK(tc.out_of_range == 1);
}

/* 1 to 5 fingers by track ID, one more each frame, then lifted in another order: BEGIN and END
 * once each, with the frame number; counts follow; the pointer never presses (a second finger
 * came before the hold). */
static void test_one_to_five(void) {
  fresh();
  CHECK(frame(1, 7, 100, 100) == 1);
  CHECK(frame(2, 7, 100, 100, 2, 200, 100) == 2);
  CHECK(frame(3, 7, 100, 100, 2, 200, 100, 9, 300, 100) == 3);
  CHECK(frame(4, 7, 100, 100, 2, 200, 100, 9, 300, 100, 4, 400, 100) == 4);
  CHECK(frame(5, 7, 100, 100, 2, 200, 100, 9, 300, 100, 4, 400, 100, 11, 500, 100) == 5);
  CHECK(tc.mt.max_seen == 5 && tc.mt.by_count[5] == 1 && tc.mt.news == 5);
  CHECK(count_kind(X11_TC_BEGIN) == 5);
  CHECK(tc.log[0].frame == 1 && tc.log[0].id == 7 && tc.log[4].frame == 5 && tc.log[4].id == 11);
  /* the same five in another order every frame, moving: identity kept, reorders counted */
  frame(5, 11, 505, 110, 4, 405, 110, 9, 305, 110, 2, 205, 110, 7, 105, 110);
  frame(5, 9, 310, 120, 7, 110, 120, 11, 510, 120, 2, 210, 120, 4, 410, 120);
  CHECK(tc.mt.news == 5 && tc.mt.reorders >= 2 && x11_tc_count(&tc) == 5);
  for (int i = 0; i < X11_MT_MAX; ++i)  /* every slot kept its ID and followed it */
    CHECK(tc.mt.c[i].x == (tc.mt.c[i].id == 7 ? 110 : tc.mt.c[i].id == 2 ? 210 : tc.mt.c[i].id == 9 ? 310 :
                           tc.mt.c[i].id == 4 ? 410 : 510));
  /* lift 9, then 7 and 11, then the rest */
  frame(4, 11, 510, 120, 4, 410, 120, 2, 210, 120, 7, 110, 120);
  frame(4, 11, 510, 120, 4, 410, 120, 2, 210, 120, 7, 110, 120);
  CHECK(x11_tc_count(&tc) == 4);
  idle(3);
  CHECK(x11_tc_count(&tc) == 0 && count_kind(X11_TC_END) >= 3);
  CHECK(tc.log_total == 10);  /* 5 BEGIN + 5 END, nothing twice */
  CHECK(b1_press == 0 && b1_release == 0 && !ptr.down);
  CHECK(tc.gest.cancels >= 1);  /* the third finger ended the pair */
}

/* One missing report is not a lift; two are. Failed reads end contacts as LOST. */
static void test_missing_and_failed(void) {
  fresh();
  frame(1, 3, 100, 100);
  frame(0);                 /* one report without it */
  frame(1, 3, 101, 100);
  CHECK(x11_tc_count(&tc) == 1 && tc.mt.dropouts == 1 && count_kind(X11_TC_END) == 0);
  frame(0), frame(0);
  CHECK(x11_tc_count(&tc) == 0 && count_kind(X11_TC_END) == 1);
  frame(1, 5, 200, 200);
  frame(-1), frame(-1), frame(-1);  /* the controller stops answering */
  CHECK(x11_tc_count(&tc) == 0 && count_kind(X11_TC_LOST) == 1 && tc.failed_frames == 3);
  idle(10);
  CHECK(!ptr.down && b1_press == b1_release);
}

/* Malformed reports: an impossible count, IDs above 15, duplicate IDs, points outside the
 * controller; a sixth contact while five are down waits for a free slot. */
static void test_malformed(void) {
  fresh();
  frame(7, 1, 10, 10, 2, 20, 20, 3, 30, 30, 4, 40, 40, 5, 50, 50, 6, 60, 60, 7, 70, 70);
  CHECK(tc.mt.malformed == 1 && x11_tc_count(&tc) == 5);
  idle(3);
  frame(2, 16, 10, 10, 1, 20, 20);
  CHECK(tc.mt.malformed == 2 && x11_tc_count(&tc) == 1);
  frame(2, 1, 20, 20, 1, 30, 30);
  CHECK(tc.mt.duplicate_ids == 1);
  frame(2, 1, 20, 20, 2, 900, 30);
  CHECK(tc.out_of_range == 1 && x11_tc_count(&tc) == 1);
  idle(3);
  /* five down, then 1 lifts and 6 lands in the same report: 6 waits until 1 is released */
  frame(5, 1, 10, 10, 2, 20, 20, 3, 30, 30, 4, 40, 40, 5, 50, 50);
  frame(5, 6, 60, 60, 2, 20, 20, 3, 30, 30, 4, 40, 40, 5, 50, 50);
  CHECK(tc.mt.overflow == 1);
  frame(5, 6, 60, 60, 2, 20, 20, 3, 30, 30, 4, 40, 40, 5, 50, 50);  /* 1 released now */
  frame(5, 6, 60, 60, 2, 20, 20, 3, 30, 30, 4, 40, 40, 5, 50, 50);  /* 6 takes the slot */
  int has6 = 0;
  for (int i = 0; i < X11_MT_MAX; ++i) has6 |= tc.mt.c[i].id == 6 && tc.mt.c[i].state != X11_MT_FREE;
  CHECK(has6 && x11_tc_count(&tc) == 5);
  idle(5);
  CHECK(!ptr.down && b1_press == b1_release && x11_tc_count(&tc) == 0);
}

/* One finger behaves as before the chain (test_gate): a tap presses at the lift, 45 ms after the
 * landing; a held finger presses at the landing point 60 ms after it; a drag follows. */
static void test_single_finger_unchanged(void) {
  fresh();
  frame(1, 3, 300, 200);
  frame(0), frame(0), frame(0);  /* lifted after one report */
  CHECK(b1_press == 1 && press_x == 300 && press_y == 200);
  idle(6);
  CHECK(b1_release == 1);
  fresh();
  frame(1, 3, 300, 200);
  for (int i = 0; i < 3; ++i) frame(1, 3, 300 + 10 * i, 200);
  frame(1, 3, 340, 210);
  frame(1, 3, 360, 220);
  CHECK(b1_press == 1 && press_x == 300 && press_y == 200);  /* pressed where it landed */
  CHECK(ptr.down && ptr.x == 360 && ptr.y == 220);             /* then follows */
  idle(8);
  CHECK(!ptr.down && b1_release == 1);
}

/* Fingers joining and leaving during gestures, in every count from 1 to 5 and back, a few
 * times over: never a stuck button, presses = releases, nothing left down. */
static void test_join_leave_no_stuck(void) {
  fresh();
  for (int round = 0; round < 3; ++round) {
    frame(1, 1, 100, 300);
    for (int i = 0; i < 4; ++i) frame(1, 1, 100, 300 + 5 * i);  /* a drag: the pointer presses */
    frame(2, 1, 100, 320, 2, 200, 320);
    for (int i = 0; i < 4; ++i) frame(2, 1, 100, 320 + 20 * i, 2, 200, 320 + 20 * i);  /* a scroll */
    frame(3, 1, 100, 400, 2, 200, 400, 3, 300, 400);
    frame(5, 1, 100, 400, 2, 200, 400, 3, 300, 400, 4, 400, 400, 5, 500, 400);
    frame(4, 5, 500, 400, 3, 300, 400, 1, 100, 400, 4, 400, 400);
    frame(2, 4, 400, 400, 1, 100, 400);
    frame(1, 4, 400, 400);
    idle(8);
  }
  CHECK(x11_tc_count(&tc) == 0 && !ptr.down && b1_press == b1_release);
  CHECK(tc.gest.cancels >= 3);
}

/* Input audit B5 and B6 with the chain's defaults (hold 50 ms when moving, 150 ms still). */
static void test_audit_b5_b6(void) {
  /* B5: three fingers, lifted one by one: no Button3 (was a two-finger tap), no Button1 */
  fresh();
  frame(3, 1, 100, 300, 2, 300, 300, 3, 500, 300);
  frame(3, 1, 100, 300, 2, 300, 300, 3, 500, 300);
  frame(2, 1, 100, 300, 2, 300, 300);
  frame(1, 1, 100, 300);
  idle(8);
  CHECK(tc.gest.taps == 0 && tc.click_n == 0 && b1_press == 0 && !ptr.down);
  /* B6: the second finger lands 75-135 ms after the first (a still first finger), then a
   * two-finger tap or scroll: no Button1 (was a press/release before the gesture) */
  for (int lag = 5; lag <= 9; ++lag) {
    fresh();
    for (int i = 0; i < lag; ++i) frame(1, 1, 300, 200);
    frame(2, 1, 300, 200, 2, 500, 200), frame(2, 1, 300, 200, 2, 500, 200);
    frame(1, 2, 500, 200);
    idle(8);
    CHECK(b1_press == 0 && b1_release == 0 && tc.gest.taps == 1);
    fresh();
    for (int i = 0; i < lag; ++i) frame(1, 1, 300, 200);
    for (int i = 0; i <= 30; ++i) frame(2, 1, 300, 200 + 4 * i, 2, 500, 200 + 4 * i);
    frame(1, 2, 500, 320), idle(8);
    CHECK(b1_press == 0 && tc.gest.starts[X11_G_SCROLL_V] == 1);
  }
  /* a still finger presses after 150 ms, at the landing point; a lift before that is a tap */
  fresh();
  uint32_t t0 = now + 15;
  int pressed_at = 0;
  for (int i = 0; i < 20; ++i) {
    frame(1, 1, 400, 240);
    if (ptr.down && !pressed_at) pressed_at = (int)(now - t0);
  }
  CHECK(pressed_at >= 150 && pressed_at < 165 && press_x == 400 && press_y == 240);
  idle(8);
  CHECK(b1_press == 1 && b1_release == 1);
  fresh();
  for (int i = 0; i < 8; ++i) frame(1, 1, 400, 240);  /* 120 ms, then lifted: one click */
  idle(8);
  CHECK(b1_press == 1 && b1_release == 1 && tc.gate.taps == 1);
  /* a finger that moves 10 px presses at 50 ms as before (drags start quickly) */
  fresh();
  t0 = now + 15, pressed_at = 0;
  for (int i = 0; i < 12; ++i) {
    frame(1, 1, 100 + 4 * i, 300);
    if (ptr.down && !pressed_at) pressed_at = (int)(now - t0);
  }
  CHECK(pressed_at >= 45 && pressed_at < 65 && press_x == 100 && press_y == 300);
  idle(8);
  CHECK(b1_press == 1 && b1_release == 1);
  /* a jittering finger (under 10 px) counts as still */
  fresh();
  for (int i = 0; i < 6; ++i) frame(1, 1, 400 + (i % 2) * 6, 240 - (i % 3) * 3);
  CHECK(b1_press == 0);
  idle(8);
}

/* Through the X server: a two-finger scroll and tap become Button4/5 and Button3 clicks at the
 * centroid (x11_tc_apply), queued while Button1 is still down; the pinch text reaches the root. */
static x11_server srv;
static int ev_buttons[8];
static void deliver(void *ctx, int client, const uint8_t *data, size_t len) {
  (void)ctx, (void)client;
  if (len == 32 && (data[0] & 0x7F) == 4 && data[1] < 8) ++ev_buttons[data[1]];  /* ButtonPress */
}
static void test_server_apply(void) {
  x11_server_config cfg;
  x11_default_config(&cfg);
  x11_platform plat = {0};
  plat.deliver = deliver;
  CHECK(x11_server_init_platform(&srv, &cfg, &plat) == 0);
  int c = x11_client_open(&srv);
  uint8_t req[12] = {'l', 0, 11, 0};
  x11_setup_result r;
  uint8_t rep[512];
  x11_client_setup(&srv, c, req, 12, rep, sizeof(rep), &r);
  uint8_t sel[16] = {2, 0, 4, 0};  /* ChangeWindowAttributes root event-mask ButtonPress */
  x_wr32(sel + 4, X11_ID_ROOT_WINDOW, 0), x_wr32(sel + 8, 1u << 11, 0), x_wr32(sel + 12, 1u << 2, 0);
  int close_after;
  static uint8_t big[X11_REPLY_MAX];
  CHECK(x11_client_request(&srv, c, sel, 16, big, sizeof(big), &close_after) == 0);
  fresh();
  memset(ev_buttons, 0, sizeof(ev_buttons));
  /* scroll: both down together, moving down 100 px (natural: Button4) */
  for (int i = 0; i <= 10; ++i) {
    frame(2, 1, 300, 100 + 10 * i, 2, 400, 100 + 10 * i);
    x11_tc_apply(&tc, &srv, ptr.down);
  }
  idle(4);
  x11_tc_apply(&tc, &srv, ptr.down);
  CHECK(ev_buttons[4] >= 2 && ev_buttons[5] == 0 && ev_buttons[1] == 0);
  /* two-finger tap: Button3 */
  frame(2, 3, 300, 300, 4, 400, 300);
  frame(2, 3, 300, 300, 4, 400, 300);
  idle(4);
  x11_tc_apply(&tc, &srv, ptr.down);
  CHECK(ev_buttons[3] == 1 && ev_buttons[1] == 0);
  CHECK(tc.clicks_x11[3] == 1 && tc.clicks_x11[4] == (uint32_t)ev_buttons[4]);
  /* a pinch publishes _WOODLAND_GESTURE on the root */
  for (int i = 0; i <= 8; ++i) {
    frame(2, 5, 350 - 12 * i, 240, 6, 450 + 12 * i, 240);  /* 100 px apart, then spreading */
    x11_tc_apply(&tc, &srv, ptr.down);
  }
  idle(4);
  x11_tc_apply(&tc, &srv, ptr.down);
  CHECK(tc.pinch_set >= 2 && tc.pinch_failed == 0);
  x11_server_free(&srv);
}

/* The diagnostics text: tracker lines, transitions with frame numbers, chain counters. */
static void test_text(void) {
  fresh();
  frame(1, 7, 100, 100);
  frame(2, 7, 100, 100, 2, 200, 100);
  idle(3);
  char buf[4096];
  size_t n = x11_tc_text(&tc, buf, sizeof(buf));
  CHECK(n > 0 && n < sizeof(buf) && buf[n] == 0);
  CHECK(strstr(buf, "tc.ev0=1,") && strstr(buf, ",BEGIN,7,0,100,100\n") && strstr(buf, ",END,2,"));
  CHECK(strstr(buf, "tc.frames=5\n") && strstr(buf, "mt.max_seen=2\n") && strstr(buf, "mt.overflow=0\n"));
  char small[40];
  n = x11_tc_text(&tc, small, sizeof(small));
  CHECK(n == sizeof(small) - 1 && small[n] == 0);  /* truncated, terminated */
}

int main(void) {
  test_map();
  test_one_to_five();
  test_missing_and_failed();
  test_malformed();
  test_single_finger_unchanged();
  test_join_leave_no_stuck();
  test_audit_b5_b6();
  test_server_apply();
  test_text();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("test_touchchain: all OK");
  return 0;
}
