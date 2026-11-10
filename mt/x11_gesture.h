/* Two-finger gesture recogniser (P6 Phase C). Portable C99, no I/O.
 *
 * Input: after every touch read, the contacts that are down (track ID, x, y), e.g. from the
 * x11_mt tracker. Contacts are matched by ID, so the order of a report does not matter.
 * One finger never makes a gesture (it stays the pointer).
 *
 *   second finger down             -> TWO_DOWN (the integration ends any pointer drag);
 *                                     the pair's centroid and distance become the baseline
 *   a finger moved >= start_px     -> decided by how the two fingers moved since the baseline:
 *     both the same way            -> START SCROLL_V / SCROLL_H (one axis dominates by
 *                                     axis_ratio) or PAN, once the centroid moved start_px
 *     opposite ways                -> START PINCH (distance changed >= pinch_px)
 *     one finger still (staggered) -> wait until the other moved 2 x start_px, then the
 *                                     larger of distance change and centroid motion decides
 *   then UPDATE per change (PINCH: scale = d / d0); the gesture keeps its kind
 *   both lifted quickly, no motion -> TAP2 (two-finger tap; the fingers may lift one after
 *                                     the other within tap_ms)
 *   a finger lifts                 -> END (or TAP2, or nothing if nothing started)
 *   a third contact, or the pair's IDs change -> CANCEL
 *
 * After a two-finger touch the remaining finger is ignored until every finger is up, so
 * lifting one finger never turns into a pointer press; a second finger landing again (one
 * contact before, two now) starts a new pair. Fingers lifting (three, then two left) never
 * do: otherwise the last two lifting quickly made a two-finger tap (input audit B5). UPDATE carries the motion since the previous UPDATE (dx, dy), the total since
 * START, and whole steps: scroll steps of scroll_step_px (wheel clicks) or zoom steps of
 * pinch_step (scale factor), with hysteresis so a wobble at a boundary does not repeat. */
#ifndef X11_GESTURE_H
#define X11_GESTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t id;
  int16_t x, y;
} x11_gesture_contact;

typedef struct {
  uint16_t start_px;        /* a finger must move this far before anything is decided, and the
                               centroid this far for a scroll/pan (32) */
  uint16_t pinch_px;        /* distance change needed for a pinch (24) */
  uint16_t still_px;        /* a finger that moved less is "still" (staggered start) (8) */
  float same_dir_cos;       /* cos between the fingers' motions: >= this same way, <= -this opposite (0.5) */
  uint16_t min_spread_px;   /* fingers closer than this cannot pinch (40) */
  float axis_ratio;         /* one axis dominates: |a| >= ratio * |b| (2.0) */
  uint16_t scroll_step_px;  /* one scroll step (30) */
  float pinch_step;         /* one zoom step: scale factor (1.15) */
  uint16_t tap_ms;          /* two-finger tap: both lifted within this of the second down (300) */
  uint16_t tap_slop_px;     /* ... and no finger moved more than this (12) */
} x11_gesture_config;

enum { X11_G_NONE, X11_G_SCROLL_V, X11_G_SCROLL_H, X11_G_PAN, X11_G_PINCH };
enum { X11_GE_TWO_DOWN = 1, X11_GE_START, X11_GE_UPDATE, X11_GE_END, X11_GE_TAP2, X11_GE_CANCEL };

typedef struct {
  uint8_t type, kind;       /* X11_GE_*, X11_G_* */
  uint32_t t_ms;
  int16_t cx, cy;           /* centroid now */
  int16_t dx, dy;           /* motion since the previous UPDATE (START: 0) */
  int32_t total_dx, total_dy;
  float scale;              /* PINCH: distance / distance at the second finger's arrival */
  int16_t steps;            /* whole scroll/zoom steps in this UPDATE: + down/right/out, - up/left/in */
} x11_gesture_event;

#define X11_GESTURE_QUEUE 16

