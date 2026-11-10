/* The touch chain as the test firmware runs it: raw GT9xx reports -> contact tracker
 * (mt/x11_mt) -> gesture recogniser (mt/x11_gesture) + pointer gate (mt/x11_gate) -> the
 * single-touch pointer filter (input/x11_touch) -> Button1; gesture clicks -> Button3/4/5.
 * Counts what an X client would see. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../input/x11_touch.h"
#include "../mt/x11_gate.h"
#include "../mt/x11_gesture.h"
#include "../mt/x11_mt.h"

#define HOLD 50

static x11_mt mt;
static x11_gesture gest;
static x11_gate gate;
static x11_touch ptr;
static uint32_t now;
static int b1_press, b1_release, clicks[6], last_down;
static int press_x, press_y;
static uint32_t press_at, first_contact_at;

static void fresh(void) {
  x11_mt_init(&mt, 800, 480);
  x11_gesture_init(&gest, NULL);
  x11_gate_init(&gate, HOLD);
  x11_touch_config c;
  x11_touch_default_config(&c, 800, 480);
  x11_touch_init(&ptr, &c);
  now = 1000, b1_press = b1_release = last_down = 0, press_at = first_contact_at = 0;
  memset(clicks, 0, sizeof(clicks));
}

/* one read after dt ms: n raw points (id, x, y, ...) */
static void rd_at(uint32_t dt, int n, ...) {
  x11_mt_point p[5];
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; ++i) {
    p[i].id = (uint8_t)va_arg(ap, int);
    p[i].x = (int16_t)va_arg(ap, int);
    p[i].y = (int16_t)va_arg(ap, int);
    p[i].size = 30;
  }
  va_end(ap);
  now += dt;
  if (n && !first_contact_at) first_contact_at = now;
  x11_mt_sample(&mt, now, 1, p, n);
  x11_gesture_contact c[5];
  int k = 0;
  for (int i = 0; i < X11_MT_MAX; ++i) {
    const x11_mt_contact *m = &mt.c[i];
    if (m->state == X11_MT_NEW || m->state == X11_MT_MOVE || m->state == X11_MT_STILL)
      c[k].id = m->id, c[k].x = m->x, c[k].y = m->y, ++k;
  }
  x11_gesture_feed(&gest, now, c, k);
  x11_gesture_event e;
  while (x11_gesture_pop(&gest, &e)) {
    x11_gesture_click ck[X11_GESTURE_MAX_CLICKS_PER_EVENT];
    int nk = x11_gesture_clicks(&e, 1, ck, X11_GESTURE_MAX_CLICKS_PER_EVENT);
    for (int i = 0; i < nk; ++i) ++clicks[ck[i].button];
  }
  int ox = 0, oy = 0;
  int pts = x11_gate_step(&gate, now, k, k ? c[0].x : 0, k ? c[0].y : 0, x11_gesture_owns_touch(&gest), &ox, &oy);
  x11_touch_sample(&ptr, now, 1, pts, ox, oy);
  if (ptr.down && !last_down) {
    ++b1_press, press_x = ptr.x, press_y = ptr.y;
    if (!press_at) press_at = now;
  }
  if (!ptr.down && last_down) ++b1_release;
  last_down = ptr.down;
}
#define rd(...) rd_at(15, __VA_ARGS__)

static void idle(int reads) {
  for (int i = 0; i < reads; ++i) rd(0);
}

/* two fingers: the second lands `lag` reads after the first, then both slide down 4 px/read */
static void two_finger_scroll(int lag) {
  for (int i = 0; i < lag; ++i) rd(1, 0, 300, 200);
  for (int i = 0; i <= 30; ++i) rd(2, 0, 300, 200 + 4 * i, 1, 500, 200 + 4 * i);
  rd(1, 1, 500, 320), idle(10);
}

