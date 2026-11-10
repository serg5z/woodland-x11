/* Window tree, visible regions, background painting, exposures and structure events.
 * There is no backing store: a window's contents exist only where it is visible on the
 * framebuffer, and newly visible parts are painted with the background and announced
 * with Expose, as the protocol allows (backing-store NotUseful/Never). */
#include "x11_internal.h"

#define ROOT_BACKGROUND 0x2945u  /* RGB565 dark grey-blue */

x11_window *xw_root_create(x11_server *s) {
  x11_window *r = (x11_window *)x_alloc(s, sizeof(x11_window));
  if (!r) return 0;
  memset(r, 0, sizeof(*r));
  r->id = X11_ID_ROOT_WINDOW;
  r->owner = 0xFF;
  r->w = s->cfg.width_px;
  r->h = s->cfg.height_px;
  r->mapped = 1;
  r->bg_mode = 1;
  r->bg_pixel = ROOT_BACKGROUND;
  r->colormap = X11_ID_DEFAULT_COLORMAP;
  r->win_gravity = 1;
  r->backing_planes = 0xFFFFFFFFu;
  return r;
}

/* Server reset: the root's standard background and cursor again, repainted. */
void xw_root_reset(x11_server *s) {
  x11_window *r = s->root;
  xw_tile_free(s, &r->bg_tile);
  r->bg_mode = 1;
  r->bg_pixel = ROOT_BACKGROUND;
  r->cursor = 0;
  r->colormap = X11_ID_DEFAULT_COLORMAP;
  xw_clear(s, r, 0, 0);
}

void xw_abs(const x11_window *w, int *ax, int *ay) {
  int x = 0, y = 0;
  for (; w && w->parent; w = w->parent) {
    x += w->x + w->bw;
    y += w->y + w->bw;
  }
  *ax = x, *ay = y;
}

static void outer_rect(const x11_window *w, xr_rect *r) {
  int ax, ay;
  xw_abs(w, &ax, &ay);
  r->x1 = (int16_t)(ax - w->bw), r->y1 = (int16_t)(ay - w->bw);
  r->x2 = (int16_t)(ax + w->w + w->bw), r->y2 = (int16_t)(ay + w->h + w->bw);
}

static void inner_rect(const x11_window *w, xr_rect *r) {
  int ax, ay;
  xw_abs(w, &ax, &ay);
  r->x1 = (int16_t)ax, r->y1 = (int16_t)ay, r->x2 = (int16_t)(ax + w->w), r->y2 = (int16_t)(ay + w->h);
}

int xw_viewable(const x11_window *w) {
  for (; w; w = w->parent)
    if (!w->mapped) return 0;
  return 1;
}

/* Clip by every ancestor's inside and by mapped siblings stacked above at each level. */
static void clip_ancestors(const x11_window *w, xr_region *g) {
  for (const x11_window *c = w; c->parent; c = c->parent) {
    xr_rect pr;
    inner_rect(c->parent, &pr);
    xr_region_intersect(g, &pr);
    for (const x11_window *sib = c->above; sib; sib = sib->above) {
      if (!sib->mapped || sib->input_only) continue;
      xr_rect o;
      outer_rect(sib, &o);
      xr_region_subtract(g, &o);
    }
  }
}

void xw_visible(const x11_window *w, xr_region *out, int include_inferiors) {
  xr_region_clear(out);
  if (!xw_viewable(w)) return;
  xr_rect r;
  inner_rect(w, &r);
  xr_region_set(out, r.x1, r.y1, r.x2, r.y2);
  clip_ancestors(w, out);
  if (!include_inferiors)
    for (const x11_window *c = w->first; c; c = c->above) {
      if (!c->mapped || c->input_only) continue;
      xr_rect o;
      outer_rect(c, &o);
      xr_region_subtract(out, &o);
    }
}

xr_surface *xw_tile_copy(x11_server *s, const xr_surface *src) {
  size_t bpp = src->depth == 16 ? 2 : 1, row = (size_t)src->w * bpp;
  xr_surface *t = (xr_surface *)x_alloc(s, sizeof(xr_surface));
  void *pix = x_alloc(s, row * (size_t)src->h);
  if (!t || !pix) {
    x_free(s, t);
    x_free(s, pix);
    return 0;
  }
  for (int y = 0; y < src->h; ++y)
    memcpy((uint8_t *)pix + row * (size_t)y, (const uint8_t *)src->pix + (size_t)src->stride * bpp * (size_t)y, row);
  t->pix = pix, t->w = src->w, t->h = src->h, t->stride = src->w, t->depth = src->depth, t->dmg = 0;
  return t;
}

