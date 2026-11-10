/* mt/x11_mt.c: the multi-touch contact tracker with synthetic GT9xx reports. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../mt/x11_mt.h"

static x11_mt t;
static uint32_t now;

/* rd(n, id, x, y, id, x, y, ...): one read 15 ms after the previous */
static int rd(int n, ...) {
  x11_mt_point p[8];
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; ++i) {
    p[i].id = (uint8_t)va_arg(ap, int);
    p[i].x = (int16_t)va_arg(ap, int);
    p[i].y = (int16_t)va_arg(ap, int);
    p[i].size = 30;
  }
  va_end(ap);
  now += 15;
  return x11_mt_sample(&t, now, 1, p, n);
}

static int failed(void) {
  now += 15;
  return x11_mt_sample(&t, now, 0, NULL, 0);
}

static const x11_mt_contact *by_id(int id) {
  for (int i = 0; i < X11_MT_MAX; ++i)
    if (t.c[i].state != X11_MT_FREE && t.c[i].id == id) return &t.c[i];
  return NULL;
}

static void fresh(void) {
  x11_mt_init(&t, 800, 480);
  now = 1000;
}

int main(void) {
  /* one finger: NEW, MOVE, STILL; one missing report is a dropout, two are a release */
  fresh();
  assert(rd(1, 0, 100, 100) && by_id(0)->state == X11_MT_NEW && x11_mt_count(&t) == 1);
  assert(rd(1, 0, 110, 100) && by_id(0)->state == X11_MT_MOVE && t.moves == 1);
  assert(rd(1, 0, 110, 100) && by_id(0)->state == X11_MT_STILL);
  assert(!rd(1, 0, 110, 101));  /* 1 px: within move_px, still STILL */
  assert(!rd(0) && by_id(0) && x11_mt_count(&t) == 1);  /* missing once: still down */
  assert(rd(1, 0, 112, 101) && t.dropouts == 1 && by_id(0)->state == X11_MT_MOVE);
  rd(0);
  assert(rd(0) && by_id(0)->state == X11_MT_UP && t.ups == 1 && x11_mt_count(&t) == 0);
  assert(rd(0) && !by_id(0));  /* UP shown once, then freed */
  assert(t.news == 1 && t.max_seen == 1 && t.lost == 0 && t.id_changes == 0 && t.multi_ms_max == 0);

  /* two fingers: independent, by ID; the report order may change */
  fresh();
  rd(1, 0, 100, 100);
  rd(2, 0, 100, 100, 1, 600, 300);
  assert(x11_mt_count(&t) == 2 && by_id(1)->state == X11_MT_NEW && t.max_seen == 2);
  rd(2, 1, 610, 300, 0, 101, 100);  /* reordered */
  assert(t.reorders == 1 && by_id(1)->x == 610 && by_id(0)->x == 101 && by_id(1)->state == X11_MT_MOVE);
  rd(2, 1, 620, 300, 0, 102, 100);  /* same order again: no new reorder */
  assert(t.reorders == 1);
  rd(1, 1, 630, 300);  /* the first finger lifts first */
  rd(1, 1, 640, 300);
  assert(by_id(0)->state == X11_MT_UP && by_id(1)->state == X11_MT_MOVE && x11_mt_count(&t) == 1);
  assert(t.multi_ms_max >= 45);  /* two down from the 2nd to the 5th read */
  rd(0), rd(0);
  assert(x11_mt_count(&t) == 0 && t.ups == 2 && t.by_count[2] == 3 && t.by_count[1] == 3);
  assert(t.id_changes == 0 && t.dropouts == 0);  /* missing once, then released: not a dropout */

  /* the second finger lifts first, then the first; slots are reused */
  fresh();
  rd(2, 3, 200, 200, 4, 300, 300);
  rd(1, 3, 200, 200), rd(1, 3, 200, 200);
  assert(by_id(4)->state == X11_MT_UP && by_id(3)->state == X11_MT_STILL);
  rd(2, 3, 200, 200, 5, 400, 100);  /* another finger, new ID, the freed slot */
  assert(by_id(5)->state == X11_MT_NEW && x11_mt_count(&t) == 2);

  /* five fingers (the family maximum) */
  fresh();
  rd(5, 0, 10, 10, 1, 200, 10, 2, 400, 10, 3, 600, 10, 4, 790, 470);
  assert(x11_mt_count(&t) == 5 && t.max_seen == 5 && t.by_count[5] == 1 && t.malformed == 0);

  /* malformed: a count above 5, an ID above 15, coordinates off the screen */
  fresh();
  {
    x11_mt_point p[6] = {{0, 1, 1, 1}, {1, 2, 2, 1}, {2, 3, 3, 1}, {3, 4, 4, 1}, {4, 5, 5, 1}, {5, 6, 6, 1}};
    now += 15;
    x11_mt_sample(&t, now, 1, p, 6);
    assert(t.malformed == 1 && x11_mt_count(&t) == 5);
  }
  fresh();
  rd(3, 20, 10, 10, 1, 900, 10, 2, 10, -5);
  assert(t.malformed == 3 && x11_mt_count(&t) == 0 && t.touched_reads == 0 && t.by_count[0] == 1);

  /* duplicate IDs in one report: the first counts */
  fresh();
  rd(2, 7, 100, 100, 7, 300, 300);
  assert(t.duplicate_ids == 1 && x11_mt_count(&t) == 1 && by_id(7)->x == 100);

  /* failed reads: one is harmless; a run releases the contact and counts it as lost */
  fresh();
  rd(1, 0, 100, 100);
  failed();
  assert(x11_mt_count(&t) == 1);
  failed();
  assert(by_id(0)->state == X11_MT_UP && t.lost == 1 && t.failed_reads == 2);
  rd(0);
  rd(1, 0, 100, 100), rd(0), rd(0);
  assert(t.lost == 1);  /* an ordinary release is not lost */

  /* an ID change: a contact disappears as a new ID appears next to it */
  fresh();
  rd(1, 0, 100, 100), rd(1, 0, 104, 100);
  rd(1, 1, 108, 101);
  rd(1, 1, 112, 102);
  assert(by_id(0)->state == X11_MT_UP && t.id_changes == 1);
  /* far away: two separate touches, not an ID change */
  fresh();
  rd(1, 0, 100, 100);
  rd(1, 1, 600, 400), rd(1, 1, 600, 400);
  assert(t.id_changes == 0 && t.ups == 1);

  /* the diagnostics text */
  fresh();
  rd(2, 0, 100, 100, 1, 600, 300);
  char buf[2048];
  size_t n = x11_mt_text(&t, buf, sizeof(buf));
  assert(n == strlen(buf) && strstr(buf, "mt.count=2\n") && strstr(buf, "mt.max_seen=2\n"));
  assert(strstr(buf, "mt.c0=0,100,100,30,NEW\n") && strstr(buf, "mt.c1=1,600,300,30,NEW\n"));
  assert(x11_mt_text(&t, buf, 40) == 39 && strlen(buf) == 39);  /* truncation stays terminated */
  printf("test_mt: all OK\n");
  return 0;
}
