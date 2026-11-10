/* The core pointer (Milestone 3A). One pointer device, fed by the platform through
 * x11_pointer_input() (the GT911 touchscreen on the ESP32, a test harness on the host).
 *
 * Implemented as the core protocol describes it (X11 protocol, sections 10-12):
 *   - the sprite: the deepest viewable window containing the pointer (borders included,
 *     InputOnly windows too); EnterNotify/LeaveNotify with the details Ancestor, Virtual,
 *     Inferior, Nonlinear, NonlinearVirtual whenever it changes, by pointer motion or by
 *     window changes (map, unmap, configure, restack), mode Normal, Grab or Ungrab;
 *   - MotionNotify (PointerMotion, ButtonMotion, Button1..5Motion, PointerMotionHint),
 *     ButtonPress, ButtonRelease, propagated from the sprite towards the root until a window
 *     where a client selected them, stopped by do-not-propagate masks;
 *   - active grabs (GrabPointer), passive grabs (GrabButton), and the automatic grab a
 *     ButtonPress starts; owner-events, the grab event mask, confine-to; a synchronous
 *     pointer mode freezes event processing until AllowEvents (AsyncPointer, SyncPointer,
 *     ReplayPointer); events that arrive meanwhile wait in a bounded queue;
 *   - grabs end when their client closes, or when their window (or confine-to window)
 *     stops being viewable.
 * There is no keyboard: key events never happen, modifiers are always 0, and keyboard
 * grabs only record their owner (GrabKeyboard answers AlreadyGrabbed/NotViewable correctly).
 * There is no cursor image: cursors are stored, never drawn. */
#include "x11_internal.h"

enum { BUTTON_PRESS = 4, BUTTON_RELEASE = 5, MOTION_NOTIFY = 6, ENTER_NOTIFY = 7, LEAVE_NOTIFY = 8 };
enum { D_ANCESTOR = 0, D_VIRTUAL = 1, D_INFERIOR = 2, D_NONLINEAR = 3, D_NONLINEAR_VIRTUAL = 4 };
enum { MODE_NORMAL = 0, MODE_GRAB = 1, MODE_UNGRAB = 2 };

#define M_BUTTON_PRESS 0x0004u
#define M_BUTTON_RELEASE 0x0008u
#define M_ENTER 0x0010u
#define M_LEAVE 0x0020u
#define M_POINTER_MOTION 0x0040u
#define M_MOTION_HINT 0x0080u
#define M_BUTTON_MOTION 0x2000u
#define M_OWNER_GRAB_BUTTON 0x01000000u
#define ANY_MODIFIER 0x8000u
#define MAX_BUTTONS 5
/* A motion or a press needs this many free queue slots, so every release always fits. */
#define QUEUE_RESERVE (MAX_BUTTONS + 1)