void xw_tile_free(x11_server *s, xr_surface **tile) {
  if (!*tile) return;
  x_free(s, (*tile)->pix);
  x_free(s, *tile);
  *tile = 0;
}

/* The window whose background shows in w (ParentRelative walks up), or NULL for None. */
static const x11_window *background(const x11_window *w) {
  while (w->bg_mode == 2 && w->parent) w = w->parent;
  return w->bg_mode == 0 || w->bg_mode == 2 ? 0 : w;
}

/* Fill with a pixel, or with a tile whose origin is the inside origin of `org`. */
static void fill_region(x11_server *s, const xr_region *g, uint32_t pixel, const xr_surface *tile,
                        const x11_window *org) {
  xr_paint p = {3, 0xFFFFFFFFu, pixel, 0};
  int ox = 0, oy = 0;
  if (tile) xw_abs(org, &ox, &oy);
  for (int i = 0; i < g->n; ++i) {
    int x = g->r[i].x1, y = g->r[i].y1, w = g->r[i].x2 - x, h = g->r[i].y2 - y;
    if (tile) xr_fill_tile(&s->fb, g, &p, tile, ox, oy, x, y, w, h);
    else xr_fill_rect(&s->fb, g, &p, x, y, w, h);
  }
  if (g->n) ++s->fb_rev;
}

static void send_expose(x11_server *s, x11_window *w, const xr_region *vis) {
  int ax, ay;
  xw_abs(w, &ax, &ay);
  for (int i = 0; i < vis->n; ++i) {
    uint8_t ev[32];
    const xr_rect *r = &vis->r[i];
    XW_FOR_SEL(w, e) {
      int c = e->client;
      if (!s->clients[c].active || !(e->mask & X_EV_EXPOSURE)) continue;
      int msb = s->clients[c].msb;
      memset(ev, 0, 32);
      ev[0] = 12;
      x_wr32(ev + 4, w->id, msb);
      x_wr16(ev + 8, (uint16_t)(r->x1 - ax), msb);
      x_wr16(ev + 10, (uint16_t)(r->y1 - ay), msb);
      x_wr16(ev + 12, (uint16_t)(r->x2 - r->x1), msb);
      x_wr16(ev + 14, (uint16_t)(r->y2 - r->y1), msb);
      x_wr16(ev + 16, (uint16_t)(vis->n - 1 - i), msb);  /* count: how many more follow */
      x_send_event(s, c, ev);
    }
  }
}

void xw_clear(x11_server *s, x11_window *w, const xr_region *area, int send_expose_events) {
  if (w->input_only) return;
  int m = xr_pool_mark();
  xr_region *vis = xr_pool_get();
  if (!vis) return;  /* pool exhausted: counted; nothing painted or exposed */
  xw_visible(w, vis, 0);
  if (area) xr_region_intersect_region(vis, area);
  if (!xr_region_empty(vis)) {
    const x11_window *b = background(w);
    if (b) fill_region(s, vis, b->bg_pixel, b->bg_mode == 3 ? b->bg_tile : 0, b);
    if (send_expose_events) send_expose(s, w, vis);
  }
  xr_pool_release(m);
}

static void paint_border(x11_server *s, x11_window *w, const xr_region *area) {
  if (!w->bw || w->input_only || !xw_viewable(w)) return;
  int m = xr_pool_mark();
  xr_region *g = xr_pool_get();
  if (!g) return;
  xr_rect o, in;
  outer_rect(w, &o);
  inner_rect(w, &in);
  xr_region_set(g, o.x1, o.y1, o.x2, o.y2);
  clip_ancestors(w, g);
  xr_region_subtract(g, &in);
  if (area) xr_region_intersect_region(g, area);
  fill_region(s, g, w->border_pixel, w->border_tile, w);  /* border tile origin = background's */
  xr_pool_release(m);
}

/* Paint and expose w and its viewable inferiors inside `area`. */
/* ChangeWindowAttributes border-pixel / border-pixmap: the border is repainted at once
 * (protocol); only its visible part (ancestors and siblings above clip it). */
void xw_repaint_border(x11_server *s, x11_window *w) { paint_border(s, w, 0); }

static void expose_tree(x11_server *s, x11_window *w, const xr_region *area) {
  if (!xw_viewable(w)) return;
  paint_border(s, w, area);
  xw_clear(s, w, area, 1);
  for (x11_window *c = w->first; c; c = c->above)
    if (c->mapped) expose_tree(s, c, area);
}