int main(void) {
  /* a quick tap (two reads, 30 ms): one press/release pair, at the landing point, sent at the lift */
  fresh();
  rd(1, 0, 123, 77), rd(1, 0, 124, 77);
  idle(10);
  assert(b1_press == 1 && b1_release == 1 && press_x == 123 && press_y == 77);
  printf("tap: press %u ms after the finger landed (the lift)\n", press_at - first_contact_at);
  assert(gate.taps == 1 && gate.commits == 0);

  /* a long still press: the press comes at the hold, at the landing point */
  fresh();
  for (int i = 0; i < 30; ++i) rd(1, 0, 400, 240);
  idle(10);
  assert(b1_press == 1 && b1_release == 1 && gate.commits == 1);
  assert(press_at - first_contact_at >= HOLD && press_at - first_contact_at < HOLD + 15);
  printf("press: %u ms after the finger landed\n", press_at - first_contact_at);

  /* a single-finger drag (text selection): press at the landing point, then it follows */
  fresh();
  for (int i = 0; i < 40; ++i) rd(1, 0, 100 + 5 * i, 300);
  assert(b1_press == 1 && press_x == 100 && press_y == 300 && ptr.x == 295);
  idle(10);
  assert(b1_release == 1 && !ptr.down);

  /* two-finger scrolls whose second finger lands 0, 1, 2 or 3 reads (0-45 ms) after the first:
   * no Button1 at all, wheel clicks as before */
  for (int lag = 0; lag <= 3; ++lag) {
    fresh();
    two_finger_scroll(lag);
    assert(b1_press == 0 && b1_release == 0);
    assert(clicks[4] == 4 && clicks[5] == 0 && clicks[3] == 0);
  }

  /* the second finger after the hold (the first finger was already dragging): Button1 was a
   * real press, released when the gesture takes over; never left down */
  fresh();
  two_finger_scroll(6);  /* 90 ms */
  assert(b1_press == 1 && b1_release == 1 && !ptr.down && gate.blocked_active == 1);

  /* two-finger taps, together and staggered, either lift order: Button3 only */
  fresh();
  for (int k = 0; k < 5; ++k) {
    if (k % 2) rd(1, 0, 300, 200);           /* the first finger alone for one read */
    rd(2, 0, 300, 200, 1, 500, 200), rd(2, 0, 300, 200, 1, 500, 200);
    if (k % 3) rd(1, 1, 500, 200);           /* one finger lifts first */
    else rd(1, 0, 300, 200);
    idle(6);
  }
  assert(b1_press == 0 && clicks[3] == 5);

  /* a contact missing from one report (dropout) is not a lift: no tap, one press */
  fresh();
  for (int i = 0; i < 4; ++i) rd(1, 0, 200, 200);
  rd(0);
  for (int i = 0; i < 10; ++i) rd(1, 0, 200, 200);
  idle(10);
  assert(b1_press == 1 && b1_release == 1 && gate.taps == 0);

  /* a dropout while pending does not make a tap either, and a second finger still blocks */
  fresh();
  rd(1, 0, 200, 200), rd(0), rd(2, 0, 200, 200, 1, 400, 200);
  for (int i = 1; i <= 20; ++i) rd(2, 0, 200, 200 + 4 * i, 1, 400, 200 + 4 * i);
  idle(10);
  assert(b1_press == 0);

  /* cancellation: three fingers, never Button1, nothing stuck */
  fresh();
  rd(1, 0, 100, 100), rd(2, 0, 100, 100, 1, 300, 100), rd(3, 0, 100, 100, 1, 300, 100, 2, 500, 100);
  rd(2, 0, 100, 100, 1, 300, 100), rd(1, 0, 100, 100);
  idle(10);
  assert(b1_press == 0 && !ptr.down);

  /* rapid transitions 1 -> 2 -> 1 -> 2 -> 0, crossing fingers, either release order */
  fresh();
  rd(1, 0, 300, 200), rd(2, 0, 300, 200, 1, 500, 200);
  for (int i = 1; i <= 12; ++i) rd(2, 0, 300 + 16 * i, 200, 1, 500 - 16 * i, 200);  /* they cross */
  rd(1, 1, 308, 200);
  rd(2, 1, 308, 200, 2, 600, 300);
  for (int i = 1; i <= 10; ++i) rd(2, 1, 308, 200 + 4 * i, 2, 600, 300 + 4 * i);
  rd(1, 2, 600, 340);
  idle(10);
  assert(b1_press == 0 && !ptr.down);

  /* missed polling intervals: a touch read once, the next read 120 ms later already without it
   * (the tracker keeps it one more read): committed at that read, released after; one pair */
  fresh();
  rd_at(15, 1, 0, 50, 60);
  rd_at(120, 0);
  idle(10);
  assert(b1_press == 1 && b1_release == 1 && press_x == 50);
  /* a press whose next read comes 200 ms later: committed then, no tap */
  fresh();
  rd_at(15, 1, 0, 50, 60);
  rd_at(200, 1, 0, 50, 60);
  assert(b1_press == 1 && gate.commits == 1);
  idle(10);
  assert(b1_release == 1);

  /* double tap: two separate press/release pairs */
  fresh();
  rd(1, 0, 400, 240), rd(1, 0, 400, 240);
  idle(5);
  rd(1, 0, 402, 241), rd(1, 0, 402, 241);
  idle(10);
  assert(b1_press == 2 && b1_release == 2);

  /* hold 0 = the old behaviour (immediate press): the staggered scroll then clicks */
  fresh();
  x11_gate_init(&gate, 0);
  two_finger_scroll(1);
  assert(b1_press == 1 && b1_release == 1);
  printf("test_gate: all OK\n");
  return 0;
}