static int earlier(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
/* Button1 -> 0x100 .. Button5 -> 0x1000; logical buttons above 5 (SetPointerMapping) have no
 * state bit in the core protocol. */
static uint16_t button_bit(int b) { return b >= 1 && b <= 5 ? (uint16_t)(0x80u << b) : 0; }

static x11_window *window_of(x11_server *s, uint32_t id) {
  x11_resource *r = id ? x_find(s, id) : 0;
  return r && r->type == X11_RES_WINDOW ? (x11_window *)r->obj : 0;
}

static int inside(const x11_window *w, const x11_window *ancestor) {  /* w is ancestor or an inferior of it */
  for (; w; w = w->parent)
    if (w == ancestor) return 1;
  return 0;
}

void x_input_init(x11_server *s) {
  for (int i = 0; i < 5; ++i) s->ptr_map[i] = (uint8_t)(i + 1);
  s->sprite = s->root;
  s->pgrab.client = -1;
  s->kgrab_client = -1;
}

/* ---- the window under the pointer --------------------------------------------------------- */

static x11_window *window_at(x11_server *s, int x, int y) {
  x11_window *w = s->root;
  for (;;) {
    int ax, ay;
    xw_abs(w, &ax, &ay);
    if (x < ax || y < ay || x >= ax + w->w || y >= ay + w->h) return w;  /* on w's border */
    x11_window *hit = 0;
    for (x11_window *c = w->last; c && !hit; c = c->below) {  /* topmost first */
      if (!c->mapped) continue;
      int cx, cy;
      xw_abs(c, &cx, &cy);
      if (x >= cx - c->bw && x < cx + c->w + c->bw && y >= cy - c->bw && y < cy + c->h + c->bw) hit = c;
    }
    if (!hit) return w;
    w = hit;
  }
}

x11_window *x_input_child(x11_server *s, x11_window *w) {
  for (x11_window *t = s->sprite; t; t = t->parent)
    if (t->parent == w) return t;
  return 0;
}

void x_input_reset_hint(x11_server *s, int c) { x_bit_clear(s->hints, c); }

/* ---- building and sending events ----------------------------------------------------------- */

static void pointer_fields(x11_server *s, uint8_t *ev, int msb, x11_window *w, uint32_t time, uint16_t state) {
  int ax, ay;
  xw_abs(w, &ax, &ay);
  x11_window *child = x_input_child(s, w);
  x_wr32(ev + 4, time, msb);
  x_wr32(ev + 8, s->root->id, msb);
  x_wr32(ev + 12, w->id, msb);
  x_wr32(ev + 16, child ? child->id : 0, msb);
  x_wr16(ev + 20, (uint16_t)s->ptr_x, msb), x_wr16(ev + 22, (uint16_t)s->ptr_y, msb);
  x_wr16(ev + 24, (uint16_t)(s->ptr_x - ax), msb), x_wr16(ev + 26, (uint16_t)(s->ptr_y - ay), msb);
  x_wr16(ev + 28, state, msb);
}

/* A press, release or motion event for client c on window w, selected with `mask`. */
static void send_device(x11_server *s, int c, x11_window *w, uint8_t type, uint8_t detail, uint32_t mask,
                        uint32_t time, uint16_t state) {
  if (type == MOTION_NOTIFY && (mask & M_MOTION_HINT)) {  /* one hint until the client asks */
    if (x_bit(s->hints, c)) return;
    x_bit_set(s->hints, c);
    detail = 1;  /* Hint */
  }
  uint8_t ev[32] = {type, detail};
  pointer_fields(s, ev, s->clients[c].msb, w, time, state);
  ev[30] = 1;  /* same-screen */
  x_send_event(s, c, ev);
}

/* Normal delivery: from the sprite towards the root, to the first window where a client
 * (only `only`, if >= 0) selected the event. Returns that client (the first one, for the
 * automatic grab) and the window, or -1. */
static int propagate(x11_server *s, int only, uint8_t type, uint8_t detail, uint32_t filter, uint32_t time,
                     uint16_t state, x11_window **at) {
  for (x11_window *w = s->sprite; w; w = w->parent) {
    int got = -1;
    XW_FOR_SEL(w, e) {
      int c = e->client;
      if (!s->clients[c].active || (only >= 0 && c != only) || !(e->mask & filter)) continue;
      send_device(s, c, w, type, detail, e->mask, time, state);
      if (got < 0) got = c;
    }
    if (got >= 0) {
      if (at) *at = w;
      return got;
    }
    if (w->dont_propagate & filter) break;
  }
  return -1;
}

/* Delivery during a pointer grab. Returns 1 if the grabbing client got the event. */
/* An event during a pointer grab: with owner-events, as normally to the grabbing client;
 * otherwise (or if that reports nothing) on the grab window if the grab's event-mask selects it.
 * always: report it on the grab window whatever the mask says: the ButtonPress that activates a
 * passive grab (protocol, GrabButton: "the ButtonPress event is reported"; X.Org reports it even
 * for a mask without ButtonPress, as mwm's click-to-focus grab has: ButtonRelease only). */
static int deliver_grabbed_as(x11_server *s, uint8_t type, uint8_t detail, uint32_t filter, uint32_t time,
                              uint16_t state, int always) {
  x11_pointer_grab *g = &s->pgrab;
  if (g->owner_events && propagate(s, g->client, type, detail, filter, time, state, 0) >= 0) return 1;
  x11_window *gw = window_of(s, g->window);
  if (!gw || (!always && !(g->event_mask & filter))) return 0;
  send_device(s, g->client, gw, type, detail, g->event_mask | (always ? filter : 0), time, state);
  return 1;
}

static int deliver_grabbed(x11_server *s, uint8_t type, uint8_t detail, uint32_t filter, uint32_t time,
                           uint16_t state) {
  return deliver_grabbed_as(s, type, detail, filter, time, state, 0);
}

static int deliver(x11_server *s, uint8_t type, uint8_t detail, uint32_t filter, uint32_t time, uint16_t state) {
  if (s->pgrab.client >= 0) return deliver_grabbed(s, type, detail, filter, time, state) ? s->pgrab.client : -1;
  return propagate(s, -1, type, detail, filter, time, state, 0);
}

/* One EnterNotify/LeaveNotify on w. Crossing events do not propagate; during a grab only
 * the grabbing client gets them (on the grab window by the grab mask, elsewhere only with
 * owner-events by its own selection). */
static void crossing_send(x11_server *s, int c, uint8_t type, uint8_t detail, uint8_t mode, x11_window *w,
                          uint32_t time, int focus) {
  uint8_t ev[32] = {type, detail};
  pointer_fields(s, ev, s->clients[c].msb, w, time, s->ptr_buttons);
  ev[30] = mode;
  ev[31] = (uint8_t)(0x02 | (focus ? 0x01 : 0));  /* same-screen, focus */
  x_send_event(s, c, ev);
}

static void crossing_one(x11_server *s, uint8_t type, uint8_t detail, uint8_t mode, x11_window *w) {
  uint32_t filter = type == ENTER_NOTIFY ? M_ENTER : M_LEAVE, time = x_now(s);
  const x11_pointer_grab *g = &s->pgrab;
  x11_window *fw = s->focus > 1 ? window_of(s, s->focus) : 0;
  int focus = s->focus == 1 || (fw && inside(w, fw));
  if (g->client >= 0) {  /* only the grabbing client */
    int c = g->client;
    uint32_t m = (w->id == g->window ? g->event_mask : 0) | (g->owner_events ? xw_mask(w, c) : 0);
    if (s->clients[c].active && (m & filter)) crossing_send(s, c, type, detail, mode, w, time, focus);
    return;
  }
  XW_FOR_SEL(w, e)
    if (s->clients[e->client].active && (e->mask & filter)) crossing_send(s, e->client, type, detail, mode, w, time, focus);
}

/* EnterNotify on the windows strictly between `top` and `w`, from the top down. */
static void enter_between(x11_server *s, x11_window *top, x11_window *w, uint8_t detail, uint8_t mode) {
  if (!w->parent || w->parent == top) return;
  enter_between(s, top, w->parent, detail, mode);
  crossing_one(s, ENTER_NOTIFY, detail, mode, w->parent);
}

/* The pointer (logically) moves from a to b. */
static void crossing(x11_server *s, x11_window *a, x11_window *b, uint8_t mode) {
  if (!a || !b || a == b) return;
  if (inside(b, a)) {         /* b is an inferior of a */
    crossing_one(s, LEAVE_NOTIFY, D_INFERIOR, mode, a);
    enter_between(s, a, b, D_VIRTUAL, mode);
    crossing_one(s, ENTER_NOTIFY, D_ANCESTOR, mode, b);
  } else if (inside(a, b)) {  /* b is an ancestor of a */
    crossing_one(s, LEAVE_NOTIFY, D_ANCESTOR, mode, a);
    for (x11_window *w = a->parent; w != b; w = w->parent) crossing_one(s, LEAVE_NOTIFY, D_VIRTUAL, mode, w);
    crossing_one(s, ENTER_NOTIFY, D_INFERIOR, mode, b);
  } else {
    x11_window *common = a->parent;
    while (common && !inside(b, common)) common = common->parent;
    crossing_one(s, LEAVE_NOTIFY, D_NONLINEAR, mode, a);
    for (x11_window *w = a->parent; w && w != common; w = w->parent)
      crossing_one(s, LEAVE_NOTIFY, D_NONLINEAR_VIRTUAL, mode, w);
    enter_between(s, common, b, D_NONLINEAR_VIRTUAL, mode);
    crossing_one(s, ENTER_NOTIFY, D_NONLINEAR, mode, b);
  }
}

/* ---- grabs -------------------------------------------------------------------------------- */

static void drain(x11_server *s);

static void clamp_to_confine(x11_server *s, int *x, int *y) {
  x11_window *cw = s->pgrab.client >= 0 ? window_of(s, s->pgrab.confine) : 0;
  if (!cw) return;
  int ax, ay;
  xw_abs(cw, &ax, &ay);
  if (*x < ax) *x = ax;
  if (*y < ay) *y = ay;
  if (*x >= ax + cw->w) *x = ax + cw->w - 1;
  if (*y >= ay + cw->h) *y = ay + cw->h - 1;
}

static void move_to(x11_server *s, int x, int y, uint32_t time);

static void activate(x11_server *s, const x11_pointer_grab *g, uint32_t time) {
  x11_window *from = s->pgrab.client >= 0 ? window_of(s, s->pgrab.window) : s->sprite;
  if (!from) from = s->sprite;
  s->pgrab.client = -1;  /* the Grab-mode crossings are delivered as without a grab */
  crossing(s, from, window_of(s, g->window), MODE_GRAB);
  s->pgrab = *g;
  s->pgrab_time = time;
  if (g->confine) {  /* confine-to: the pointer moves into the window */
    int x = s->ptr_x, y = s->ptr_y;
    clamp_to_confine(s, &x, &y);
    move_to(s, x, y, time);
  }
}

static void deactivate(x11_server *s) {
  if (s->pgrab.client < 0) return;
  x11_window *gw = window_of(s, s->pgrab.window);
  s->pgrab.client = -1;
  s->frozen = s->sync_next = s->replay_valid = 0;
  if (gw) crossing(s, gw, s->sprite, MODE_UNGRAB);
}

int x_grab_pointer(x11_server *s, int c, x11_window *w, int owner_events, uint16_t mask, int pointer_mode,
                   x11_window *confine, uint32_t cursor, uint32_t time) {
  uint32_t now = x_now(s), t = time ? time : now;
  if (s->pgrab.client >= 0 && s->pgrab.client != c) return X11_ALREADY_GRABBED;
  if (!xw_viewable(w) || (confine && !xw_viewable(confine))) return X11_GRAB_NOT_VIEWABLE;
  if (earlier(t, s->pgrab_time) || earlier(now, t)) return X11_GRAB_INVALID_TIME;
  if (s->frozen && s->pgrab.client != c) return X11_GRAB_FROZEN;
  x11_pointer_grab g = {(int16_t)c, X11_GRAB_ACTIVE, (uint8_t)owner_events, (uint8_t)pointer_mode, mask, w->id,
                        confine ? confine->id : 0, cursor};
  activate(s, &g, t);
  if (pointer_mode == 0) {  /* Synchronous: frozen until AllowEvents; nothing to replay */
    s->frozen = 1;
    s->replay_valid = 0;
  } else if (s->frozen) {   /* re-grabbed asynchronously by the same client: thaw */
    s->frozen = 0;
    drain(s);
  }
  return X11_GRAB_SUCCESS;
}

void x_ungrab_pointer(x11_server *s, int c, uint32_t time) {
  uint32_t now = x_now(s), t = time ? time : now;
  if (s->pgrab.client != c || earlier(t, s->pgrab_time) || earlier(now, t)) return;
  deactivate(s);
  drain(s);
}

void x_change_active_grab(x11_server *s, int c, uint16_t mask, uint32_t cursor, uint32_t time) {
  uint32_t now = x_now(s), t = time ? time : now;
  if (s->pgrab.client != c || earlier(t, s->pgrab_time) || earlier(now, t)) return;
  s->pgrab.event_mask = mask;
  s->pgrab.cursor = cursor;
}

static int overlaps(uint8_t b1, uint16_t m1, uint8_t b2, uint16_t m2) {
  return (!b1 || !b2 || b1 == b2) && (m1 == ANY_MODIFIER || m2 == ANY_MODIFIER || m1 == m2);
}

int x_grab_button(x11_server *s, int c, const x11_button_grab *g) {
  x11_button_grab *slot = 0;
  for (int i = 0; i < X11_MAX_BUTTON_GRABS; ++i) {
    x11_button_grab *e = &s->button_grabs[i];
    if (!e->window) {
      if (!slot) slot = e;
      continue;
    }
    if (e->window != g->window || !overlaps(e->button, e->modifiers, g->button, g->modifiers)) continue;
    if (e->client != c) return X11_BAD_ACCESS;
    if (e->button == g->button && e->modifiers == g->modifiers) slot = e;  /* the same grab: replaced */
  }
  if (!slot) return X11_BAD_ALLOC;
  *slot = *g;
  slot->client = (uint8_t)c;
  return 0;
}

void x_ungrab_button(x11_server *s, int c, uint32_t window, uint8_t button, uint16_t modifiers) {
  for (int i = 0; i < X11_MAX_BUTTON_GRABS; ++i) {
    x11_button_grab *e = &s->button_grabs[i];
    if (e->window == window && e->client == c && (!button || e->button == button) &&
        (modifiers == ANY_MODIFIER || e->modifiers == modifiers))
      memset(e, 0, sizeof(*e));
  }
}

int x_grab_keyboard(x11_server *s, int c, x11_window *w, uint32_t time) {
  uint32_t now = x_now(s), t = time ? time : now;
  if (s->kgrab_client >= 0 && s->kgrab_client != c) return X11_ALREADY_GRABBED;
  if (!xw_viewable(w)) return X11_GRAB_NOT_VIEWABLE;
  if (earlier(t, s->kgrab_time) || earlier(now, t)) return X11_GRAB_INVALID_TIME;
  s->kgrab_client = (int16_t)c;
  s->kgrab_window = w->id;
  s->kgrab_time = t;
  return X11_GRAB_SUCCESS;
}

void x_ungrab_keyboard(x11_server *s, int c, uint32_t time) {
  uint32_t now = x_now(s), t = time ? time : now;
  if (s->kgrab_client == c && !earlier(t, s->kgrab_time) && !earlier(now, t)) s->kgrab_client = -1;
}

/* ---- processing pointer events -------------------------------------------------------------- */

static void set_sprite(x11_server *s, x11_window *now) {
  if (now == s->sprite) return;
  x11_window *old = s->sprite;
  s->sprite = now;
  memset(s->hints, 0, sizeof(s->hints));  /* the pointer left the window of any pending hint */
  crossing(s, old, now, MODE_NORMAL);
}

static void move_to(x11_server *s, int x, int y, uint32_t time) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= s->root->w) x = s->root->w - 1;
  if (y >= s->root->h) y = s->root->h - 1;
  clamp_to_confine(s, &x, &y);
  if (x == s->ptr_x && y == s->ptr_y) return;
  s->ptr_x = (int16_t)x, s->ptr_y = (int16_t)y;
  set_sprite(s, window_at(s, x, y));
  /* Button1Motion..Button5Motion have the same bits as Button1Mask..Button5Mask */
  uint32_t filter = M_POINTER_MOTION | (s->ptr_buttons ? M_BUTTON_MOTION | s->ptr_buttons : 0);
  deliver(s, MOTION_NOTIFY, 0, filter, time, s->ptr_buttons);
}