/* After `gone` (a child of parent) stopped covering `area`: repaint what became visible:
 * the parent's own inside and the siblings stacked below `gone`. Windows above it were
 * visible before and keep their contents. gone == NULL: all children (after a restack). */
static void expose_below(x11_server *s, x11_window *parent, x11_window *gone, const xr_region *area) {
  if (!parent || !xw_viewable(parent)) return;
  xw_clear(s, parent, area, 1);
  for (x11_window *c = parent->first; c && c != gone; c = c->above)
    if (c->mapped) expose_tree(s, c, area);
}

static void structure_event(x11_server *s, x11_window *w, uint8_t code, int fill(uint8_t *ev, int msb, void *a),
                            void *arg) {
  /* to the window itself (StructureNotify), then to its parent (SubstructureNotify) */
  for (int pass = 0; pass < 2; ++pass) {
    x11_window *target = pass == 0 ? w : w->parent;
    uint32_t mask = pass == 0 ? X_EV_STRUCTURE : X_EV_SUBSTRUCTURE;
    if (!target) continue;
    XW_FOR_SEL(target, e) {
      int c = e->client;
      if (!s->clients[c].active || !(e->mask & mask)) continue;
      uint8_t ev[32];
      int msb = s->clients[c].msb;
      memset(ev, 0, 32);
      ev[0] = code;
      x_wr32(ev + 4, target->id, msb);  /* event window */
      x_wr32(ev + 8, w->id, msb);
      fill(ev, msb, arg);
      x_send_event(s, c, ev);
    }
  }
}

static int fill_map(uint8_t *ev, int msb, void *a) {
  (void)msb;
  ev[12] = ((x11_window *)a)->override_redirect;
  return 0;
}
static int fill_unmap(uint8_t *ev, int msb, void *a) {
  (void)msb, (void)a;
  ev[12] = 0;  /* from-configure False */
  return 0;
}
static int fill_destroy(uint8_t *ev, int msb, void *a) {
  (void)ev, (void)msb, (void)a;
  return 0;
}
static int fill_configure(uint8_t *ev, int msb, void *a) {
  x11_window *w = (x11_window *)a;
  x_wr32(ev + 12, w->below ? w->below->id : 0, msb);  /* above-sibling: the sibling below w */
  x_wr16(ev + 16, (uint16_t)w->x, msb);
  x_wr16(ev + 18, (uint16_t)w->y, msb);
  x_wr16(ev + 20, w->w, msb);
  x_wr16(ev + 22, w->h, msb);
  x_wr16(ev + 24, w->bw, msb);
  ev[26] = w->override_redirect;
  return 0;
}

static void focus_event(x11_server *s, uint32_t win, uint8_t code) {
  x11_resource *r = x_find(s, win);
  if (!r || r->type != X11_RES_WINDOW) return;
  x11_window *w = (x11_window *)r->obj;
  XW_FOR_SEL(w, e) {
    int c = e->client;
    if (!s->clients[c].active || !(e->mask & X_EV_FOCUS)) continue;
    uint8_t ev[32] = {code, 3 /* detail: Nonlinear */};
    x_wr32(ev + 4, w->id, s->clients[c].msb);
    x_send_event(s, c, ev);
  }
}

void xw_set_focus(x11_server *s, uint32_t f, uint8_t revert_to) {
  if (f != s->focus) {
    focus_event(s, s->focus, 10);  /* FocusOut */
    s->focus = f;
    focus_event(s, f, 9);          /* FocusIn */
  }
  s->focus_revert = revert_to;
}

/* `w` stopped being viewable (unmapped, or about to be destroyed): if the focus window is
 * w or one of its inferiors, the focus reverts as SetInputFocus's revert-to says. */
static void focus_lost(x11_server *s, x11_window *w) {
  x11_resource *r = s->focus > 1 ? x_find(s, s->focus) : 0;
  if (!r || r->type != X11_RES_WINDOW) return;
  x11_window *f = (x11_window *)r->obj, *a = f;
  while (a && a != w) a = a->parent;
  if (!a) return;  /* not inside w */
  if (s->focus_revert == 2) {  /* Parent: the closest viewable ancestor, then revert-to None */
    x11_window *p = w->parent;
    while (p && !xw_viewable(p)) p = p->parent;
    xw_set_focus(s, p ? p->id : 1, 0);
  } else {
    xw_set_focus(s, s->focus_revert == 1 ? 1 : 0, s->focus_revert);  /* PointerRoot or None */
  }
}

