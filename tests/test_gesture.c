/* mt/x11_gesture.c: two-finger gestures from synthetic contact sequences. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../mt/x11_gesture.h"

static x11_gesture g;
static uint32_t now;
static x11_gesture_event ev[256];
static int nev;

static void drain(void) {
  while (nev < 256 && x11_gesture_pop(&g, &ev[nev])) ++nev;
}

static void fresh(void) {
  x11_gesture_init(&g, NULL);
  now = 1000, nev = 0;
}

static void none(void) {
  now += 15;
  x11_gesture_feed(&g, now, NULL, 0);
  drain();
}

static void one(int id, int x, int y) {
  x11_gesture_contact c = {(uint8_t)id, (int16_t)x, (int16_t)y};
  now += 15;
  x11_gesture_feed(&g, now, &c, 1);
  drain();
}

static void two(int ia, int ax, int ay, int ib, int bx, int by) {
  x11_gesture_contact c[2] = {{(uint8_t)ia, (int16_t)ax, (int16_t)ay}, {(uint8_t)ib, (int16_t)bx, (int16_t)by}};
  now += 15;
  x11_gesture_feed(&g, now, c, 2);
  drain();
}

static int count(int type) {
  int n = 0;
  for (int i = 0; i < nev; ++i) n += ev[i].type == type;
  return n;
}

static const x11_gesture_event *first(int type) {
  for (int i = 0; i < nev; ++i)
    if (ev[i].type == type) return &ev[i];
  return NULL;
}

static int sum_steps(void) {
  int s = 0;
  for (int i = 0; i < nev; ++i)
    if (ev[i].type == X11_GE_UPDATE) s += ev[i].steps;
  return s;
}

/* two fingers 200 px apart moving together by (dx, dy) per read, n reads, then lifted */
static void pair_move(int dx, int dy, int n, int swap_order) {
  int ax = 300, ay = 200, bx = 500, by = 200;
  two(0, ax, ay, 1, bx, by);
  for (int i = 1; i <= n; ++i) {
    if (swap_order && i % 2) two(1, bx + dx * i, by + dy * i, 0, ax + dx * i, ay + dy * i);
    else two(0, ax + dx * i, ay + dy * i, 1, bx + dx * i, by + dy * i);
  }
  none();
}