/* The outermost passive grab for a press of `button` on the path from the root to the
 * sprite, below the window `skip` (ReplayPointer: grabs at or above it are ignored). */
static const x11_button_grab *find_button_grab(x11_server *s, int button, x11_window *skip) {
  x11_window *path[64];
  int n = 0;
  for (x11_window *w = s->sprite; w && n < 64; w = w->parent) path[n++] = w;
  int start = n - 1;
  if (skip)
    for (int i = 0; i < n; ++i)
      if (path[i] == skip) start = i - 1;
  for (int i = start; i >= 0; --i)
    for (int k = 0; k < X11_MAX_BUTTON_GRABS; ++k) {
      const x11_button_grab *g = &s->button_grabs[k];
      if (g->window != path[i]->id || (g->button && g->button != button)) continue;
      if (g->modifiers != ANY_MODIFIER && g->modifiers != 0) continue;  /* no keyboard: no modifiers down */
      if (!s->clients[g->client].active) continue;
      if (g->confine) {
        x11_window *cw = window_of(s, g->confine);
        if (!cw || !xw_viewable(cw)) continue;
      }
      return g;
    }
  return 0;
}

static void freeze_on(x11_server *s, const x11_device_event *e) {
  s->frozen = 1;
  s->sync_next = 0;
  s->replay = *e;
  s->replay_valid = 1;
}