typedef struct {
  x11_gesture_config cfg;
  uint8_t state;            /* 0 idle, 1 one finger, 2 pair pending, 3 pair active, 4 ignoring */
  uint8_t kind;
  uint8_t ida, idb;
  uint32_t t_pair;
  int ax0, ay0, bx0, by0;   /* the pair at its baseline */
  float cx0, cy0, d0;       /* centroid and distance at the baseline */
  float lcx, lcy, ld;       /* centroid and distance at the last UPDATE */
  float step_pos;           /* steps emitted so far (scroll: px / step; pinch: log scale / log step) */
  int moved_max;            /* largest single-finger motion since the baseline (tap test) */
  uint8_t last_n;           /* contacts down at the previous read */
  uint8_t tap_id;           /* a possible tap: the finger still down after the first lifted */
  int16_t tap_x, tap_y;
  x11_gesture_event q[X11_GESTURE_QUEUE];
  int qh, qn;
  uint32_t dropped;         /* events lost to a full queue */
  /* counters */
  uint32_t pairs, starts[5], taps, cancels, ends;
} x11_gesture;

void x11_gesture_default_config(x11_gesture_config *c);
void x11_gesture_init(x11_gesture *g, const x11_gesture_config *c);
/* 1 while two fingers own the touch (a pair, or the finger left after one): the pointer
 * must not act on the touch then (no Button1 press from a two-finger gesture). */
int x11_gesture_owns_touch(const x11_gesture *g);
/* One read: the contacts down now (any order). */
void x11_gesture_feed(x11_gesture *g, uint32_t now_ms, const x11_gesture_contact *c, int n);
int x11_gesture_pop(x11_gesture *g, x11_gesture_event *out);
const char *x11_gesture_kind_name(int kind);
const char *x11_gesture_event_name(int type);

/* ---- Phase D: gestures as X11 core pointer events ----
 * scroll_v steps -> wheel clicks: Button4 (up) / Button5 (down), one per step; "natural"
 *                  (the content follows the fingers): fingers moving down = Button4
 * tap2           -> Button3 click (right click) at the pair's centroid
 * scroll_h, pan, pinch: nothing yet (horizontal wheel = Buttons 6/7 and Ctrl+wheel need X11
 * core support the server does not have). */
typedef struct {
  uint8_t button;           /* 3, 4 or 5 */
  int16_t x, y;             /* where (root coordinates) */
} x11_gesture_click;

#define X11_GESTURE_MAX_CLICKS_PER_EVENT 5

/* The clicks for one event (0..X11_GESTURE_MAX_CLICKS_PER_EVENT); natural: see above. */
int x11_gesture_clicks(const x11_gesture_event *e, int natural, x11_gesture_click *out, int max);

/* ---- Stage 4: pinch for X11 clients (the root property _WOODLAND_GESTURE) ----
 * A pinch is published as one line of text (STRING) in a root property; clients select
 * PropertyChange on the root and read it on PropertyNotify:
 *     pinch <phase> <seq> <steps> <scale> <cx> <cy>
 *   phase  start | update | end | cancel
 *   seq    the pinch's number (one per pinch, from 1)
 *   steps  whole zoom steps since the start (pinch_step each): + fingers apart (larger),
 *          - fingers together (smaller); the total, so a client that misses an update
 *          loses nothing
 *   scale  finger distance / distance at the start (2 decimals); cx, cy the centroid (root)
 * Written at the start, whenever the step total changes, and at the end: a handful of
 * writes per pinch, never one per touch read. */
#define X11_GESTURE_ATOM "_WOODLAND_GESTURE"
#define X11_PINCH_TEXT_MAX 64

typedef struct {
  uint32_t seq;             /* pinches so far */
  int32_t steps;            /* the current pinch's step total */
  uint8_t active;           /* a pinch started and has not ended */
  uint32_t published;       /* texts produced */
} x11_pinch_pub;

/* The text to publish for this event (NUL-terminated, length returned), or 0 when nothing
 * changes for clients. cap >= X11_PINCH_TEXT_MAX. */
int x11_pinch_text(x11_pinch_pub *p, const x11_gesture_event *e, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