int main(void) {
  /* one finger: drags, taps, jitter: never a gesture event */
  fresh();
  for (int i = 0; i < 60; ++i) one(0, 100 + 5 * i, 100 + 3 * i);
  none();
  one(0, 50, 50), none();
  assert(nev == 0 && g.pairs == 0);

  /* vertical two-finger scroll: down 4 px per read for 30 reads = 120 px = 4 steps */
  fresh();
  pair_move(0, 4, 30, 0);
  assert(count(X11_GE_TWO_DOWN) == 1 && count(X11_GE_START) == 1 && count(X11_GE_END) == 1);
  assert(first(X11_GE_START)->kind == X11_G_SCROLL_V);
  assert(sum_steps() == 4 && ev[nev - 1].type == X11_GE_END && ev[nev - 1].total_dy == 120);
  /* START comes at the threshold (32 px = the 8th read), not before */
  assert(first(X11_GE_START)->t_ms - first(X11_GE_TWO_DOWN)->t_ms == 8 * 15);

  /* up is negative steps; horizontal; diagonal is a pan without steps */
  fresh();
  pair_move(0, -4, 30, 0);
  assert(first(X11_GE_START)->kind == X11_G_SCROLL_V && sum_steps() == -4);
  fresh();
  pair_move(5, 1, 24, 0);
  assert(first(X11_GE_START)->kind == X11_G_SCROLL_H && sum_steps() == 4);
  fresh();
  pair_move(4, 4, 20, 0);
  assert(first(X11_GE_START)->kind == X11_G_PAN && sum_steps() == 0 && ev[nev - 1].total_dx == 80);

  /* the report order changes every other read: the same result (matched by ID) */
  fresh();
  pair_move(0, 4, 30, 1);
  assert(first(X11_GE_START)->kind == X11_G_SCROLL_V && sum_steps() == 4 && count(X11_GE_CANCEL) == 0);

  /* pinch out and in: scale relative to the distance at the second finger's arrival */
  fresh();
  two(0, 300, 240, 1, 500, 240);  /* d0 = 200 */
  for (int i = 1; i <= 20; ++i) two(0, 300 - 5 * i, 240, 1, 500 + 5 * i, 240);  /* d = 400 */
  const x11_gesture_event *s = first(X11_GE_START);
  assert(s && s->kind == X11_G_PINCH);
  assert(fabsf(ev[nev - 1].scale - 2.0f) < 0.01f);
  assert(sum_steps() == (int)(logf(2.0f) / logf(1.15f)));  /* 4 zoom steps */
  for (int i = 19; i >= 0; --i) two(0, 300 - 5 * i, 240, 1, 500 + 5 * i, 240);  /* back to d0 */
  assert(sum_steps() == 0);  /* back at scale 1.0: the steps are undone */
  for (int i = 1; i <= 10; ++i) two(0, 300 + 5 * i, 240, 1, 500 - 5 * i, 240);  /* d = 100: pinch in */
  assert(sum_steps() < 0 && ev[nev - 1].scale < 0.51f);
  none();
  assert(count(X11_GE_END) == 1 && count(X11_GE_START) == 1);

  /* fingers too close cannot pinch (d0 < 40); moving them together is a scroll */
  fresh();
  two(0, 400, 200, 1, 430, 200);
  for (int i = 1; i <= 15; ++i) two(0, 400, 200 + 3 * i, 1, 430 + i, 200 + 3 * i);  /* 45 px */
  assert(first(X11_GE_START) && first(X11_GE_START)->kind == X11_G_SCROLL_V);

  /* below the thresholds: no START; a quick still pair is a two-finger tap */
  fresh();
  two(0, 300, 200, 1, 500, 200), two(0, 303, 202, 1, 498, 199), none();
  assert(count(X11_GE_TAP2) == 1 && count(X11_GE_START) == 0 && count(X11_GE_END) == 0);
  fresh();  /* held too long: not a tap */
  two(0, 300, 200, 1, 500, 200);
  for (int i = 0; i < 30; ++i) two(0, 300, 200, 1, 500, 200);
  none();
  assert(count(X11_GE_TAP2) == 0 && count(X11_GE_START) == 0);
  fresh();  /* moved past the tap slop but under the start threshold, then lifted: nothing */
  two(0, 300, 200, 1, 500, 200), two(0, 314, 200, 1, 500, 200), none();
  assert(count(X11_GE_TAP2) == 0 && count(X11_GE_START) == 0 && nev == 1);

  /* release order: either finger first ends the gesture; the remaining finger is ignored
   * (it never becomes a pointer gesture) until all are up */
  for (int which = 0; which < 2; ++which) {
    fresh();
    two(0, 300, 200, 1, 500, 200);
    for (int i = 1; i <= 10; ++i) two(0, 300, 200 + 4 * i, 1, 500, 200 + 4 * i);
    if (which) one(0, 300, 240);
    else one(1, 500, 240);
    assert(count(X11_GE_END) == 1);
    int before = nev;
    for (int i = 0; i < 20; ++i) one(which ? 0 : 1, 300 + 10 * i, 300);  /* the remaining finger drags */
    none();
    assert(nev == before && g.state == 0);
  }

  /* transitions 1 -> 2 -> 1 -> 2: the second finger landing again starts a new pair */
  fresh();
  one(0, 300, 200), one(0, 310, 200);
  two(0, 310, 200, 1, 500, 200);
  for (int i = 1; i <= 8; ++i) two(0, 310, 200 + 4 * i, 1, 500, 200 + 4 * i);
  one(0, 310, 232);
  two(0, 310, 232, 2, 520, 232);  /* a new finger, new ID */
  for (int i = 1; i <= 8; ++i) two(0, 310 + 4 * i, 232, 2, 520 + 4 * i, 232);
  none();
  assert(count(X11_GE_TWO_DOWN) == 2 && count(X11_GE_START) == 2 && count(X11_GE_END) == 2);
  assert(ev[nev - 1].kind == X11_G_SCROLL_H);

  /* a third contact cancels; an ID change within the pair cancels */
  fresh();
  two(0, 300, 200, 1, 500, 200);
  for (int i = 1; i <= 6; ++i) two(0, 300, 200 + 4 * i, 1, 500, 200 + 4 * i);
  {
    x11_gesture_contact c[3] = {{0, 300, 224}, {1, 500, 224}, {2, 400, 400}};
    now += 15;
    x11_gesture_feed(&g, now, c, 3);
    drain();
  }
  assert(count(X11_GE_CANCEL) == 1 && count(X11_GE_END) == 0);
  none();
  fresh();
  two(0, 300, 200, 1, 500, 200), two(0, 300, 210, 3, 500, 210);
  assert(count(X11_GE_CANCEL) == 1);
  fresh();  /* three at once from idle: ignored, never a pair */
  {
    x11_gesture_contact c[3] = {{0, 100, 100}, {1, 300, 100}, {2, 500, 100}};
    now += 15;
    x11_gesture_feed(&g, now, c, 3);
    drain();
  }
  two(0, 100, 100, 1, 300, 100);  /* one lifted: still the same touch, never a new pair (B5) */
  one(0, 100, 100);
  none();
  assert(count(X11_GE_TWO_DOWN) == 0 && count(X11_GE_TAP2) == 0);

  /* B5: three (or five) fingers lifting one by one, quickly: no pair, no two-finger tap */
  for (int k = 3; k <= 5; ++k) {
    fresh();
    x11_gesture_contact c[5] = {{0, 100, 100}, {1, 250, 100}, {2, 400, 100}, {3, 550, 100}, {4, 700, 100}};
    two(0, 100, 100, 1, 250, 100);  /* they land one after another */
    for (int n = 3; n <= k; ++n) now += 15, x11_gesture_feed(&g, now, c, n), drain();
    for (int n = k - 1; n >= 1; --n) now += 15, x11_gesture_feed(&g, now, c, n), drain();
    none();
    assert(count(X11_GE_TWO_DOWN) == 1 && count(X11_GE_CANCEL) == 1 && count(X11_GE_TAP2) == 0);
  }
  /* ... but after they lift to one, a finger landing again is a new pair (a tap here) */
  fresh();
  {
    x11_gesture_contact c[3] = {{0, 100, 100}, {1, 300, 100}, {2, 500, 100}};
    now += 15, x11_gesture_feed(&g, now, c, 3), drain();
  }
  one(0, 100, 100);
  two(0, 100, 100, 5, 300, 100), two(0, 100, 100, 5, 300, 100);
  none();
  assert(count(X11_GE_TWO_DOWN) == 1 && count(X11_GE_TAP2) == 1);

  /* jitter around the start: nothing */
  fresh();
  two(0, 300, 200, 1, 500, 200);
  for (int i = 0; i < 50; ++i) two(0, 300 + (i % 3) - 1, 200 + (i % 5) - 2, 1, 500 - (i % 3) + 1, 200 + (i % 4) - 2);
  none();
  assert(count(X11_GE_START) == 0 && count(X11_GE_TAP2) == 0);

  /* scroll wobble at a step boundary does not repeat steps */
  fresh();
  two(0, 300, 200, 1, 500, 200);
  for (int i = 1; i <= 8; ++i) two(0, 300, 200 + 4 * i, 1, 500, 200 + 4 * i);  /* 32 px: 1 step */
  for (int k = 0; k < 10; ++k) {
    two(0, 300, 226, 1, 500, 226);  /* 26 px */
    two(0, 300, 236, 1, 500, 236);  /* 36 px */
  }
  assert(sum_steps() == 1);
  none();
  /* from the 00:02 panel session: a slide whose second finger starts late is a scroll, not a
   * pinch (one finger first moves 24 px alone: the distance changes, the centroid barely) */
  fresh();
  two(0, 300, 200, 1, 500, 200);
  for (int i = 1; i <= 6; ++i) two(0, 300, 200 + 4 * i, 1, 500, 200);  /* A alone: 24 px */
  assert(count(X11_GE_START) == 0);
  for (int i = 1; i <= 20; ++i) two(0, 300, 224 + 4 * i, 1, 500, 200 + 5 * i);  /* B follows */
  none();
  assert(first(X11_GE_START) && first(X11_GE_START)->kind == X11_G_SCROLL_V);
  /* a scroll whose first few px wobble sideways is still vertical (decided at 32 px) */
  fresh();
  two(0, 300, 200, 1, 500, 200);
  for (int i = 1; i <= 3; ++i) two(0, 300 + 4 * i, 200 + 2 * i, 1, 500 + 4 * i, 200 + 2 * i);
  for (int i = 1; i <= 20; ++i) two(0, 312, 206 + 6 * i, 1, 512, 206 + 6 * i);
  none();
  assert(first(X11_GE_START)->kind == X11_G_SCROLL_V);
  /* the panel's "diagonal" (264, -168: ratio 1.57) is a pan */
  fresh();
  two(0, 300, 300, 1, 500, 300);
  for (int i = 1; i <= 24; ++i) two(0, 300 + 11 * i, 300 - 7 * i, 1, 500 + 11 * i, 300 - 7 * i);
  none();
  assert(first(X11_GE_START)->kind == X11_G_PAN);
  /* zoom with one finger anchored: a pinch, decided once the moving finger went 64 px */
  fresh();
  two(0, 300, 240, 1, 400, 240);
  for (int i = 1; i <= 15; ++i) two(0, 300, 240, 1, 400 + 5 * i, 240);
  assert(first(X11_GE_START) && first(X11_GE_START)->kind == X11_G_PINCH);
  assert(first(X11_GE_START)->t_ms - first(X11_GE_TWO_DOWN)->t_ms == 13 * 15);  /* 65 px */
  none();
  assert(fabsf(ev[nev - 1].scale - 1.75f) < 0.01f);  /* END carries the final scale */
  /* rotation-like motion (perpendicular) waits until one finger went 64 px */
  fresh();
  two(0, 300, 240, 1, 500, 240);
  for (int i = 1; i <= 7; ++i) two(0, 300, 240 - 5 * i, 1, 500 + 5 * i, 240);
  assert(count(X11_GE_START) == 0);
  none();
  /* two-finger taps with staggered lifts (the panel: 5 of 10 were lost this way) */
  fresh();
  two(0, 300, 200, 1, 500, 200), two(0, 301, 200, 1, 500, 201), one(1, 500, 201), none();
  assert(count(X11_GE_TAP2) == 1);
  fresh();
  two(0, 300, 200, 1, 500, 200), one(0, 300, 200), none();
  assert(count(X11_GE_TAP2) == 1);
  fresh();  /* the second lift comes too late: no tap */
  two(0, 300, 200, 1, 500, 200), one(0, 300, 200);
  for (int i = 0; i < 25; ++i) one(0, 300, 200);
  none();
  assert(count(X11_GE_TAP2) == 0);
  fresh();  /* the remaining finger moves away: no tap, and nothing else either */
  two(0, 300, 200, 1, 500, 200), one(0, 300, 200), one(0, 330, 200), none();
  assert(count(X11_GE_TAP2) == 0 && nev == 1);
  fresh();  /* ten quick taps with alternating lift orders: ten taps */
  for (int k = 0; k < 10; ++k) {
    two(0, 300, 200, 1, 500, 200);
    if (k % 2) one(0, 300, 200);
    else one(1, 500, 200);
    none(), none(), none();
  }
  assert(count(X11_GE_TAP2) == 10 && count(X11_GE_TWO_DOWN) == 10);
  /* Phase D: events -> X11 clicks */
  {
    x11_gesture_click k[8];
    x11_gesture_event e;
    memset(&e, 0, sizeof(e));
    e.type = X11_GE_UPDATE, e.kind = X11_G_SCROLL_V, e.cx = 400, e.cy = 240, e.steps = 2;
    assert(x11_gesture_clicks(&e, 1, k, 8) == 2 && k[0].button == 4 && k[1].button == 4 && k[0].x == 400);
    assert(x11_gesture_clicks(&e, 0, k, 8) == 2 && k[0].button == 5);  /* classic direction */
    e.steps = -3;
    assert(x11_gesture_clicks(&e, 1, k, 8) == 3 && k[2].button == 5);
    e.steps = -9;  /* bounded per event */
    assert(x11_gesture_clicks(&e, 1, k, 8) == X11_GESTURE_MAX_CLICKS_PER_EVENT);
    assert(x11_gesture_clicks(&e, 1, k, 2) == 2);
    e.steps = 0;
    assert(x11_gesture_clicks(&e, 1, k, 8) == 0);
    e.kind = X11_G_SCROLL_H, e.steps = 2;
    assert(x11_gesture_clicks(&e, 1, k, 8) == 0);  /* no horizontal wheel yet */
    e.kind = X11_G_PINCH;
    assert(x11_gesture_clicks(&e, 1, k, 8) == 0);
    e.type = X11_GE_TAP2, e.kind = X11_G_NONE, e.cx = 100, e.cy = 50;
    assert(x11_gesture_clicks(&e, 1, k, 8) == 1 && k[0].button == 3 && k[0].x == 100 && k[0].y == 50);
    e.type = X11_GE_START;
    assert(x11_gesture_clicks(&e, 1, k, 8) == 0);
  }
  /* a whole scroll: 120 px down = 4 wheel-up clicks (natural); the pair owns the touch */
  fresh();
  two(0, 300, 200, 1, 500, 200);
  assert(x11_gesture_owns_touch(&g));
  for (int i = 1; i <= 30; ++i) two(0, 300, 200 + 4 * i, 1, 500, 200 + 4 * i);
  one(0, 300, 320);
  assert(x11_gesture_owns_touch(&g));  /* the finger left behind still belongs to the gesture */
  none();
  assert(!x11_gesture_owns_touch(&g));
  {
    int clicks = 0, up = 0;
    x11_gesture_click k[8];
    for (int i = 0; i < nev; ++i) {
      int c = x11_gesture_clicks(&ev[i], 1, k, 8);
      clicks += c;
      for (int j = 0; j < c; ++j) up += k[j].button == 4;
    }
    assert(clicks == 4 && up == 4);
  }
  fresh();
  one(0, 100, 100);
  assert(!x11_gesture_owns_touch(&g));  /* one finger: the pointer's */
  /* Stage 4: the pinch as _WOODLAND_GESTURE text: start, one line per step total change, end */
  {
    x11_pinch_pub pub;
    memset(&pub, 0, sizeof(pub));
    char texts[32][X11_PINCH_TEXT_MAX];
    int nt = 0;
    fresh();
    two(0, 300, 240, 1, 500, 240);                                               /* d0 = 200 */
    for (int i = 1; i <= 20; ++i) two(0, 300 - 5 * i, 240, 1, 500 + 5 * i, 240);  /* d = 400 */
    none();
    for (int i = 0; i < nev; ++i) {
      char t[X11_PINCH_TEXT_MAX];
      if (x11_pinch_text(&pub, &ev[i], t, sizeof(t)) > 0 && nt < 32) strcpy(texts[nt++], t);
    }
    /* 2x = log(2)/log(1.15) = 4.96 -> 4 whole steps (hysteresis), each its own update */
    assert(nt >= 3 && !strncmp(texts[0], "pinch start 1 0 ", 16));
    assert(!strncmp(texts[nt - 1], "pinch end 1 4 2.00 ", 19));
    int updates = 0, last = 0;
    for (int i = 1; i < nt - 1; ++i) {
      int st = 0;
      assert(sscanf(texts[i], "pinch update 1 %d", &st) == 1);
      assert(st > last);  /* totals, rising (a fast read can add more than one step) */
      last = st, ++updates;
    }
    assert(last == 4 && updates >= 2 && pub.published == (uint32_t)nt && !pub.active);
    /* the next pinch (in): seq 2, negative steps; scroll events publish nothing */
    nt = 0;
    fresh();
    two(0, 200, 240, 1, 600, 240);                                               /* d0 = 400 */
    for (int i = 1; i <= 20; ++i) two(0, 200 + 5 * i, 240, 1, 600 - 5 * i, 240);  /* d = 200 */
    none();
    pair_move(0, 4, 30, 0);
    for (int i = 0; i < nev; ++i) {
      char t[X11_PINCH_TEXT_MAX];
      if (x11_pinch_text(&pub, &ev[i], t, sizeof(t)) > 0 && nt < 32) strcpy(texts[nt++], t);
    }
    assert(!strncmp(texts[0], "pinch start 2 0 ", 16));
    assert(!strncmp(texts[nt - 1], "pinch end 2 -4 0.50 ", 20));
    /* a cancel ends it too; an END without a pinch publishes nothing */
    x11_gesture_event e;
    memset(&e, 0, sizeof(e));
    e.type = X11_GE_START, e.kind = X11_G_PINCH, e.scale = 1.0f;
    char t[X11_PINCH_TEXT_MAX];
    assert(x11_pinch_text(&pub, &e, t, sizeof(t)) > 0 && !strncmp(t, "pinch start 3 0 1.00", 20));
    e.type = X11_GE_CANCEL;
    assert(x11_pinch_text(&pub, &e, t, sizeof(t)) > 0 && !strncmp(t, "pinch cancel 3 0", 16));
    e.type = X11_GE_END;
    assert(x11_pinch_text(&pub, &e, t, sizeof(t)) == 0);
  }
  printf("test_gesture: all OK\n");
  return 0;
}