static void press(x11_server *s, const x11_device_event *e, x11_window *skip) {
  uint16_t bit = button_bit(e->button), state = s->ptr_buttons;
  if (state & bit) return;
  s->ptr_buttons |= bit;
  if (!bit) ++s->hi_down;  /* a button above 5: no state bit, counted (one press per physical press) */
  memset(s->hints, 0, sizeof(s->hints));
  if (s->pgrab.client >= 0) {
    if (deliver_grabbed(s, BUTTON_PRESS, e->button, M_BUTTON_PRESS, e->time, state) && s->sync_next) freeze_on(s, e);
    return;
  }
  const x11_button_grab *bg = state ? 0 : find_button_grab(s, e->button, skip);  /* no other button down */
  if (bg) {
    x11_pointer_grab g = {(int16_t)bg->client, X11_GRAB_PASSIVE, bg->owner_events, bg->pointer_mode,
                          bg->event_mask, bg->window, bg->confine, bg->cursor};
    activate(s, &g, e->time);
    deliver_grabbed_as(s, BUTTON_PRESS, e->button, M_BUTTON_PRESS, e->time, state, 1);  /* always reported */
    if (g.pointer_mode == 0) freeze_on(s, e);
    return;
  }
  x11_window *at = 0;
  int c = propagate(s, -1, BUTTON_PRESS, e->button, M_BUTTON_PRESS, e->time, state, &at);
  if (c >= 0 && at) {  /* the automatic grab, as if by GrabButton with the client's selection */
    uint32_t m = xw_mask(at, c);
    x11_pointer_grab g = {(int16_t)c, X11_GRAB_AUTOMATIC, (uint8_t)((m & M_OWNER_GRAB_BUTTON) != 0), 1,
                          (uint16_t)m, at->id, 0, 0};
    activate(s, &g, e->time);
  }
}