int xw_redirect_client(const x11_server *s, const x11_window *w, uint32_t bit, int except) {
  XW_FOR_SEL(w, e)
    if (e->client != except && s->clients[e->client].active && (e->mask & bit)) return e->client;
  return -1;
}

int xw_select(x11_server *s, x11_window *w, int c, uint32_t mask) {
  x11_sel **pp = &w->sels;
  while (*pp && (*pp)->client < c) pp = &(*pp)->next;
  x11_sel *e = *pp && (*pp)->client == c ? *pp : 0;
  if (!mask) {  /* remove */
    if (e) {
      *pp = e->next;
      x_free(s, e);
    }
    return 0;
  }
  if (!e) {
    e = (x11_sel *)x_alloc(s, sizeof(*e));
    if (!e) return -1;
    e->client = (uint8_t)c;
    e->next = *pp;
    *pp = e;
  }
  e->mask = mask;
  return 0;
}

void xw_free_sels(x11_server *s, x11_window *w) {
  while (w->sels) {
    x11_sel *e = w->sels;
    w->sels = e->next;
    x_free(s, e);
  }
}

void xw_link_top(x11_window *parent, x11_window *w) {
  w->parent = parent;
  w->above = 0;
  w->below = parent->last;
  if (parent->last) parent->last->above = w;
  else parent->first = w;
  parent->last = w;
}

void xw_unlink(x11_window *w) {
  x11_window *p = w->parent;
  if (!p) return;
  if (w->below) w->below->above = w->above;
  else p->first = w->above;
  if (w->above) w->above->below = w->below;
  else p->last = w->below;
  w->above = w->below = 0;
}

static void link_bottom(x11_window *parent, x11_window *w) {
  w->parent = parent;
  w->below = 0;
  w->above = parent->first;
  if (parent->first) parent->first->below = w;
  else parent->last = w;
  parent->first = w;
}

void xw_map(x11_server *s, x11_window *w) {
  if (w->mapped || !w->parent) return;
  w->mapped = 1;
  structure_event(s, w, 19, fill_map, w);  /* MapNotify */
  if (!xw_viewable(w)) return;
  xr_rect o;
  outer_rect(w, &o);
  int m = xr_pool_mark();
  xr_region *area = xr_pool_get();
  if (area) {
    xr_region_set(area, o.x1, o.y1, o.x2, o.y2);
    expose_tree(s, w, area);
  }
  xr_pool_release(m);
  x_input_update(s);  /* the pointer may now be in w: crossing events */
}

void xw_unmap(x11_server *s, x11_window *w, int send_events) {
  if (!w->mapped || !w->parent) return;
  int was_viewable = xw_viewable(w);
  xr_rect o;
  outer_rect(w, &o);
  w->mapped = 0;
  if (send_events) structure_event(s, w, 18, fill_unmap, w);  /* UnmapNotify */
  if (!was_viewable) return;
  focus_lost(s, w);
  int m = xr_pool_mark();
  xr_region *area = xr_pool_get();
  if (area) {
    xr_region_set(area, o.x1, o.y1, o.x2, o.y2);
    expose_below(s, w->parent, w, area);
  }
  xr_pool_release(m);
  /* while w still exists: grabs on it end, the pointer leaves it (crossing events) */
  x_input_update(s);
}

static void destroy_subtree(x11_server *s, x11_window *w) {
  while (w->first) destroy_subtree(s, w->first);  /* inferiors first */
  structure_event(s, w, 17, fill_destroy, w);     /* DestroyNotify */
  x_input_window_destroyed(s, w);
  x_prop_free_all(s, w);
  xw_tile_free(s, &w->bg_tile);
  xw_tile_free(s, &w->border_tile);
  xw_free_sels(s, w);
  x_saveset_forget(s, w->id);
  x11_resource *r = x_find(s, w->id);
  if (r) x_res_remove(s, r);
  xw_unlink(w);
  if (s->focus == w->id) s->focus = 1;            /* only if never viewable: unmap reverted it */
  x_free(s, w);
}

void xw_destroy(x11_server *s, x11_window *w) {
  if (!w->parent) return;  /* the root is never destroyed */
  xw_unmap(s, w, 1);
  destroy_subtree(s, w);
}

static void insert_above(x11_window *sib, x11_window *w) {
  x11_window *p = sib->parent;
  w->parent = p;
  w->below = sib;
  w->above = sib->above;
  if (sib->above) sib->above->below = w;
  else p->last = w;
  sib->above = w;
}