static void release(x11_server *s, const x11_device_event *e) {
  uint16_t bit = button_bit(e->button), state = s->ptr_buttons;
  if (bit ? !(state & bit) : !s->hi_down) return;
  s->ptr_buttons &= (uint16_t)~bit;
  if (!bit) --s->hi_down;
  memset(s->hints, 0, sizeof(s->hints));
  if (s->pgrab.client >= 0) {
    if (deliver_grabbed(s, BUTTON_RELEASE, e->button, M_BUTTON_RELEASE, e->time, state) && s->sync_next)
      freeze_on(s, e);
    if (!s->ptr_buttons && !s->hi_down && s->pgrab.kind != X11_GRAB_ACTIVE) deactivate(s);  /* passive, automatic */
    return;
  }
  propagate(s, -1, BUTTON_RELEASE, e->button, M_BUTTON_RELEASE, e->time, state, 0);
}

static void process(x11_server *s, const x11_device_event *e, x11_window *skip) {
  move_to(s, e->x, e->y, e->time);
  if (e->type == BUTTON_PRESS) press(s, e, skip);
  else if (e->type == BUTTON_RELEASE) release(s, e);
}

static void drain(x11_server *s) {
  if (s->draining) return;
  s->draining = 1;
  while (!s->frozen && s->qlen) {
    x11_device_event e = s->queue[s->qhead];
    s->qhead = (uint8_t)((s->qhead + 1) % X11_INPUT_QUEUE);
    --s->qlen;
    process(s, &e, 0);
  }
  s->draining = 0;
}

void x_allow_events(x11_server *s, int c, int mode, uint32_t time) {
  uint32_t now = x_now(s), t = time ? time : now;
  if (s->pgrab.client != c || !s->frozen || earlier(t, s->pgrab_time) || earlier(now, t)) return;
  switch (mode) {
    case 0: case 6:  /* AsyncPointer, AsyncBoth */
      s->frozen = s->sync_next = 0;
      break;
    case 1: case 7:  /* SyncPointer, SyncBoth: until the next press or release reaches the client */
      s->frozen = 0;
      s->sync_next = 1;
      break;
    case 2: {        /* ReplayPointer */
      if (!s->replay_valid) return;
      x11_window *gw = window_of(s, s->pgrab.window);
      x11_device_event e = s->replay;
      uint16_t bit = button_bit(e.button);
      deactivate(s);
      /* reprocess the event as if the grab had not existed, without grabs at or above gw */
      if (e.type == BUTTON_PRESS) {  /* undo the press (its state bit, or the count above 5) */
        s->ptr_buttons &= (uint16_t)~bit;
        if (!bit && s->hi_down) --s->hi_down;
        press(s, &e, gw);
      } else if (e.type == BUTTON_RELEASE) {
        s->ptr_buttons |= bit;
        if (!bit) ++s->hi_down;
        release(s, &e);
      }
      break;
    }
    default:         /* keyboard modes: there is no keyboard */
      return;
  }
  drain(s);
}

/* button: the logical button (events carry it); phys: the physical one (1..5, 0 for motion),
 * which keys the dropped-press bookkeeping so a remapping never mismatches a release. */