static void insert_below(x11_window *sib, x11_window *w) {
  x11_window *p = sib->parent;
  w->parent = p;
  w->above = sib;
  w->below = sib->below;
  if (sib->below) sib->below->above = w;
  else p->first = w;
  sib->below = w;
}

void xw_configure(x11_server *s, x11_window *w, int x, int y, int wd, int ht, int bw, int stack, x11_window *sib) {
  int viewable = xw_viewable(w);
  xr_rect old;
  outer_rect(w, &old);
  x11_window *old_below = w->below;
  int changed = x != w->x || y != w->y || wd != w->w || ht != w->h || bw != w->bw;
  w->x = (int16_t)x, w->y = (int16_t)y, w->w = (uint16_t)wd, w->h = (uint16_t)ht, w->bw = (uint16_t)bw;
  if (stack != XW_STACK_NONE && sib != w) {
    x11_window *p = w->parent;
    xw_unlink(w);
    if (stack == XW_STACK_ABOVE) sib ? insert_above(sib, w) : xw_link_top(p, w);
    else sib ? insert_below(sib, w) : link_bottom(p, w);
    changed |= w->below != old_below;
  }
  structure_event(s, w, 22, fill_configure, w);  /* ConfigureNotify */
  if (!changed || !viewable) return;
  /* No bit gravity is kept: everything the window covered before is repainted below it,
   * and the window (with its inferiors) is painted and exposed in full at its new place. */
  int m = xr_pool_mark();
  xr_region *area = xr_pool_get();
  if (area) {
    xr_region_set(area, old.x1, old.y1, old.x2, old.y2);
    expose_below(s, w->parent, 0, area);  /* siblings may have changed order: repaint them all there */
    xr_rect now;
    outer_rect(w, &now);
    xr_region_set(area, now.x1, now.y1, now.x2, now.y2);
    expose_tree(s, w, area);
  }
  xr_pool_release(m);
  x_input_update(s);  /* moved, resized or restacked under the pointer */
}

static int fill_circulate(uint8_t *ev, int msb, void *a) {
  (void)msb;
  ev[16] = (uint8_t)(uintptr_t)a;  /* place: 0 Top, 1 Bottom */
  return 0;
}

/* CirculateWindow's restack of child w to the top (place 0) or bottom (1) of its siblings:
 * CirculateNotify, then the area w covers is repainted as after a ConfigureWindow restack. */
void xw_circulate(x11_server *s, x11_window *w, int place) {
  x11_window *p = w->parent;
  xw_unlink(w);
  if (place == 0) xw_link_top(p, w);
  else link_bottom(p, w);
  structure_event(s, w, 26, fill_circulate, (void *)(uintptr_t)place);  /* CirculateNotify */
  if (!xw_viewable(w)) return;
  int m = xr_pool_mark();
  xr_region *area = xr_pool_get();
  if (area) {
    xr_rect o;
    outer_rect(w, &o);
    xr_region_set(area, o.x1, o.y1, o.x2, o.y2);
    expose_below(s, p, 0, area);  /* every child in the area, bottom to top, in the new order */
  }
  xr_pool_release(m);
  x_input_update(s);
}

void xw_reparent(x11_server *s, x11_window *w, x11_window *np, int x, int y) {
  if (w->mapped) xw_unmap(s, w, 1);
  x11_window *old = w->parent;
  xw_unlink(w);
  xw_link_top(np, w);
  w->x = (int16_t)x, w->y = (int16_t)y;
  /* ReparentNotify: the window (StructureNotify), old and new parent (SubstructureNotify) */
  x11_window *targets[3] = {w, old, np == old ? 0 : np};
  for (int t = 0; t < 3; ++t) {
    if (!targets[t]) continue;
    uint32_t mask = t == 0 ? X_EV_STRUCTURE : X_EV_SUBSTRUCTURE;
    XW_FOR_SEL(targets[t], e) {
      int c = e->client;
      if (!s->clients[c].active || !(e->mask & mask)) continue;
      int msb = s->clients[c].msb;
      uint8_t ev[32] = {21};
      x_wr32(ev + 4, targets[t]->id, msb);
      x_wr32(ev + 8, w->id, msb);
      x_wr32(ev + 12, np->id, msb);
      x_wr16(ev + 16, (uint16_t)w->x, msb), x_wr16(ev + 18, (uint16_t)w->y, msb);
      ev[20] = w->override_redirect;
      x_send_event(s, c, ev);
    }
  }
}