static int enqueue(x11_server *s, uint8_t type, int button, int phys, int x, int y) {
  x11_device_event e = {type, (uint8_t)button, (int16_t)x, (int16_t)y, x_now(s)};
  uint16_t bit = phys ? (uint16_t)(1u << (phys - 1)) : 0;
  ++s->input_events;
  if (type == MOTION_NOTIFY && s->qlen) {  /* consecutive motions while frozen: keep the last */
    x11_device_event *last = &s->queue[(s->qhead + s->qlen - 1) % X11_INPUT_QUEUE];
    if (last->type == MOTION_NOTIFY) {
      *last = e;
      return 0;
    }
  }
  if (type == BUTTON_RELEASE && (s->swallowed & bit)) {  /* its press was dropped */
    s->swallowed &= (uint16_t)~bit;
    ++s->input_dropped;
    return 0;
  }
  int free_slots = X11_INPUT_QUEUE - s->qlen;
  if (!free_slots) {  /* not reached: the reserve always leaves room for pending releases */
    ++s->input_dropped;
    return 0;
  }
  if (type != BUTTON_RELEASE && free_slots < QUEUE_RESERVE) {
    if (type == BUTTON_PRESS) s->swallowed |= bit;
    ++s->input_dropped;  /* a dropped motion only loses an intermediate position */
    return 0;
  }
  s->queue[(s->qhead + s->qlen) % X11_INPUT_QUEUE] = e;
  ++s->qlen;
  return 1;
}

void x11_pointer_input(x11_server *s, int x, int y, unsigned buttons) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= s->root->w) x = s->root->w - 1;
  if (y >= s->root->h) y = s->root->h - 1;
  buttons &= (1u << MAX_BUTTONS) - 1;
  if (x != s->in_x || y != s->in_y) {
    s->in_x = (int16_t)x, s->in_y = (int16_t)y;
    enqueue(s, MOTION_NOTIFY, 0, 0, x, y);
  }
  for (int b = 1; b <= MAX_BUTTONS; ++b) {
    unsigned was = s->in_buttons >> (b - 1) & 1, now = buttons >> (b - 1) & 1;
    if (was == now) continue;
    int logical = s->ptr_map[b - 1];  /* SetPointerMapping; 0: the button is disabled */
    if (logical) enqueue(s, now ? BUTTON_PRESS : BUTTON_RELEASE, logical, b, x, y);
  }
  s->in_buttons = (uint16_t)buttons;
  drain(s);
}

void x_warp_pointer(x11_server *s, int x, int y) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= s->root->w) x = s->root->w - 1;
  if (y >= s->root->h) y = s->root->h - 1;
  s->in_x = (int16_t)x, s->in_y = (int16_t)y;
  enqueue(s, MOTION_NOTIFY, 0, 0, x, y);
  drain(s);
}

/* ---- window and client changes ------------------------------------------------------------ */

void x_input_update(x11_server *s) {
  if (s->pgrab.client >= 0) {  /* the grab ends when its window or confine-to window is not viewable */
    x11_window *gw = window_of(s, s->pgrab.window), *cw = window_of(s, s->pgrab.confine);
    if (!gw || !xw_viewable(gw) || (s->pgrab.confine && (!cw || !xw_viewable(cw)))) deactivate(s);
  }
  if (s->kgrab_client >= 0) {
    x11_window *kw = window_of(s, s->kgrab_window);
    if (!kw || !xw_viewable(kw)) s->kgrab_client = -1;
  }
  int x = s->ptr_x, y = s->ptr_y;
  clamp_to_confine(s, &x, &y);
  if (x != s->ptr_x || y != s->ptr_y) move_to(s, x, y, x_now(s));
  else set_sprite(s, window_at(s, x, y));
  drain(s);
}

void x_input_window_destroyed(x11_server *s, x11_window *w) {
  for (int i = 0; i < X11_MAX_BUTTON_GRABS; ++i)
    if (s->button_grabs[i].window == w->id) memset(&s->button_grabs[i], 0, sizeof(s->button_grabs[i]));
  if (s->sprite == w) s->sprite = w->parent ? w->parent : s->root;  /* not reached: unmapped first */
}

void x_input_client_gone(x11_server *s, int c) {
  for (int i = 0; i < X11_MAX_BUTTON_GRABS; ++i)
    if (s->button_grabs[i].window && s->button_grabs[i].client == c)
      memset(&s->button_grabs[i], 0, sizeof(s->button_grabs[i]));
  if (s->pgrab.client == c) deactivate(s);
  if (s->kgrab_client == c) s->kgrab_client = -1;
  x_bit_clear(s->hints, c);
  drain(s);
}
