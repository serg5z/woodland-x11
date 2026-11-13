/* X11 requests after setup, see x11_server.h. Encodings follow the protocol specification,
 * "Encoding: Requests", "Events", "Errors" and "Replies". */
#include <stdio.h>

#include "x11_font.h"
#include "x11_colors.h"
#include "x11_internal.h"
#ifdef X11_LATENCY_TRACE
#include "../net/x11_latency_trace.h"
#endif

/* ---- predefined atoms (protocol appendix "Predefined Atoms") -------------------------- */

static const char *const PREDEFINED[X11_PREDEFINED_ATOMS + 1] = {
    0, "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR", "CUT_BUFFER0",
    "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7",
    "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP",
    "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING",
    "VISUALID", "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE",
    "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE",
    "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH",
    "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME", "FAMILY_NAME", "FULL_NAME",
    "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"};

/* An atom's name and length (names may contain NUL bytes), or NULL if the atom does not exist. */
static const char *atom_get(const x11_server *s, uint32_t atom, size_t *len) {
  if (atom >= 1 && atom <= X11_PREDEFINED_ATOMS) {
    *len = strlen(PREDEFINED[atom]);
    return PREDEFINED[atom];
  }
  if (atom > X11_PREDEFINED_ATOMS && atom <= (uint32_t)(X11_PREDEFINED_ATOMS + s->atom_count)) {
    int i = (int)(atom - X11_PREDEFINED_ATOMS - 1);
    *len = s->atom_len[i];
    return s->atom_pool + s->atom_off[i];
  }
  return 0;
}

const char *x11_atom_name(const x11_server *s, uint32_t atom) {
  size_t len;
  return atom_get(s, atom, &len);
}

static uint32_t atom_lookup(const x11_server *s, const uint8_t *name, size_t n) {
  for (uint32_t a = 1; a <= (uint32_t)(X11_PREDEFINED_ATOMS + s->atom_count); ++a) {
    size_t len;
    const char *v = atom_get(s, a, &len);
    if (len == n && memcmp(v, name, n) == 0) return a;
  }
  return 0;
}

static uint32_t atom_intern(x11_server *s, const uint8_t *name, size_t n);
typedef char atom_pool_fits_uint16[X11_ATOM_POOL <= 65535 ? 1 : -1];  /* atom_off, atom_len */

/* ---- platform ------------------------------------------------------------------------- */

void *x_alloc(x11_server *s, size_t n) {
  void *p = s->plat.alloc ? s->plat.alloc(s->plat.ctx, n) : malloc(n);
  if (!p) s->oom = 1;
  return p;
}

void x_free(x11_server *s, void *p) {
  if (!p) return;
  if (s->plat.release) s->plat.release(s->plat.ctx, p);
  else free(p);
}

uint32_t x_now(x11_server *s) { return s->plat.now_ms ? s->plat.now_ms(s->plat.ctx) : 0; }

void x_send_event(x11_server *s, int client, uint8_t *ev) {
  if (!s->clients[client].active || !s->plat.deliver) return;
  x_wr16(ev + 2, s->clients[client].seq, s->clients[client].msb);
  s->plat.deliver(s->plat.ctx, client, ev, 32);
}

/* ---- damage --------------------------------------------------------------------------- */

static long area(const xr_rect *r) { return (long)(r->x2 - r->x1) * (r->y2 - r->y1); }

static xr_rect unite(const xr_rect *a, const xr_rect *b) {
  xr_rect u = {a->x1 < b->x1 ? a->x1 : b->x1, a->y1 < b->y1 ? a->y1 : b->y1, a->x2 > b->x2 ? a->x2 : b->x2,
               a->y2 > b->y2 ? a->y2 : b->y2};
  return u;
}

/* Adds a changed rectangle: merged with an existing one when that costs little extra area
 * (nearby or overlapping), else appended; when the list is full, merged where it grows the
 * least. Never loses an area: the union always covers everything that changed. */
static void damage_add(x11_server *s, xr_rect r) {
  for (int i = 0; i < s->ndamage; ++i) {
    xr_rect u = unite(&s->damage[i], &r);
    if (area(&u) <= area(&s->damage[i]) + area(&r) + 32 * 32) {
      s->damage[i] = s->damage[--s->ndamage];  /* merge, and re-add (may merge further) */
      damage_add(s, u);
      return;
    }
  }
  if (s->ndamage < X11_DAMAGE_RECTS) {
    s->damage[s->ndamage++] = r;
    return;
  }
  int best = 0;
  long grow = -1;
  for (int i = 0; i < s->ndamage; ++i) {
    xr_rect u = unite(&s->damage[i], &r);
    long g = area(&u) - area(&s->damage[i]);
    if (grow < 0 || g < grow) grow = g, best = i;
  }
  xr_rect u = unite(&s->damage[best], &r);
  s->damage[best] = s->damage[--s->ndamage];
  damage_add(s, u);
}

/* Moves the current request's written area into the damage list. */
static void damage_commit(x11_server *s) {
  xr_damage *d = &s->fb_op;
  if (d->x1 < d->x2 && d->y1 < d->y2) {
    xr_rect r = {(int16_t)d->x1, (int16_t)d->y1, (int16_t)d->x2, (int16_t)d->y2};
    damage_add(s, r);
  }
  xr_damage_reset(d);
}

int x11_take_damage(x11_server *s, xr_rect *out, int max) {
  damage_commit(s);
  while (s->ndamage > max && s->ndamage > 1) {  /* caller wants fewer: merge the last two */
    xr_rect u = unite(&s->damage[s->ndamage - 2], &s->damage[s->ndamage - 1]);
    --s->ndamage;
    s->damage[s->ndamage - 1] = u;
  }
  int n = s->ndamage < max ? s->ndamage : max;
  memcpy(out, s->damage, sizeof(xr_rect) * (size_t)n);
  s->ndamage = 0;
  return n;
}

/* ---- resources ------------------------------------------------------------------------ */

/* The index: open addressing with linear probing over entry numbers + 1 (0 empty, 0xFFFF a
 * deleted slot). Its size is a power of two at least twice the table, so a probe sequence
 * always ends at an empty slot; it is rebuilt when deleted slots pile up. */
#define RINDEX_EMPTY 0u
#define RINDEX_DELETED 0xFFFFu

static uint32_t rhash(uint32_t id) {
  id ^= id >> 16;
  id *= 0x7FEB352Du;
  id ^= id >> 15;
  return id;
}

x11_resource *x_find(x11_server *s, uint32_t id) {
  if (!id) return 0;
  for (uint32_t h = rhash(id) & s->rindex_mask;; h = (h + 1) & s->rindex_mask) {
    uint16_t e = s->rindex[h];
    if (e == RINDEX_EMPTY) return 0;
    if (e != RINDEX_DELETED && s->resources[e - 1].id == id && s->resources[e - 1].type) return &s->resources[e - 1];
  }
}

static void rindex_insert(x11_server *s, int entry) {
  uint32_t h = rhash(s->resources[entry].id) & s->rindex_mask;
  while (s->rindex[h] != RINDEX_EMPTY && s->rindex[h] != RINDEX_DELETED) h = (h + 1) & s->rindex_mask;
  s->rindex[h] = (uint16_t)(entry + 1);
}

static void rindex_rebuild(x11_server *s) {
  memset(s->rindex, 0, sizeof(uint16_t) * (s->rindex_mask + 1));
  s->rindex_deleted = 0;
  for (int i = 0; i < s->max_resources; ++i)
    if (s->resources[i].type) rindex_insert(s, i);
}

x11_resource *x_add(x11_server *s, uint32_t id, int owner, int type, void *obj) {
  if (s->resources_used >= s->max_resources) return 0;
  if (owner != 0xFF && s->cfg.max_resources_per_client && s->clients[owner].resources >= s->cfg.max_resources_per_client)
    return 0;
  for (int n = 0; n < s->max_resources; ++n) {
    int i = (s->res_hint + n) % s->max_resources;
    x11_resource *r = &s->resources[i];
    if (r->type) continue;
    r->id = id, r->owner = (uint8_t)owner, r->type = (uint8_t)type, r->obj = obj;
    rindex_insert(s, i);
    ++s->resources_used;
    if (owner != 0xFF) ++s->clients[owner].resources;
    s->res_hint = (i + 1) % s->max_resources;
    return r;
  }
  return 0;
}

void x_res_remove(x11_server *s, x11_resource *r) {
  uint16_t entry = (uint16_t)(r - s->resources + 1);
  for (uint32_t h = rhash(r->id) & s->rindex_mask;; h = (h + 1) & s->rindex_mask) {
    if (s->rindex[h] == RINDEX_EMPTY) break;  /* not indexed (cannot happen) */
    if (s->rindex[h] == entry) {
      s->rindex[h] = RINDEX_DELETED;
      ++s->rindex_deleted;
      break;
    }
  }
  if (r->owner != 0xFF && s->clients[r->owner].resources) {
    --s->clients[r->owner].resources;
    if (!s->clients[r->owner].resources && s->clients[r->owner].retained) s->retained_empty = 1;
  }
  --s->resources_used;
  memset(r, 0, sizeof(*r));
  if (s->rindex_deleted > (s->rindex_mask + 1) / 4) rindex_rebuild(s);
}

int x11_resources_used(const x11_server *s) {
  return s->resources_used - 1;  /* without the root window */
}

size_t x11_server_stats(const x11_server *s, char *out, size_t cap) {
  int active = 0, saves = 0;
  uint32_t requests = 0, errors = 0;
  for (int c = 0; c < X11_SERVER_CLIENTS; ++c) {
    active += s->clients[c].active;
    requests += s->clients[c].requests;
    errors += s->clients[c].errors;
  }
  for (int i = 0; i < X11_MAX_SAVE_SET; ++i) saves += s->save_sets[i].window != 0;
  int retained = 0;
  for (int c = 0; c < X11_SERVER_CLIENTS; ++c) retained += s->clients[c].retained != 0;
  int n = snprintf(out, cap,
                   "core.clients_compiled=%d\ncore.clients_max=%d\ncore.clients_open=%d\ncore.clients_active=%d\n"
                   "core.requests_open_clients=%lu\ncore.errors_open_clients=%lu\n"
                   "core.resources=%d\ncore.resources_max=%d\ncore.resources_per_client_max=%u\n"
                   "core.atoms=%d\ncore.atoms_max=%d\ncore.atom_bytes=%lu\ncore.atom_bytes_max=%d\n"
                   "core.save_set=%d\ncore.alloc_failed=%u\n"
                   "core.clients_retained=%d\ncore.clients_retained_max=%d\ncore.retain_refused=%lu\n"
                   "core.input_events=%lu\ncore.input_dropped=%lu\n"
                   "core.region_pool=%d\ncore.region_pool_peak=%d\ncore.region_pool_failures=%lu\n",
                   X11_SERVER_CLIENTS, s->max_clients, s->nclients, active, (unsigned long)requests,
                   (unsigned long)errors, x11_resources_used(s), s->max_resources - 1,
                   (unsigned)s->cfg.max_resources_per_client, s->atom_count, X11_MAX_DYNAMIC_ATOMS,
                   (unsigned long)s->atom_pool_used, X11_ATOM_POOL, saves,
                   (unsigned)s->oom, retained, X11_MAX_RETAINED, (unsigned long)s->retain_refused, (unsigned long)s->input_events, (unsigned long)s->input_dropped,
                   XR_POOL_REGIONS, s->pool.peak, (unsigned long)s->pool.failures);
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}

/* ---- save-sets ------------------------------------------------------------------------ */

static int saveset_has(const x11_server *s, int c, uint32_t w) {
  for (int i = 0; i < X11_MAX_SAVE_SET; ++i)
    if (s->save_sets[i].window == w && s->save_sets[i].client == c) return i;
  return -1;
}

void x_saveset_forget(x11_server *s, uint32_t window) {
  for (int i = 0; i < X11_MAX_SAVE_SET; ++i)
    if (s->save_sets[i].window == window) memset(&s->save_sets[i], 0, sizeof(s->save_sets[i]));
}

int x11_take_kill(x11_server *s) {
  for (int w = 0; w < X11_CLIENT_WORDS; ++w)
    if (s->kills[w]) {
      int c = w * 32;
      while (!x_bit(s->kills, c)) ++c;
      x_bit_clear(s->kills, c);
      return c;
    }
  return -1;
}

void x_prop_free_all(x11_server *s, x11_window *w) {
  while (w->props) {
    x11_prop *p = w->props;
    w->props = p->next;
    x_free(s, p->data);
    x_free(s, p);
  }
}

/* ---- lifecycle ------------------------------------------------------------------------ */

/* Keyboard control defaults (X.Org's): no click, bell 50 % at 400 Hz for 100 ms, LEDs off,
 * auto-repeat on for every key. */
static void kbd_defaults(x11_server *s) {
  s->kb_click = 0, s->kb_bell = 50, s->kb_pitch = 400, s->kb_duration = 100;
  s->kb_auto_repeat = 1, s->kb_leds = 0;
  memset(s->kb_repeats, 0xFF, sizeof(s->kb_repeats));
}

/* The default modifier map, X.Org's for the default keymap's keycodes (as Xvfb: dix with the
 * evdev keymap): Shift Shift_L Shift_R, Lock Caps_Lock, Control Control_L Control_R, Mod1
 * Alt_L/Meta_L Alt_R/Meta_R, Mod2 Num_Lock, Mod4 Super_L Super_R. Xvfb adds keycodes 205-207
 * (a second Meta_L, Super_L, Hyper_L), which the default keymap does not have. Clients that ask
 * which modifier holds Alt or Num_Lock (Motif, GTK, xmodmap) find it. No key is ever down. */
static void mod_defaults(x11_server *s) {
  static const uint8_t def[][2] = {{50, 1}, {62, 1}, {66, 2}, {37, 3}, {105, 3}, {64, 4}, {108, 4}, {77, 5}, {133, 7}, {134, 7}};
  memset(s->mod_of, 0, sizeof(s->mod_of));
  for (size_t i = 0; i < sizeof(def) / sizeof(def[0]); ++i) s->mod_of[def[i][0]] = def[i][1];
}

int x11_server_init_platform(x11_server *s, const x11_server_config *cfg, const x11_platform *plat) {
  memset(s, 0, sizeof(*s));
  xr_pool_use(&s->pool);  /* one server per process: its pool serves all region temporaries */
  s->cfg = *cfg;
  if (plat) s->plat = *plat;
  s->focus = 1;  /* PointerRoot */
  s->grab = -1;
  s->ss_blanking = s->ss_exposures = 1;  /* timeout 0: the screen saver is disabled */
  kbd_defaults(s);
  mod_defaults(s);
  s->kb_width = 1;
  s->ptr_accel_num = 2, s->ptr_accel_den = 1, s->ptr_threshold = 4;  /* X.Org's defaults */
  s->max_clients = cfg->max_clients && cfg->max_clients < X11_SERVER_CLIENTS ? cfg->max_clients : X11_SERVER_CLIENTS;
  s->max_resources = cfg->max_resources ? cfg->max_resources : X11_MAX_RESOURCES;
  if (s->max_resources > X11_MAX_RESOURCES_LIMIT) s->max_resources = X11_MAX_RESOURCES_LIMIT;
  if (s->max_resources < 16) s->max_resources = 16;
  uint32_t isize = 16;
  while (isize < 2u * (uint32_t)s->max_resources) isize <<= 1;
  s->rindex_mask = isize - 1;
  s->resources = (x11_resource *)x_alloc(s, sizeof(x11_resource) * (size_t)s->max_resources);
  s->rindex = (uint16_t *)x_alloc(s, sizeof(uint16_t) * isize);
  if (!s->resources || !s->rindex) return -1;
  memset(s->resources, 0, sizeof(x11_resource) * (size_t)s->max_resources);
  memset(s->rindex, 0, sizeof(uint16_t) * isize);
  s->atom_pool = (char *)x_alloc(s, X11_ATOM_POOL);
  s->atom_off = (uint16_t *)x_alloc(s, sizeof(uint16_t) * 2 * X11_MAX_DYNAMIC_ATOMS);
  if (s->atom_off) s->atom_len = s->atom_off + X11_MAX_DYNAMIC_ATOMS;
  uint16_t *fb = s->plat.framebuffer;
  if (!fb) fb = (uint16_t *)x_alloc(s, (size_t)cfg->width_px * cfg->height_px * 2);
  s->root = xw_root_create(s);
  if (!s->atom_pool || !s->atom_off || !fb || !s->root) return -1;
  s->fb.pix = fb;
  s->fb.w = s->fb.stride = cfg->width_px;
  s->fb.h = cfg->height_px;
  s->fb.depth = 16;
  xr_damage_reset(&s->fb_op);
  s->fb.dmg = &s->fb_op;
  x_add(s, X11_ID_ROOT_WINDOW, 0xFF, X11_RES_WINDOW, s->root);
  if (s->plat.diag) {  /* the diagnostics atom exists from the start (InternAtom only-if-exists finds it) */
    atom_intern(s, (const uint8_t *)X11_DIAG_ATOM, sizeof(X11_DIAG_ATOM) - 1);
  }
  x_input_init(s);
  xw_clear(s, s->root, 0, 0);  /* root background */
  damage_commit(s);
  return 0;
}

int x11_server_init(x11_server *s, const x11_server_config *cfg) { return x11_server_init_platform(s, cfg, 0); }

/* ColormapNotify (32) to the clients selecting ColormapChange on w. `is_new`: the window's
 * colormap attribute changed (ChangeWindowAttributes); else the colormap was freed. The
 * default colormap is the one installed. */
static void colormap_notify(x11_server *s, x11_window *w, uint32_t cmap, int is_new) {
  XW_FOR_SEL(w, e) {
    if (!(e->mask & X_EV_COLORMAP_CHANGE) || !s->clients[e->client].active) continue;
    int msb = s->clients[e->client].msb;
    uint8_t ev[32] = {32};
    x_wr32(ev + 4, w->id, msb);
    x_wr32(ev + 8, cmap, msb);
    ev[12] = (uint8_t)is_new;
    ev[13] = cmap == X11_ID_DEFAULT_COLORMAP;  /* Installed / Uninstalled */
    x_send_event(s, e->client, ev);
  }
}

/* FreeColormap (or its owner's close): every window using it gets colormap None and a
 * ColormapNotify with new = False. */
static void colormap_freed(x11_server *s, x11_window *w, uint32_t cmap) {
  if (w->colormap == cmap) {
    w->colormap = 0;
    colormap_notify(s, w, 0, 0);
  }
  for (x11_window *c = w->first; c; c = c->above) colormap_freed(s, c, cmap);
}

/* ---- colour allocations: the TrueColor visual has read-only cells, but AllocColor still
 * allocates them to the client (X.Org keeps a count per channel entry), and FreeColors must
 * refuse a pixel the client did not allocate (BadAccess). One record per client and colormap,
 * made at the client's first allocation: 32 red, 64 green, 32 blue counts (a count that
 * reaches 255 stays there: the entry then never frees to unallocated). */
static x11_cmap_use *cmap_use(x11_server *s, int ci, uint32_t cmap, int make) {
  for (x11_cmap_use *u = s->cmap_uses; u; u = u->next)
    if (u->client == ci && u->cmap == cmap) return u;
  if (!make) return 0;
  x11_cmap_use *u = (x11_cmap_use *)x_alloc(s, sizeof(x11_cmap_use));
  if (!u) return 0;
  memset(u, 0, sizeof(*u));
  u->cmap = cmap, u->client = (uint8_t)ci, u->next = s->cmap_uses;
  s->cmap_uses = u;
  return u;
}

/* Frees the records of a client (ci >= 0) or of a colormap (cmap != 0). */
static void cmap_uses_free(x11_server *s, int ci, uint32_t cmap) {
  for (x11_cmap_use **pu = &s->cmap_uses; *pu;) {
    x11_cmap_use *u = *pu;
    if ((ci >= 0 && u->client == ci) || (cmap && u->cmap == cmap)) {
      *pu = u->next;
      x_free(s, u);
    } else {
      pu = &u->next;
    }
  }
}

static void free_resource(x11_server *s, x11_resource *r) {
  if (r->type == X11_RES_COLORMAP) {
    cmap_uses_free(s, -1, r->id);
    if (s->root) colormap_freed(s, s->root, r->id);
  } else if (r->type == X11_RES_PIXMAP) {
    x11_pixmap *pm = (x11_pixmap *)r->obj;
    x_free(s, pm->surf.pix);
    x_free(s, pm);
  } else if (r->type == X11_RES_GC) {
    x11_gc *g = (x11_gc *)r->obj;
    xw_tile_free(s, &g->tile);
    xw_tile_free(s, &g->stipple);
    x_free(s, g);
  }
  x_res_remove(s, r);
}

void x11_server_free(x11_server *s) {
  while (s->cmap_uses) {
    x11_cmap_use *u = s->cmap_uses;
    s->cmap_uses = u->next;
    x_free(s, u);
  }
  for (int c = 0; c < X11_SERVER_CLIENTS; ++c) {
    if (s->clients[c].active || s->clients[c].id_base) x11_client_close(s, c);
    if (s->clients[c].retained) x11_client_close(s, c);  /* closed in a Retain mode just now */
  }
  x_free(s, s->kb_map);
  if (s->root) {
    while (s->root->first) xw_destroy(s, s->root->first);
    x_prop_free_all(s, s->root);
    xw_free_sels(s, s->root);
    x_free(s, s->root);
  }
  if (!s->plat.framebuffer) x_free(s, s->fb.pix);
  x_free(s, s->atom_pool);
  x_free(s, s->atom_off);
  x_free(s, s->resources);
  x_free(s, s->rindex);
  memset(s, 0, sizeof(*s));
}

int x11_client_open(x11_server *s) {
  if (s->nclients >= s->max_clients) return -1;
  for (int i = 0; i < s->max_clients; ++i) {
    x11_client *c = &s->clients[i];
    if (!c->active && !c->id_base) {
      memset(c, 0, sizeof(*c));
      c->id_base = 1;  /* slot taken (setup pending); replaced by the real base */
      ++s->nclients;
      return i;
    }
  }
  return -1;
}

size_t x11_client_setup(x11_server *s, int ci, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                        x11_setup_result *r) {
  size_t n = x11_setup_handle(&s->cfg, in, in_len, (unsigned)ci, out, out_cap, r);
  if (r->status == X11_SETUP_SUCCESS) {
    x11_client *c = &s->clients[ci];
    c->active = 1;
    c->msb = r->msb_first;
    c->seq = 0;
    c->id_base = r->resource_id_base;
    c->id_mask = r->resource_id_mask;
  }
  return n;
}

static void clear_masks(x11_server *s, x11_window *w, int c) {
  xw_select(s, w, c, 0);
  for (x11_window *k = w->first; k; k = k->above) clear_masks(s, k, c);
}

static void map_as(x11_server *s, int client, x11_window *w);

/* Save-set (ChangeSaveSet) of a closing client: each window in it that is an inferior of a
 * window the client created is reparented to the closest ancestor outside the client's
 * windows, keeping its place on the screen, and mapped if it was unmapped. */
static void restore_save_set(x11_server *s, int ci) {
  for (int i = 0; i < X11_MAX_SAVE_SET; ++i) {
    x11_saveset *e = &s->save_sets[i];
    if (!e->window || e->client != ci) continue;
    x11_resource *r = x_find(s, e->window);
    memset(e, 0, sizeof(*e));
    if (!r || r->type != X11_RES_WINDOW) continue;
    x11_window *w = (x11_window *)r->obj, *top = 0;
    for (x11_window *a = w->parent; a; a = a->parent)
      if (a->owner == ci) top = a;  /* highest ancestor the client created */
    if (top) {
      int wx, wy, px, py;
      xw_abs(w, &wx, &wy);
      xw_abs(top->parent, &px, &py);
      xw_reparent(s, w, top->parent, wx - w->bw - px, wy - w->bw - py);
    }
    if (!w->mapped) map_as(s, ci, w);
  }
}

/* Server reset (protocol, "Connection Close"), when cfg.reset_on_last_close is set: when the
 * last connection closes in Destroy mode, retained clients are destroyed (x11_client_close),
 * all but the predefined atoms are deleted, and so are the root window's properties; device
 * settings (keyboard and pointer control), device maps (keyboard, modifier and pointer), the
 * font path (always built-ins here), the input focus (PointerRoot) and the root's background
 * and cursor go back to their defaults. The access control list has nothing to reset (there
 * is none). The screen saver settings are not part of the reset. */
static void server_reset(x11_server *s) {
  s->atom_count = 0;
  s->atom_pool_used = 0;
  if (s->plat.diag) atom_intern(s, (const uint8_t *)X11_DIAG_ATOM, sizeof(X11_DIAG_ATOM) - 1);
  if (s->root) {
    x_prop_free_all(s, s->root);
    xw_root_reset(s);
  }
  kbd_defaults(s);
  s->ptr_accel_num = 2, s->ptr_accel_den = 1, s->ptr_threshold = 4;
  for (int i = 0; i < 5; ++i) s->ptr_map[i] = (uint8_t)(i + 1);  /* device maps reset too */
  x_free(s, s->kb_map);
  s->kb_map = 0, s->kb_width = 1;
  mod_defaults(s);
  s->focus = 1, s->focus_revert = 0;
}

/* The client's resources are destroyed (Destroy mode at close, or later for a retained
 * client: KillClient, AllTemporary, server reset): save-set processing, then its windows,
 * other resources and colours; the slot becomes free. */
static void client_destroy(x11_server *s, int ci) {
  restore_save_set(s, ci);
  cmap_uses_free(s, ci, 0);
  /* its windows, with inferiors (also other clients'): destroying never adds entries, so one
   * pass finds every window still owned by the client */
  for (int i = 0; i < s->max_resources; ++i) {
    x11_resource *r = &s->resources[i];
    if (r->type == X11_RES_WINDOW && r->owner == ci) xw_destroy(s, (x11_window *)r->obj);
  }
  for (int i = 0; i < s->max_resources; ++i)
    if (s->resources[i].type && s->resources[i].owner == ci) free_resource(s, &s->resources[i]);
  if (s->clients[ci].active || s->clients[ci].id_base) --s->nclients;
  memset(&s->clients[ci], 0, sizeof(s->clients[ci]));
}

/* A retained client (closed in a Retain mode) is destroyed: KillClient of one of its
 * resources, or AllTemporary for those that closed in RetainTemporary. Also when other clients
 * have freed all its resources (client_request): X.Org keeps such a client until the next
 * server reset, which the panel never does, so its slot would be lost for good; with no
 * resource left nothing of it can be reached, and its save-set is processed as at a kill. */
static void retained_destroy(x11_server *s, int ci) {
  client_destroy(s, ci);
  if (s->root) x_input_update(s);
  damage_commit(s);
}

/* The connection closed (or the server is freed). Always (protocol, "Connection Close"): event
 * selections, grabs, the server grab and selections end. In Destroy mode the resources are
 * destroyed; in RetainPermanent/RetainTemporary they stay, with the slot (its resource IDs
 * cannot be given to a new client), until KillClient or a server reset. A retained slot
 * closed again (x11_server_free) is destroyed. */
void x11_client_close(x11_server *s, int ci) {
  x11_client *c = &s->clients[ci];
  if (c->retained) {
    retained_destroy(s, ci);
    return;
  }
  int live = c->active;
  c->active = 0;  /* no events to a closing client */
  if (s->grab == ci) s->grab = -1;
  x_input_client_gone(s, ci);  /* its pointer grab ends (Ungrab crossings to the others) */
  for (int i = 0; i < X11_MAX_SELECTIONS; ++i)  /* its selections revert to None (no events) */
    if (s->selections[i].atom && s->selections[i].client == ci) memset(&s->selections[i], 0, sizeof(s->selections[i]));
  if (s->root) clear_masks(s, s->root, ci);
  x_bit_clear(s->kills, ci);
  if (live && c->close_mode) {  /* retained: no reset (protocol) */
    int retained = 0;
    for (int k = 0; k < X11_SERVER_CLIENTS; ++k) retained += s->clients[k].retained != 0;
    if (retained < X11_MAX_RETAINED) {
      c->retained = c->close_mode;
      if (s->root) x_input_update(s);
      return;
    }
    ++s->retain_refused;  /* the cap: destroyed as in Destroy mode */
  }
  int was_set_up = c->id_base > 1;  /* 1: setup was still pending */
  client_destroy(s, ci);
  /* Server reset: the last connection closed in Destroy mode (pending setups and retained
   * clients are not connections). It starts by destroying the retained clients. */
  if (was_set_up && s->cfg.reset_on_last_close) {
    int others = 0;
    for (int k = 0; k < X11_SERVER_CLIENTS; ++k) others += s->clients[k].active;
    if (!others) {
      for (int k = 0; k < X11_SERVER_CLIENTS; ++k)
        if (s->clients[k].retained) client_destroy(s, k);
      server_reset(s);
    }
  }
  if (s->root) x_input_update(s);
  damage_commit(s);  /* destroyed windows exposed what was below them */
}

/* ---- framing -------------------------------------------------------------------------- */

size_t x11_request_size(const x11_server *s, int ci, const uint8_t hdr[4]) {
  size_t units = x_rd16(hdr + 2, s->clients[ci].msb);
  if (units == 0 || units > s->cfg.max_request_units) return 0;
  return units * 4;
}

/* ---- replies and errors --------------------------------------------------------------- */

typedef struct {
  x11_server *s;
  int ci;
  x11_client *c;
  int msb;
  uint8_t *out;
  size_t cap;
  const uint8_t *q;
  size_t len;
  uint8_t major;
  x11_reply_stream *stream;
} ctx;

static size_t error(ctx *x, uint8_t code, uint32_t bad_value) {
  memset(x->out, 0, 32);
  x->out[1] = code;
  x_wr16(x->out + 2, x->c->seq, x->msb);
  x_wr32(x->out + 4, bad_value, x->msb);
  x->out[10] = x->major;
  ++x->c->errors;
  return 32;
}

static uint8_t *reply(ctx *x, uint8_t data1, size_t extra) {
  memset(x->out, 0, 32 + extra);
  x->out[0] = 1;
  x->out[1] = data1;
  x_wr16(x->out + 2, x->c->seq, x->msb);
  x_wr32(x->out + 4, (uint32_t)(extra / 4), x->msb);
  return x->out;
}

#define U8(o) (x->q[(o)])
#define U16(o) x_rd16(x->q + (o), x->msb)
#define I16(o) ((int16_t)x_rd16(x->q + (o), x->msb))
#define U32(o) x_rd32(x->q + (o), x->msb)
#define REQ_LEN(units) do { if (x->len != (size_t)(units) * 4) return error(x, X11_BAD_LENGTH, 0); } while (0)
#define REQ_MIN(bytes) do { if (x->len < (size_t)(bytes)) return error(x, X11_BAD_LENGTH, 0); } while (0)

static int new_id_ok(ctx *x, uint32_t id) { return (id & ~x->c->id_mask) == x->c->id_base && !x_find(x->s, id); }

/* The default colormap, or one a client created (CreateColormap AllocNone): every colormap
 * is the one read-only TrueColor RGB565 mapping, so colour requests treat them alike. */
static int cmap_ok(ctx *x, uint32_t id) {
  if (id == X11_ID_DEFAULT_COLORMAP) return 1;
  x11_resource *r = x_find(x->s, id);
  return r && r->type == X11_RES_COLORMAP;
}

static x11_window *find_window(ctx *x, uint32_t id) {
  x11_resource *r = x_find(x->s, id);
  return r && r->type == X11_RES_WINDOW ? (x11_window *)r->obj : 0;
}

static x11_gc *find_gc(ctx *x, uint32_t id) {
  x11_resource *r = x_find(x->s, id);
  return r && r->type == X11_RES_GC ? (x11_gc *)r->obj : 0;
}

static int popcount(uint32_t m) {
  int n = 0;
  for (; m; m &= m - 1) ++n;
  return n;
}

/* ---- atoms ---------------------------------------------------------------------------- */

/* The atom for a name, created if needed; 0 if the name is invalid or the table is full. */
/* The atom named name[0..n), made if it does not exist; 0 when the table or the pool is full. */
static uint32_t atom_intern(x11_server *s, const uint8_t *name, size_t n) {
  uint32_t atom = atom_lookup(s, name, n);
  if (atom) return atom;
  if (s->atom_count == X11_MAX_DYNAMIC_ATOMS || n + 1 > X11_ATOM_POOL - s->atom_pool_used) return 0;
  int i = s->atom_count++;
  s->atom_off[i] = (uint16_t)s->atom_pool_used;
  s->atom_len[i] = (uint16_t)n;
  if (n) memcpy(s->atom_pool + s->atom_pool_used, name, n);
  s->atom_pool[s->atom_pool_used + n] = 0;
  s->atom_pool_used += (uint32_t)n + 1;
  return (uint32_t)(X11_PREDEFINED_ATOMS + s->atom_count);
}

static size_t intern_atom(ctx *x) {
  REQ_MIN(8);
  size_t n = U16(4);
  if (x->len != 8 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));
  uint32_t atom = U8(1) ? atom_lookup(x->s, x->q + 8, n) : atom_intern(x->s, x->q + 8, n);
  if (!atom && !U8(1)) return error(x, X11_BAD_ALLOC, 0);
  x_wr32(reply(x, 0, 0) + 8, atom, x->msb);
  return 32;
}

static size_t get_atom_name(ctx *x) {
  REQ_LEN(2);
  size_t n;
  const char *name = atom_get(x->s, U32(4), &n);
  if (!name) return error(x, X11_BAD_ATOM, U32(4));
  uint8_t *r = reply(x, 0, x_pad4(n));
  x_wr16(r + 8, (uint16_t)n, x->msb);
  memcpy(r + 32, name, n);
  return 32 + x_pad4(n);
}

/* ---- properties ----------------------------------------------------------------------- */

static x11_prop *prop_find(x11_window *w, uint32_t name) {
  for (x11_prop *p = w->props; p; p = p->next)
    if (p->name == name) return p;
  return 0;
}

static void property_notify(ctx *x, x11_window *w, uint32_t atom, int deleted) {
  XW_FOR_SEL(w, e) {
    int c = e->client;
    if (!x->s->clients[c].active || !(e->mask & X_EV_PROPERTY)) continue;
    int msb = x->s->clients[c].msb;
    uint8_t ev[32] = {28};
    x_wr32(ev + 4, w->id, msb);
    x_wr32(ev + 8, atom, msb);
    x_wr32(ev + 12, x_now(x->s), msb);
    ev[16] = (uint8_t)deleted;
    x_send_event(x->s, c, ev);
  }
}

/* Converts n units of `format` between the client's byte order and storage (LSB). */
static void swap_units(uint8_t *d, const uint8_t *src, size_t nbytes, int format, int msb) {
  if (!msb || format == 8) {
    memcpy(d, src, nbytes);
    return;
  }
  for (size_t i = 0; i + (size_t)format / 8 <= nbytes; i += (size_t)format / 8) {
    if (format == 16) d[i] = src[i + 1], d[i + 1] = src[i];
    else d[i] = src[i + 3], d[i + 1] = src[i + 2], d[i + 2] = src[i + 1], d[i + 3] = src[i];
  }
}

/* _WOODLAND_DIAG on the root: generated by plat.diag at each GetProperty, never stored. */
static int is_diag(ctx *x, const x11_window *w, uint32_t name) {
  if (!x->s->plat.diag || w != x->s->root || name <= X11_PREDEFINED_ATOMS) return 0;
  const char *n = x11_atom_name(x->s, name);
  return n && !strcmp(n, X11_DIAG_ATOM);
}

static size_t change_property(ctx *x) {
  REQ_MIN(24);
  uint8_t mode = U8(1), format = U8(16);
  uint32_t wid = U32(4), name = U32(8), type = U32(12), units = U32(20);
  if (format != 8 && format != 16 && format != 32) return error(x, X11_BAD_VALUE, format);
  uint64_t nbytes = (uint64_t)units * format / 8;
  if (x->len != 24 + x_pad4((size_t)(nbytes > 0x7FFFFFFF ? 0 : nbytes)) || nbytes > 0x7FFFFFFF)
    return error(x, X11_BAD_LENGTH, 0);
  x11_window *w = find_window(x, wid);
  if (!w) return error(x, X11_BAD_WINDOW, wid);
  if (!x11_atom_name(x->s, name)) return error(x, X11_BAD_ATOM, name);
  if (!x11_atom_name(x->s, type)) return error(x, X11_BAD_ATOM, type);
  if (mode > 2) return error(x, X11_BAD_VALUE, mode);
  if (is_diag(x, w, name)) return error(x, X11_BAD_ACCESS, name);  /* read-only */
  x11_prop *p = prop_find(w, name);
  if (p && mode != 0 && (p->format != format || p->type != type)) return error(x, X11_BAD_MATCH, 0);
  size_t old = (p && mode != 0) ? p->nbytes : 0;
  if (old + nbytes > X11_MAX_PROPERTY) return error(x, X11_BAD_ALLOC, 0);
  uint8_t *data = (uint8_t *)x_alloc(x->s, old + nbytes + 1);
  if (!data) return error(x, X11_BAD_ALLOC, 0);
  if (mode == 1) {  /* Prepend */
    swap_units(data, x->q + 24, (size_t)nbytes, format, x->msb);
    if (old) memcpy(data + nbytes, p->data, old);
  } else {          /* Replace, Append */
    if (old) memcpy(data, p->data, old);
    swap_units(data + old, x->q + 24, (size_t)nbytes, format, x->msb);
  }
  if (!p) {
    p = (x11_prop *)x_alloc(x->s, sizeof(x11_prop));
    if (!p) { x_free(x->s, data); return error(x, X11_BAD_ALLOC, 0); }
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->next = w->props;
    w->props = p;
  } else {
    x_free(x->s, p->data);
  }
  p->type = type, p->format = format, p->data = data, p->nbytes = (uint32_t)(old + nbytes);
  property_notify(x, w, name, 0);
  return 0;
}

static void prop_delete(ctx *x, x11_window *w, uint32_t name) {
  for (x11_prop **pp = &w->props; *pp; pp = &(*pp)->next)
    if ((*pp)->name == name) {
      x11_prop *p = *pp;
      *pp = p->next;
      x_free(x->s, p->data);
      x_free(x->s, p);
      property_notify(x, w, name, 1);
      return;
    }
}

static size_t delete_property(ctx *x) {
  REQ_LEN(3);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  if (!x11_atom_name(x->s, U32(8))) return error(x, X11_BAD_ATOM, U32(8));
  if (is_diag(x, w, U32(8))) return error(x, X11_BAD_ACCESS, U32(8));  /* read-only */
  prop_delete(x, w, U32(8));
  return 0;
}

/* GetProperty of _WOODLAND_DIAG: type STRING, format 8, with the usual offset/length slicing.
 * The delete flag does not apply: the text is regenerated at every read and its counters
 * are cumulative, so a read never resets anything. */
static size_t get_diag(ctx *x, uint32_t type, uint32_t off, uint32_t length) {
  const uint32_t STRING = 31;
  if (x->cap < 32 + 2 * (size_t)X11_DIAG_MAX) return error(x, X11_BAD_ALLOC, 0);
  char *text = (char *)x->out + 32 + X11_DIAG_MAX;  /* scratch, past the reply's data */
  size_t n = x->s->plat.diag(x->s->plat.ctx, text, X11_DIAG_MAX);
  if (n > X11_DIAG_MAX) n = X11_DIAG_MAX;
  if (type != 0 && type != STRING) {
    uint8_t *r = reply(x, 8, 0);
    x_wr32(r + 8, STRING, x->msb);
    x_wr32(r + 12, (uint32_t)n, x->msb);
    return 32;
  }
  uint64_t start = (uint64_t)off * 4;
  if (start > n) return error(x, X11_BAD_VALUE, off);
  uint64_t want = (uint64_t)length * 4, avail = n - start;
  uint32_t k = (uint32_t)(want < avail ? want : avail);
  uint8_t *r = reply(x, 8, x_pad4(k));
  x_wr32(r + 8, STRING, x->msb);
  x_wr32(r + 12, (uint32_t)(n - (start + k)), x->msb);
  x_wr32(r + 16, k, x->msb);
  memcpy(r + 32, text + start, k);
  return 32 + x_pad4(k);
}

static size_t get_property(ctx *x) {
  REQ_LEN(6);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));
  uint32_t wid = U32(4), name = U32(8), type = U32(12), off = U32(16), length = U32(20);
  x11_window *w = find_window(x, wid);
  if (!w) return error(x, X11_BAD_WINDOW, wid);
  if (!x11_atom_name(x->s, name)) return error(x, X11_BAD_ATOM, name);
  if (type != 0 && !x11_atom_name(x->s, type)) return error(x, X11_BAD_ATOM, type);
  if (is_diag(x, w, name)) return get_diag(x, type, off, length);
  x11_prop *p = prop_find(w, name);
  if (!p) { reply(x, 0, 0); return 32; }  /* type None, format 0 */
  if (type != 0 && type != p->type) {      /* wrong type: report it, no data */
    uint8_t *r = reply(x, p->format, 0);
    x_wr32(r + 8, p->type, x->msb);
    x_wr32(r + 12, p->nbytes, x->msb);
    return 32;
  }
  uint64_t start = (uint64_t)off * 4;
  if (start > p->nbytes) return error(x, X11_BAD_VALUE, off);
  uint64_t want = (uint64_t)length * 4, avail = p->nbytes - start;
  uint32_t n = (uint32_t)(want < avail ? want : avail);
  uint32_t after = (uint32_t)(p->nbytes - (start + n));
  uint8_t *r = reply(x, p->format, x_pad4(n));
  x_wr32(r + 8, p->type, x->msb);
  x_wr32(r + 12, after, x->msb);
  x_wr32(r + 16, n / (p->format / 8), x->msb);
  /* storage is LSB: swapping again converts to the client's order */
  swap_units(r + 32, p->data + start, n, p->format, x->msb);
  if (U8(1) && after == 0) prop_delete(x, w, name);
  return 32 + x_pad4(n);
}

/* RotateProperties: the value of property i goes to property (i + delta) mod N. Everything is
 * checked before anything moves (BadAtom, then BadMatch for a repeated atom or a property the
 * window does not have); then one PropertyNotify (NewValue) per listed atom, in list order,
 * unless delta mod N is 0. The values move along cycles of the permutation, so no buffer is
 * needed whatever N is. */
static void prop_swap_value(x11_prop *a, x11_prop *b) {
  x11_prop t = *a;
  a->type = b->type, a->format = b->format, a->nbytes = b->nbytes, a->data = b->data;
  b->type = t.type, b->format = t.format, b->nbytes = t.nbytes, b->data = t.data;
}
static size_t rotate_properties(ctx *x) {
  REQ_MIN(12);
  size_t n = U16(8);
  if (x->len != 12 + 4 * n) return error(x, X11_BAD_LENGTH, 0);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  if (!n) return 0;
  for (size_t i = 0; i < n; ++i)
    if (!x11_atom_name(x->s, U32(12 + 4 * i))) return error(x, X11_BAD_ATOM, U32(12 + 4 * i));
  for (size_t i = 0; i < n; ++i) {
    uint32_t a = U32(12 + 4 * i);
    for (size_t j = 0; j < i; ++j)
      if (U32(12 + 4 * j) == a) return error(x, X11_BAD_MATCH, a);
    if (!prop_find(w, a)) return error(x, X11_BAD_MATCH, a);
  }
  long d = (long)I16(10) % (long)n;
  if (d < 0) d += (long)n;
  if (!d) return 0;
  /* new[k] = old[(k - d) mod n]: walk each cycle from its start, swapping the start's value
   * along (gcd(n, d) cycles, which start at 0 .. gcd - 1) */
  size_t g = n, r = (size_t)d;
  while (r) { size_t t = g % r; g = r; r = t; }
  for (size_t start = 0; start < g; ++start) {
    x11_prop *first = prop_find(w, U32(12 + 4 * start));
    for (size_t k = (start + (size_t)d) % n; k != start; k = (k + (size_t)d) % n)
      prop_swap_value(first, prop_find(w, U32(12 + 4 * k)));
  }
  for (size_t i = 0; i < n; ++i) property_notify(x, w, U32(12 + 4 * i), 0);
  return 0;
}

static size_t list_properties(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  size_t n = 0;
  for (x11_prop *p = w->props; p; p = p->next) ++n;
  if (32 + 4 * n > x->cap) return error(x, X11_BAD_ALLOC, 0);
  uint8_t *r = reply(x, 0, 4 * n);
  x_wr16(r + 8, (uint16_t)n, x->msb);
  size_t i = 0;
  for (x11_prop *p = w->props; p; p = p->next) x_wr32(r + 32 + 4 * i++, p->name, x->msb);
  return 32 + 4 * n;
}

/* ---- windows -------------------------------------------------------------------------- */

/* A background or border pixmap: same depth as the window (16), copied (the client may free it). */
static size_t window_tile(ctx *x, const x11_window *w, uint32_t pid, xr_surface **out) {
  x11_resource *r = x_find(x->s, pid);
  if (!r || r->type != X11_RES_PIXMAP) return error(x, X11_BAD_PIXMAP, pid);
  if (w->input_only || ((x11_pixmap *)r->obj)->surf.depth != 16) return error(x, X11_BAD_MATCH, pid);
  *out = xw_tile_copy(x->s, &((x11_pixmap *)r->obj)->surf);
  return *out ? 0 : error(x, X11_BAD_ALLOC, 0);
}

static size_t set_window_values(ctx *x, x11_window *w, uint32_t mask, const uint8_t *v, int creating) {
  /* bit order: bg-pixmap bg-pixel border-pixmap border-pixel bit-gravity win-gravity
     backing-store backing-planes backing-pixel override-redirect save-under event-mask
     do-not-propagate-mask colormap cursor */
  if (mask & ~0x7FFFu) return error(x, X11_BAD_VALUE, mask);
  for (int bit = 0; bit < 15; ++bit) {
    if (!(mask & (1u << bit))) continue;
    uint32_t val = x_rd32(v, x->msb);
    v += 4;
    switch (bit) {
      case 0:  /* background-pixmap: None, ParentRelative or a pixmap (copied) */
        if (val > 1) {
          xr_surface *t;
          size_t e = window_tile(x, w, val, &t);
          if (e) return e;
          xw_tile_free(x->s, &w->bg_tile);
          w->bg_tile = t, w->bg_mode = 3;
        } else {
          if (val == 1 && w->parent && w->parent->input_only) return error(x, X11_BAD_MATCH, val);
          xw_tile_free(x->s, &w->bg_tile);
          w->bg_mode = val ? 2 : 0;
        }
        break;
      case 1: xw_tile_free(x->s, &w->bg_tile); w->bg_mode = 1, w->bg_pixel = val & 0xFFFFu; break;
      case 2:  /* border-pixmap: CopyFromParent or a pixmap (copied) */
        if (val == 0) {
          if (w->parent && w->parent->border_tile && !w->input_only) {
            xr_surface *t = xw_tile_copy(x->s, w->parent->border_tile);
            if (!t) return error(x, X11_BAD_ALLOC, 0);
            xw_tile_free(x->s, &w->border_tile);
            w->border_tile = t;
          } else if (w->parent) {
            xw_tile_free(x->s, &w->border_tile);
            w->border_pixel = w->parent->border_pixel;
          }
        } else {
          xr_surface *t;
          size_t e = window_tile(x, w, val, &t);
          if (e) return e;
          xw_tile_free(x->s, &w->border_tile);
          w->border_tile = t;
        }
        break;
      case 3: xw_tile_free(x->s, &w->border_tile); w->border_pixel = val & 0xFFFFu; break;
      case 4: if (val > 10) return error(x, X11_BAD_VALUE, val); w->bit_gravity = (uint8_t)val; break;
      case 5: if (val > 10) return error(x, X11_BAD_VALUE, val); w->win_gravity = (uint8_t)val; break;
      case 6: if (val > 2) return error(x, X11_BAD_VALUE, val); w->backing_store = (uint8_t)val; break;
      case 7: w->backing_planes = val; break;
      case 8: w->backing_pixel = val; break;
      case 9: if (val > 1) return error(x, X11_BAD_VALUE, val); w->override_redirect = (uint8_t)val; break;
      case 10: if (val > 1) return error(x, X11_BAD_VALUE, val); w->save_under = (uint8_t)val; break;
      case 11: {
        if (val & 0xFE000000u) return error(x, X11_BAD_VALUE, val);
        /* at most one client may select each of these on a window */
        static const uint32_t EXCLUSIVE[3] = {X_EV_SUBSTRUCTURE_REDIRECT, X_EV_RESIZE_REDIRECT, X_EV_BUTTON_PRESS};
        for (int i = 0; i < 3; ++i)
          if ((val & EXCLUSIVE[i]) && xw_redirect_client(x->s, w, EXCLUSIVE[i], x->ci) >= 0)
            return error(x, X11_BAD_ACCESS, val);
        if (xw_select(x->s, w, x->ci, val) != 0) return error(x, X11_BAD_ALLOC, 0);
        break;
      }
      case 12: w->dont_propagate = (uint16_t)val; break;
      case 13:
        if (val != 0 && !cmap_ok(x, val)) return error(x, X11_BAD_COLORMAP, val);
        val = val ? val : (w->parent ? w->parent->colormap : X11_ID_DEFAULT_COLORMAP);  /* CopyFromParent */
        if (!val) val = X11_ID_DEFAULT_COLORMAP;
        if (val != w->colormap) {
          w->colormap = val;
          if (!creating) colormap_notify(x->s, w, val, 1);
        }
        break;
      case 14: {
        x11_resource *r = x_find(x->s, val);
        if (val && (!r || r->type != X11_RES_CURSOR)) return error(x, X11_BAD_CURSOR, val);
        w->cursor = val;
        break;
      }
    }
  }
  (void)creating;
  return 0;
}

static size_t create_window(ctx *x) {
  REQ_MIN(32);
  uint32_t wid = U32(4), pid = U32(8), visual = U32(24), mask = U32(28);
  uint8_t depth = U8(1);
  uint16_t cls = U16(22);
  /* the value list has one word per set bit, defined or not (as X.Org): an undefined bit is
   * BadValue, not BadLength */
  if (x->len != 32 + 4 * (size_t)popcount(mask)) return error(x, X11_BAD_LENGTH, 0);
  if (mask & ~0x7FFFu) return error(x, X11_BAD_VALUE, mask);
  if (!new_id_ok(x, wid)) return error(x, X11_BAD_IDCHOICE, wid);
  x11_window *parent = find_window(x, pid);
  if (!parent) return error(x, X11_BAD_WINDOW, pid);
  if (U16(16) == 0 || U16(18) == 0) return error(x, X11_BAD_VALUE, 0);
  if (cls > 2) return error(x, X11_BAD_VALUE, cls);
  int input_only = cls == 2 || (cls == 0 && parent->input_only);
  if (input_only ? (depth != 0 || U16(20) != 0) : (depth != 0 && depth != 16))
    return error(x, X11_BAD_MATCH, 0);
  if (!input_only && parent->input_only) return error(x, X11_BAD_MATCH, 0);
  if (visual != 0 && visual != X11_ID_VISUAL_TRUECOLOR16) return error(x, X11_BAD_MATCH, visual);
  x11_window *w = (x11_window *)x_alloc(x->s, sizeof(x11_window));
  if (!w) return error(x, X11_BAD_ALLOC, 0);
  memset(w, 0, sizeof(*w));
  w->id = wid;
  w->owner = (uint8_t)x->ci;
  w->x = I16(12), w->y = I16(14), w->w = U16(16), w->h = U16(18), w->bw = U16(20);
  w->input_only = (uint8_t)input_only;
  w->colormap = X11_ID_DEFAULT_COLORMAP;
  w->backing_planes = 0xFFFFFFFFu;  /* defaults: all planes, pixel 0 */
  w->win_gravity = 1;  /* NorthWest */
  w->parent = parent;  /* for CopyFromParent; linked into the tree below */
  size_t e = set_window_values(x, w, mask, x->q + 32, 1);
  if (!e && !x_add(x->s, wid, x->ci, X11_RES_WINDOW, w)) e = error(x, X11_BAD_ALLOC, 0);
  if (e) {
    xw_tile_free(x->s, &w->bg_tile);
    xw_tile_free(x->s, &w->border_tile);
    xw_free_sels(x->s, w);
    x_free(x->s, w);
    return e;
  }
  xw_link_top(parent, w);
  /* CreateNotify to the parent's SubstructureNotify selectors */
  XW_FOR_SEL(parent, se) {
    int c = se->client;
    if (!x->s->clients[c].active || !(se->mask & X_EV_SUBSTRUCTURE)) continue;
    int msb = x->s->clients[c].msb;
    uint8_t ev[32] = {16};
    x_wr32(ev + 4, parent->id, msb);
    x_wr32(ev + 8, wid, msb);
    x_wr16(ev + 12, (uint16_t)w->x, msb), x_wr16(ev + 14, (uint16_t)w->y, msb);
    x_wr16(ev + 16, w->w, msb), x_wr16(ev + 18, w->h, msb), x_wr16(ev + 20, w->bw, msb);
    ev[22] = w->override_redirect;
    x_send_event(x->s, c, ev);
  }
  return 0;
}

static size_t change_window_attributes(ctx *x) {
  REQ_MIN(12);
  uint32_t mask = U32(8);
  if (x->len != 12 + 4 * (size_t)popcount(mask)) return error(x, X11_BAD_LENGTH, 0);
  if (mask & ~0x7FFFu) return error(x, X11_BAD_VALUE, mask);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  uint8_t old_mode = w->bg_mode;
  uint32_t old_bg = w->bg_pixel;
  size_t e = set_window_values(x, w, mask, x->q + 12, 0);
  (void)old_mode, (void)old_bg;  /* a new background shows at the next exposure or ClearArea */
  if (!e && (mask & 0x0Cu)) xw_repaint_border(x->s, w);  /* border-pixmap, border-pixel: at once */
  return e;
}

static size_t get_window_attributes(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  uint8_t *r = reply(x, w->backing_store, 12);
  uint32_t all = xw_all_masks(w);
  x_wr32(r + 8, w->input_only ? 0 : X11_ID_VISUAL_TRUECOLOR16, x->msb);
  x_wr16(r + 12, w->input_only ? 2 : 1, x->msb);
  r[14] = w->bit_gravity, r[15] = w->win_gravity;
  x_wr32(r + 16, w->backing_planes, x->msb);
  x_wr32(r + 20, w->backing_pixel, x->msb);
  r[24] = w->save_under;
  r[25] = 1;  /* the one colormap is always installed */
  r[26] = w->mapped ? (xw_viewable(w) ? 2 : 1) : 0;
  r[27] = w->override_redirect;
  x_wr32(r + 28, w->input_only ? 0 : w->colormap, x->msb);
  x_wr32(r + 32, all, x->msb);
  x_wr32(r + 36, xw_mask(w, x->ci), x->msb);
  x_wr16(r + 40, w->dont_propagate, x->msb);
  return 44;
}

static size_t destroy_window(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  xw_destroy(x->s, w);  /* the root: no effect */
  return 0;
}

static size_t destroy_subwindows(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  while (w->last) xw_destroy(x->s, w->last);
  return 0;
}

/* MapWindow semantics for `client`: if another client selected SubstructureRedirect on the
 * parent and the window is not override-redirect, that client gets a MapRequest and the
 * window stays unmapped. */
static void map_as(x11_server *s, int client, x11_window *w) {
  if (w->mapped || !w->parent) return;
  int wm = w->override_redirect ? -1 : xw_redirect_client(s, w->parent, X_EV_SUBSTRUCTURE_REDIRECT, client);
  if (wm < 0) {
    xw_map(s, w);
    return;
  }
  int msb = s->clients[wm].msb;
  uint8_t ev[32] = {20};  /* MapRequest */
  x_wr32(ev + 4, w->parent->id, msb);
  x_wr32(ev + 8, w->id, msb);
  x_send_event(s, wm, ev);
}

static size_t map_window(ctx *x, int sub) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  if (sub) {  /* unmapped children, top to bottom */
    for (x11_window *c = w->last, *next; c; c = next) {
      next = c->below;
      map_as(x->s, x->ci, c);
    }
  } else {
    map_as(x->s, x->ci, w);
  }
  return 0;
}

static size_t change_save_set(ctx *x) {
  REQ_LEN(2);
  uint8_t mode = U8(1);
  x11_window *w = find_window(x, U32(4));
  if (mode > 1) return error(x, X11_BAD_VALUE, mode);
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  if (w->owner == x->ci || !w->parent) return error(x, X11_BAD_MATCH, U32(4));  /* not one's own window */
  int i = saveset_has(x->s, x->ci, w->id);
  if (mode == 1) {  /* Delete */
    if (i >= 0) memset(&x->s->save_sets[i], 0, sizeof(x->s->save_sets[i]));
    return 0;
  }
  if (i >= 0) return 0;  /* already in it */
  for (i = 0; i < X11_MAX_SAVE_SET && x->s->save_sets[i].window; ++i) {}
  if (i == X11_MAX_SAVE_SET) return error(x, X11_BAD_ALLOC, 0);
  x->s->save_sets[i].window = w->id;
  x->s->save_sets[i].client = (uint8_t)x->ci;
  return 0;
}

static size_t reparent_window(ctx *x) {
  REQ_LEN(4);
  x11_window *w = find_window(x, U32(4)), *np = find_window(x, U32(8));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  if (!np) return error(x, X11_BAD_WINDOW, U32(8));
  if (!w->parent) return error(x, X11_BAD_MATCH, U32(4));  /* the root */
  for (x11_window *a = np; a; a = a->parent)
    if (a == w) return error(x, X11_BAD_MATCH, U32(8));  /* the window itself or an inferior */
  if (np->input_only && !w->input_only) return error(x, X11_BAD_MATCH, U32(8));
  int was_mapped = w->mapped;
  xw_reparent(x->s, w, np, I16(12), I16(14));
  if (was_mapped) map_as(x->s, x->ci, w);
  return 0;
}

/* UnmapSubwindows: UnmapWindow on each mapped child, bottom to top. */
static size_t unmap_subwindows(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  for (x11_window *c = w->first, *next; c; c = next) {
    next = c->above;
    xw_unmap(x->s, c, 1);
  }
  return 0;
}
static size_t unmap_window(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  xw_unmap(x->s, w, 1);
  return 0;
}

/* Outer rectangles of two siblings intersect (w with the geometry it is about to get). */
static int outer_overlap(const x11_window *a, int ax, int ay, int aw, int ah, int abw, const x11_window *b) {
  int a2x = ax + aw + 2 * abw, a2y = ay + ah + 2 * abw;
  int b2x = b->x + b->w + 2 * b->bw, b2y = b->y + b->h + 2 * b->bw;
  (void)a;
  return ax < b2x && b->x < a2x && ay < b2y && b->y < a2y;
}

static int stacked_above(const x11_window *a, const x11_window *b) {  /* a higher than b */
  for (const x11_window *c = b->above; c; c = c->above)
    if (c == a) return 1;
  return 0;
}

static size_t configure_window(ctx *x) {
  REQ_MIN(12);
  uint16_t mask = U16(8);
  if (x->len != 12 + 4 * (size_t)popcount(mask)) return error(x, X11_BAD_LENGTH, 0);
  if (mask & ~0x7Fu) return error(x, X11_BAD_VALUE, mask);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  int nx = w->x, ny = w->y, nw = w->w, nh = w->h, nb = w->bw, mode = -1;
  uint32_t sib_id = 0;
  const uint8_t *v = x->q + 12;
  for (int bit = 0; bit < 7; ++bit) {
    if (!(mask & (1u << bit))) continue;
    uint32_t val = x_rd32(v, x->msb);
    v += 4;
    switch (bit) {
      case 0: nx = (int16_t)val; break;
      case 1: ny = (int16_t)val; break;
      case 2: if (!(val & 0xFFFF)) return error(x, X11_BAD_VALUE, val); nw = (uint16_t)val; break;
      case 3: if (!(val & 0xFFFF)) return error(x, X11_BAD_VALUE, val); nh = (uint16_t)val; break;
      case 4: nb = (uint16_t)val; break;
      case 5: sib_id = val; break;
      case 6: if (val > 4) return error(x, X11_BAD_VALUE, val); mode = (int)val; break;
    }
  }
  x11_window *sib = 0;
  if (mask & 0x20) {  /* a sibling needs a stack-mode and must be a sibling */
    sib = find_window(x, sib_id);
    if (!sib) return error(x, X11_BAD_WINDOW, sib_id);
    if (mode < 0 || sib == w || sib->parent != w->parent) return error(x, X11_BAD_MATCH, sib_id);
  }
  if (!w->parent) return 0;  /* the root: nothing to configure */
  int wm = w->override_redirect ? -1 : xw_redirect_client(x->s, w->parent, X_EV_SUBSTRUCTURE_REDIRECT, x->ci);
  if (wm >= 0) {  /* a window manager decides: ConfigureRequest, nothing changes */
    int msb = x->s->clients[wm].msb;
    uint8_t ev[32] = {23, (uint8_t)(mode < 0 ? 0 : mode)};
    x_wr32(ev + 4, w->parent->id, msb);
    x_wr32(ev + 8, w->id, msb);
    x_wr32(ev + 12, sib ? sib->id : 0, msb);
    x_wr16(ev + 16, (uint16_t)nx, msb), x_wr16(ev + 18, (uint16_t)ny, msb);
    x_wr16(ev + 20, (uint16_t)nw, msb), x_wr16(ev + 22, (uint16_t)nh, msb), x_wr16(ev + 24, (uint16_t)nb, msb);
    x_wr16(ev + 26, mask, msb);
    x_send_event(x->s, wm, ev);
    return 0;
  }
  /* stack-mode with the window's final geometry: TopIf, BottomIf, Opposite decide by
   * occlusion (both mapped, one higher, outer edges intersecting) */
  int stack = XW_STACK_NONE;
  x11_window *rel = sib;
  if (mode == 0 || mode == 1) {
    stack = mode;
  } else if (mode > 1) {
    int sib_over_w = 0, w_over_sib = 0;
    for (x11_window *c = w->parent->first; c; c = c->above) {
      if (c == w || (sib && c != sib) || !c->mapped || !w->mapped) continue;
      if (!outer_overlap(w, nx, ny, nw, nh, nb, c)) continue;
      if (stacked_above(c, w)) sib_over_w = 1;
      else w_over_sib = 1;
    }
    if ((mode == 2 || mode == 4) && sib_over_w) stack = XW_STACK_ABOVE;
    else if ((mode == 3 || mode == 4) && w_over_sib) stack = XW_STACK_BELOW;
    rel = 0;  /* these move to the top or bottom of the stack */
  }
  xw_configure(x->s, w, nx, ny, nw, nh, nb, stack, rel);
  return 0;
}

/* CirculateWindow: RaiseLowest raises the lowest mapped child that a mapped child above it
 * overlaps; LowerHighest lowers the highest mapped child that overlaps a mapped child below
 * it (outer edges, as X.Org). With SubstructureRedirect selected by another client that
 * client gets a CirculateRequest and nothing changes. */
static int overlaps_any(const x11_window *c, int up) {
  for (const x11_window *o = up ? c->above : c->below; o; o = up ? o->above : o->below)
    if (o->mapped && outer_overlap(c, c->x, c->y, c->w, c->h, c->bw, o)) return 1;
  return 0;
}
static size_t circulate_window(ctx *x) {
  REQ_LEN(2);
  uint8_t dir = U8(1);
  if (dir > 1) return error(x, X11_BAD_VALUE, dir);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  x11_window *c = dir == 0 ? w->first : w->last;
  for (; c; c = dir == 0 ? c->above : c->below)
    if (c->mapped && overlaps_any(c, dir == 0)) break;
  if (!c) return 0;
  int wm = xw_redirect_client(x->s, w, X_EV_SUBSTRUCTURE_REDIRECT, x->ci);
  if (wm >= 0) {
    int msb = x->s->clients[wm].msb;
    uint8_t ev[32] = {27};  /* CirculateRequest */
    x_wr32(ev + 4, w->id, msb);
    x_wr32(ev + 8, c->id, msb);
    ev[16] = dir;  /* RaiseLowest: place Top; LowerHighest: Bottom */
    x_send_event(x->s, wm, ev);
    return 0;
  }
  xw_circulate(x->s, c, dir);
  return 0;
}

static size_t get_geometry(ctx *x) {
  REQ_LEN(2);
  x11_resource *res = x_find(x->s, U32(4));
  if (!res || (res->type != X11_RES_WINDOW && res->type != X11_RES_PIXMAP)) return error(x, X11_BAD_DRAWABLE, U32(4));
  uint8_t *r;
  if (res->type == X11_RES_WINDOW) {
    x11_window *w = (x11_window *)res->obj;
    r = reply(x, w->input_only ? 0 : 16, 0);
    x_wr16(r + 12, (uint16_t)w->x, x->msb), x_wr16(r + 14, (uint16_t)w->y, x->msb);
    x_wr16(r + 16, w->w, x->msb), x_wr16(r + 18, w->h, x->msb), x_wr16(r + 20, w->bw, x->msb);
  } else {
    x11_pixmap *p = (x11_pixmap *)res->obj;
    r = reply(x, (uint8_t)p->surf.depth, 0);
    x_wr16(r + 16, (uint16_t)p->surf.w, x->msb), x_wr16(r + 18, (uint16_t)p->surf.h, x->msb);
  }
  x_wr32(r + 8, X11_ID_ROOT_WINDOW, x->msb);
  return 32;
}

static size_t query_tree(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  size_t n = 0;
  for (x11_window *c = w->first; c; c = c->above) ++n;
  if (32 + 4 * n > x->cap) return error(x, X11_BAD_ALLOC, 0);
  uint8_t *r = reply(x, 0, 4 * n);
  x_wr32(r + 8, X11_ID_ROOT_WINDOW, x->msb);
  x_wr32(r + 12, w->parent ? w->parent->id : 0, x->msb);
  x_wr16(r + 16, (uint16_t)n, x->msb);
  size_t i = 0;
  for (x11_window *c = w->first; c; c = c->above) x_wr32(r + 32 + 4 * i++, c->id, x->msb);  /* bottom to top */
  return 32 + 4 * n;
}

static size_t translate_coordinates(ctx *x) {
  REQ_LEN(4);
  x11_window *a = find_window(x, U32(4)), *b = find_window(x, U32(8));
  if (!a) return error(x, X11_BAD_WINDOW, U32(4));
  if (!b) return error(x, X11_BAD_WINDOW, U32(8));
  int ax, ay, bx, by;
  xw_abs(a, &ax, &ay);
  xw_abs(b, &bx, &by);
  int px = ax + I16(12), py = ay + I16(14);
  uint32_t child = 0;  /* the mapped child of b containing the point, topmost */
  for (x11_window *c = b->last; c; c = c->below) {
    if (!c->mapped) continue;
    int cx, cy;
    xw_abs(c, &cx, &cy);
    if (px >= cx - c->bw && px < cx + c->w + c->bw && py >= cy - c->bw && py < cy + c->h + c->bw) {
      child = c->id;
      break;
    }
  }
  uint8_t *r = reply(x, 1, 0);
  x_wr32(r + 8, child, x->msb);
  x_wr16(r + 12, (uint16_t)(px - bx), x->msb);
  x_wr16(r + 14, (uint16_t)(py - by), x->msb);
  return 32;
}

static size_t query_pointer(ctx *x) {
  REQ_LEN(2);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  x11_server *s = x->s;
  x11_window *child = x_input_child(s, w);
  int ax, ay;
  xw_abs(w, &ax, &ay);
  x_input_reset_hint(s, x->ci);
  uint8_t *r = reply(x, 1, 0);  /* same-screen */
  x_wr32(r + 8, X11_ID_ROOT_WINDOW, x->msb);
  x_wr32(r + 12, child ? child->id : 0, x->msb);
  x_wr16(r + 16, (uint16_t)s->ptr_x, x->msb), x_wr16(r + 18, (uint16_t)s->ptr_y, x->msb);
  x_wr16(r + 20, (uint16_t)(s->ptr_x - ax), x->msb), x_wr16(r + 22, (uint16_t)(s->ptr_y - ay), x->msb);
  x_wr16(r + 24, s->ptr_buttons, x->msb);
  return 32;
}

static size_t set_input_focus(ctx *x) {
  REQ_LEN(3);
  uint8_t revert = U8(1);
  uint32_t f = U32(4);
  if (revert > 2) return error(x, X11_BAD_VALUE, revert);
  if (f > 1) {
    x11_window *w = find_window(x, f);
    if (!w) return error(x, X11_BAD_WINDOW, f);
    if (!xw_viewable(w)) return error(x, X11_BAD_MATCH, f);
  }
  xw_set_focus(x->s, f, revert);
  return 0;
}

static size_t get_input_focus(ctx *x) {
  REQ_LEN(1);
  uint8_t *r = reply(x, x->s->focus_revert, 0);
  x_wr32(r + 8, x->s->focus, x->msb);
  return 32;
}

/* ---- pixmaps -------------------------------------------------------------------------- */

static size_t create_pixmap(ctx *x) {
  REQ_LEN(4);
  uint8_t depth = U8(1);
  uint32_t pid = U32(4), did = U32(8);
  uint16_t w = U16(12), h = U16(14);
  if (!new_id_ok(x, pid)) return error(x, X11_BAD_IDCHOICE, pid);
  x11_resource *d = x_find(x->s, did);
  if (!d || (d->type != X11_RES_WINDOW && d->type != X11_RES_PIXMAP)) return error(x, X11_BAD_DRAWABLE, did);
  if (!w || !h) return error(x, X11_BAD_VALUE, 0);
  if (depth != 1 && depth != 16) return error(x, X11_BAD_VALUE, depth);
  x11_pixmap *pm = (x11_pixmap *)x_alloc(x->s, sizeof(x11_pixmap));
  size_t bpp = depth == 16 ? 2 : 1;
  if ((size_t)w > SIZE_MAX / (size_t)h / bpp) { x_free(x->s, pm); return error(x, X11_BAD_ALLOC, 0); }
  size_t bytes = (size_t)w * h * bpp;
  void *pix = pm ? x_alloc(x->s, bytes) : 0;
  if (!pix) { x_free(x->s, pm); return error(x, X11_BAD_ALLOC, 0); }
  memset(pix, 0, bytes);  /* contents are undefined by the protocol; zero is deterministic */
  pm->surf.pix = pix, pm->surf.w = pm->surf.stride = w, pm->surf.h = h, pm->surf.depth = depth;
  pm->surf.dmg = 0;  /* only the screen is tracked */
  if (!x_add(x->s, pid, x->ci, X11_RES_PIXMAP, pm)) {
    x_free(x->s, pix);
    x_free(x->s, pm);
    return error(x, X11_BAD_ALLOC, 0);
  }
  return 0;
}

static size_t free_pixmap(ctx *x) {
  REQ_LEN(2);
  x11_resource *r = x_find(x->s, U32(4));
  if (!r || r->type != X11_RES_PIXMAP) return error(x, X11_BAD_PIXMAP, U32(4));
  free_resource(x->s, r);
  return 0;
}

/* ---- graphics contexts ---------------------------------------------------------------- */

static void gc_defaults(x11_gc *g, uint8_t depth) {
  memset(g, 0, sizeof(*g));
  g->depth = depth;
  g->function = 3;            /* GXcopy */
  g->plane_mask = 0xFFFFFFFFu;
  g->fg = 0, g->bg = 1;
  g->cap_style = 1;           /* CapButt */
  g->arc_mode = 1;            /* PieSlice */
  g->graphics_exposures = 1;
  g->ndash = 1, g->dash[0] = 4;
  g->nclip = -1;
  g->font = 0;                /* the server's default font (gc_font) */
}

/* A clip-mask bitmap as rectangles (relative to the clip origin): runs of 1-bits per row,
 * identical runs on consecutive rows merged. Its contents are taken when it is set. Returns
 * the count, or -1 if more than max rectangles are needed. */
static int mask_rects(const xr_surface *m, xr_rect *out, int max) {
  int n = 0, open0 = 0;  /* rectangles from open0 on can still grow downwards */
  for (int y = 0; y < m->h; ++y) {
    const uint8_t *row = (const uint8_t *)m->pix + (size_t)y * m->stride;
    int open_end = n;
    for (int x = 0; x < m->w;) {
      if (!row[x]) { ++x; continue; }
      int x1 = x;
      while (x < m->w && row[x]) ++x;
      int i = open0;  /* the same run on the previous row: extend it */
      while (i < open_end && !(out[i].x1 == x1 && out[i].x2 == x && out[i].y2 == y)) ++i;
      if (i < open_end) {
        out[i].y2 = (int16_t)(y + 1);
        continue;
      }
      if (n == max) return -1;
      out[n].x1 = (int16_t)x1, out[n].x2 = (int16_t)x, out[n].y1 = (int16_t)y, out[n].y2 = (int16_t)(y + 1);
      ++n;
    }
    /* runs not continued on this row are final; keep the earlier list order stable */
    while (open0 < open_end && out[open0].y2 < y + 1) ++open0;
  }
  return n;
}

static size_t gc_values(ctx *x, x11_gc *g, uint32_t mask, const uint8_t *v) {
  if (mask & ~0x7FFFFFu) return error(x, X11_BAD_VALUE, mask);
  for (int bit = 0; bit < 23; ++bit) {
    if (!(mask & (1u << bit))) continue;
    uint32_t val = x_rd32(v, x->msb);
    v += 4;
    switch (bit) {
      case 0: if (val > 15) return error(x, X11_BAD_VALUE, val); g->function = (uint8_t)val; break;
      case 1: g->plane_mask = val; break;
      case 2: g->fg = val; break;
      case 3: g->bg = val; break;
      case 4: g->line_width = (uint16_t)val; break;
      case 5: if (val > 2) return error(x, X11_BAD_VALUE, val); g->line_style = (uint8_t)val; break;
      case 6: if (val > 3) return error(x, X11_BAD_VALUE, val); g->cap_style = (uint8_t)val; break;
      case 7: if (val > 2) return error(x, X11_BAD_VALUE, val); g->join_style = (uint8_t)val; break;
      case 8: if (val > 3) return error(x, X11_BAD_VALUE, val); g->fill_style = (uint8_t)val; break;
      case 9: if (val > 1) return error(x, X11_BAD_VALUE, val); g->fill_rule = (uint8_t)val; break;
      case 10: case 11: {  /* tile: the GC's depth; stipple: depth 1 (else BadMatch); copied */
        x11_resource *r = x_find(x->s, val);
        if (!r || r->type != X11_RES_PIXMAP) return error(x, X11_BAD_PIXMAP, val);
        const xr_surface *ps = &((x11_pixmap *)r->obj)->surf;
        if (ps->depth != (bit == 10 ? g->depth : 1)) return error(x, X11_BAD_MATCH, val);
        xr_surface *copy = xw_tile_copy(x->s, ps);
        if (!copy) return error(x, X11_BAD_ALLOC, 0);
        *(bit == 10 ? &g->tile : &g->stipple) = copy;  /* the caller frees what it replaces (gc_settle) */
        break;
      }
      case 12: g->ts_x = (int16_t)val; break;
      case 13: g->ts_y = (int16_t)val; break;
      case 14: {
        x11_resource *r = x_find(x->s, val);
        if (!r || r->type != X11_RES_FONT) return error(x, X11_BAD_FONT, val);
        g->font = (const x11_font *)r->obj;
        break;
      }
      case 15: if (val > 1) return error(x, X11_BAD_VALUE, val); g->subwindow_mode = (uint8_t)val; break;
      case 16: if (val > 1) return error(x, X11_BAD_VALUE, val); g->graphics_exposures = (uint8_t)val; break;
      case 17: g->clip_x = (int16_t)val; break;
      case 18: g->clip_y = (int16_t)val; break;
      case 19:
        if (val != 0) {
          x11_resource *r = x_find(x->s, val);
          if (!r || r->type != X11_RES_PIXMAP) return error(x, X11_BAD_PIXMAP, val);
          const xr_surface *m = &((x11_pixmap *)r->obj)->surf;
          if (m->depth != 1) return error(x, X11_BAD_MATCH, val);
          int n = mask_rects(m, g->clip, X11_MAX_CLIP_RECTS);
          if (n < 0) return error(x, X11_BAD_IMPLEMENTATION, val);  /* too complex for the rectangle list */
          g->nclip = n;
          g->clip_mask = val;
          break;
        }
        g->nclip = -1;
        g->clip_mask = 0;
        break;
      case 20: g->dash_offset = (uint16_t)val; break;
      case 21: if (val == 0 || val > 255) return error(x, X11_BAD_VALUE, val); g->ndash = 1, g->dash[0] = (uint8_t)val; break;
      case 22: if (val > 1) return error(x, X11_BAD_VALUE, val); g->arc_mode = (uint8_t)val; break;
    }
  }
  return 0;
}

/* After gc_values on a copy `next` of the GC `now` (NULL: a new GC): tile and stipple copies
 * that changed are freed from the side that is dropped (the old ones on success, the new
 * ones on an error). */
static void gc_settle(x11_server *s, x11_gc *now, x11_gc *next, int ok) {
  xr_surface **a[2] = {now ? &now->tile : 0, now ? &now->stipple : 0}, **b[2] = {&next->tile, &next->stipple};
  for (int i = 0; i < 2; ++i) {
    xr_surface *old = a[i] ? *a[i] : 0;
    if (*b[i] == old) continue;
    if (ok) {
      if (a[i]) xw_tile_free(s, a[i]);
    } else {
      xw_tile_free(s, b[i]);
    }
  }
}

static size_t create_gc(ctx *x) {
  REQ_MIN(16);
  uint32_t gid = U32(4), did = U32(8), mask = U32(12);
  if (x->len != 16 + 4 * (size_t)popcount(mask)) return error(x, X11_BAD_LENGTH, 0);
  if (!new_id_ok(x, gid)) return error(x, X11_BAD_IDCHOICE, gid);
  x11_resource *d = x_find(x->s, did);
  if (!d || (d->type != X11_RES_WINDOW && d->type != X11_RES_PIXMAP)) return error(x, X11_BAD_DRAWABLE, did);
  uint8_t depth = d->type == X11_RES_PIXMAP ? (uint8_t)((x11_pixmap *)d->obj)->surf.depth : 16;
  if (d->type == X11_RES_WINDOW && ((x11_window *)d->obj)->input_only) return error(x, X11_BAD_MATCH, did);
  x11_gc tmp;
  gc_defaults(&tmp, depth);
  size_t e = gc_values(x, &tmp, mask, x->q + 16);
  x11_gc *g = e ? 0 : (x11_gc *)x_alloc(x->s, sizeof(x11_gc));
  if (!e && !g) e = error(x, X11_BAD_ALLOC, 0);
  if (e) {
    gc_settle(x->s, 0, &tmp, 0);
    return e;
  }
  *g = tmp;
  if (!x_add(x->s, gid, x->ci, X11_RES_GC, g)) {
    gc_settle(x->s, 0, g, 0);
    x_free(x->s, g);
    return error(x, X11_BAD_ALLOC, 0);
  }
  return 0;
}

static size_t change_gc(ctx *x) {
  REQ_MIN(12);
  uint32_t mask = U32(8);
  if (x->len != 12 + 4 * (size_t)popcount(mask)) return error(x, X11_BAD_LENGTH, 0);
  x11_gc *g = find_gc(x, U32(4));
  if (!g) return error(x, X11_BAD_GC, U32(4));
  x11_gc tmp = *g;  /* all or nothing */
  size_t e = gc_values(x, &tmp, mask, x->q + 12);
  gc_settle(x->s, g, &tmp, !e);
  if (!e) *g = tmp;
  return e;
}

static size_t copy_gc(ctx *x) {
  REQ_LEN(4);
  x11_gc *a = find_gc(x, U32(4)), *b = find_gc(x, U32(8));
  if (!a) return error(x, X11_BAD_GC, U32(4));
  if (!b) return error(x, X11_BAD_GC, U32(8));
  if (a->depth != b->depth) return error(x, X11_BAD_MATCH, 0);
  uint32_t m = U32(12);
  if (m & ~0x7FFFFFu) return error(x, X11_BAD_VALUE, m);
#define C(bit, f) if (m & (1u << (bit))) b->f = a->f
  C(0, function); C(1, plane_mask); C(2, fg); C(3, bg); C(4, line_width); C(5, line_style); C(6, cap_style);
  C(7, join_style); C(8, fill_style); C(9, fill_rule); C(12, ts_x); C(13, ts_y);
  C(14, font); C(15, subwindow_mode); C(16, graphics_exposures); C(17, clip_x); C(18, clip_y); C(20, dash_offset);
  C(22, arc_mode);
#undef C
  if (m & (1u << 21)) b->ndash = a->ndash, memcpy(b->dash, a->dash, sizeof(b->dash));
  if (m & (1u << 19)) {
    b->nclip = a->nclip;
    memcpy(b->clip, a->clip, sizeof(b->clip));
  }
  for (int i = 0; i < 2; ++i) {  /* tile, stipple: the destination gets its own copy */
    if (!(m & (1u << (10 + i)))) continue;
    xr_surface *src = i ? a->stipple : a->tile, **dst = i ? &b->stipple : &b->tile;
    xr_surface *copy = src ? xw_tile_copy(x->s, src) : 0;
    if (src && !copy) return error(x, X11_BAD_ALLOC, 0);
    xw_tile_free(x->s, dst);
    *dst = copy;
  }
  return 0;
}

static size_t set_dashes(ctx *x) {
  REQ_MIN(12);
  x11_gc *g = find_gc(x, U32(4));
  size_t n = U16(10);
  if (x->len != 12 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  if (!g) return error(x, X11_BAD_GC, U32(4));
  if (!n) return error(x, X11_BAD_VALUE, 0);
  for (size_t i = 0; i < n; ++i)
    if (!U8(12 + i)) return error(x, X11_BAD_VALUE, 0);
  g->dash_offset = U16(8);
  g->ndash = n <= XR_MAX_DASHES ? (uint8_t)n : 0;  /* longer lists: drawing with them is not implemented */
  if (g->ndash) memcpy(g->dash, x->q + 12, n);
  return 0;
}

static size_t set_clip_rectangles(ctx *x) {
  REQ_MIN(12);
  if ((x->len - 12) % 8) return error(x, X11_BAD_LENGTH, 0);
  x11_gc *g = find_gc(x, U32(4));
  if (!g) return error(x, X11_BAD_GC, U32(4));
  if (U8(1) > 3) return error(x, X11_BAD_VALUE, U8(1));
  size_t n = (x->len - 12) / 8;
  if (n > X11_MAX_CLIP_RECTS) return error(x, X11_BAD_ALLOC, 0);
  g->clip_x = I16(8), g->clip_y = I16(10);
  for (size_t i = 0; i < n; ++i) {
    int rx = I16(12 + 8 * i), ry = I16(14 + 8 * i), rw = U16(16 + 8 * i), rh = U16(18 + 8 * i);
    g->clip[i].x1 = (int16_t)rx, g->clip[i].y1 = (int16_t)ry;
    g->clip[i].x2 = (int16_t)(rx + rw), g->clip[i].y2 = (int16_t)(ry + rh);
  }
  g->nclip = (int)n;
  g->clip_mask = 0;
  return 0;
}

static size_t free_gc(ctx *x) {
  REQ_LEN(2);
  x11_resource *r = x_find(x->s, U32(4));
  if (!r || r->type != X11_RES_GC) return error(x, X11_BAD_GC, U32(4));
  free_resource(x->s, r);
  return 0;
}

/* ---- drawing -------------------------------------------------------------------------- */

typedef struct {
  const xr_surface *surf;
  xr_region *clip;         /* surface coordinates; from the region pool (released with the request) */
  int ox, oy;              /* drawable origin on the surface */
  x11_window *win;         /* NULL for pixmaps */
  int depth;
} target;

/* Resolves drawable + GC: the surface, clip region (visibility and GC clip rectangles) and
 * origin. Returns 0 or an error already written. */
static size_t resolve(ctx *x, uint32_t did, uint32_t gid, target *t, x11_gc **gc, int need_solid) {
  x11_resource *d = x_find(x->s, did);
  if (!d || (d->type != X11_RES_WINDOW && d->type != X11_RES_PIXMAP)) return error(x, X11_BAD_DRAWABLE, did);
  x11_gc *g = find_gc(x, gid);
  if (!g) return error(x, X11_BAD_GC, gid);
  t->clip = xr_pool_get();
  if (!t->clip) return error(x, X11_BAD_ALLOC, 0);
  if (d->type == X11_RES_WINDOW) {
    x11_window *w = (x11_window *)d->obj;
    if (w->input_only) return error(x, X11_BAD_MATCH, did);
    if (g->depth != 16) return error(x, X11_BAD_MATCH, gid);
    t->surf = &x->s->fb;
    t->win = w;
    t->depth = 16;
    xw_abs(w, &t->ox, &t->oy);
    xw_visible(w, t->clip, g->subwindow_mode == 1);
  } else {
    x11_pixmap *p = (x11_pixmap *)d->obj;
    if (g->depth != p->surf.depth) return error(x, X11_BAD_MATCH, gid);
    t->surf = &p->surf;
    t->win = 0;
    t->depth = p->surf.depth;
    t->ox = t->oy = 0;
    xr_region_set(t->clip, 0, 0, p->surf.w, p->surf.h);
  }
  if (g->nclip >= 0) {  /* GC clip rectangles, relative to the clip origin */
    int m = xr_pool_mark();
    xr_region *out = xr_pool_get(), *part = xr_pool_get();
    if (!out || !part) return error(x, X11_BAD_ALLOC, 0);
    for (int i = 0; i < g->nclip; ++i) {
      *part = *t->clip;
      xr_rect r = g->clip[i];
      r.x1 = (int16_t)(r.x1 + g->clip_x + t->ox), r.x2 = (int16_t)(r.x2 + g->clip_x + t->ox);
      r.y1 = (int16_t)(r.y1 + g->clip_y + t->oy), r.y2 = (int16_t)(r.y2 + g->clip_y + t->oy);
      xr_region_intersect(part, &r);
      for (int k = 0; k < part->n; ++k) {
        if (out->n < XR_MAX_RECTS) out->r[out->n++] = part->r[k];
        else out->overflow = 1;  /* too many pieces: conservative (draws less) */
      }
    }
    *t->clip = *out;
    xr_pool_release(m);
  }
  /* need_solid: 0 copies and ImageText (no fill style, no line style); 1 points, fills and
   * text (the fill style applies); 2 lines and arc outlines (dashed lines: not yet); 3
   * rectangle outlines (dashed lines too, with a dash list of up to XR_MAX_DASHES) */
  if (need_solid == 2 && g->line_style != 0) return error(x, X11_BAD_IMPLEMENTATION, 0);
  if (need_solid == 3 && g->line_style != 0 && !g->ndash) return error(x, X11_BAD_IMPLEMENTATION, 0);
  if (need_solid && g->fill_style) {
    static xr_fill f;  /* in use until the request ends (client_request clears it) */
    f.style = g->fill_style;
    f.pattern = g->fill_style == 1 ? g->tile : g->stipple;
    f.x = t->ox + g->ts_x, f.y = t->oy + g->ts_y;
    xr_fill_use(&f);
  }
  *gc = g;
  return 0;
}

static xr_paint paint_of(const x11_gc *g, int depth) {
  xr_paint p;
  p.function = g->function;
  p.plane_mask = g->plane_mask;
  p.fg = depth == 1 ? (g->fg & 1u) : (g->fg & 0xFFFFu);
  p.bg = depth == 1 ? (g->bg & 1u) : (g->bg & 0xFFFFu);
  return p;
}

static void touched(ctx *x, const target *t) {
  if (t->win) ++x->s->fb_rev;
}

#define DRAW_PROLOGUE(hdr, unit, need)                                               \
  REQ_MIN(hdr);                                                                      \
  if ((x->len - (hdr)) % (unit)) return error(x, X11_BAD_LENGTH, 0);                 \
  target t;                                                                          \
  x11_gc *g;                                                                         \
  size_t e_ = resolve(x, U32(4), U32(8), &t, &g, need);                              \
  if (e_) return e_;                                                                 \
  xr_paint p = paint_of(g, t.depth);                                                 \
  size_t n = (x->len - (hdr)) / (unit)

static size_t poly_fill_rectangle(ctx *x) {
  DRAW_PROLOGUE(12, 8, 1);
#ifdef X11_LATENCY_TRACE
  x11_latency_trace_polyfill_begin((unsigned)n);
#endif
  for (size_t i = 0; i < n; ++i) {
    const size_t o = 12 + 8 * i;
    xr_fill_rect(t.surf, t.clip, &p, t.ox + I16(o), t.oy + I16(o + 2), U16(o + 4), U16(o + 6));
    damage_commit(x->s);  /* one rectangle per primitive: far apart shapes stay apart */
  }
  touched(x, &t);
#ifdef X11_LATENCY_TRACE
  x11_latency_trace_polyfill_end();
#endif
  return 0;
}

/* Each rectangle as a closed PolyLine with the GC's line attributes (x11_outline.c). Odd dashes
 * of LineDoubleDash use the background (through the stipple for FillStippled); with a tiled or
 * opaque-stippled fill they look like even dashes, as the protocol says. */
static size_t poly_rectangle(ctx *x) {
  DRAW_PROLOGUE(12, 8, 3);
  xr_paint odd = p;
  odd.fg = p.bg;
  xr_line_attr la = {g->line_width, g->line_style, g->cap_style, g->join_style, g->ndash, g->dash, g->dash_offset,
                     g->fill_style};
  for (size_t i = 0; i < n; ++i) {
    const size_t o = 12 + 8 * i;
    xr_rect_outline(t.surf, t.clip, &p, &odd, &la, t.ox, t.oy, I16(o), I16(o + 2), U16(o + 4), U16(o + 6));
    damage_commit(x->s);
  }
  touched(x, &t);
  return 0;
}

static size_t poly_point(ctx *x) {
  DRAW_PROLOGUE(12, 4, 1);
  int px = 0, py = 0, rel = U8(1);
  if (rel > 1) return error(x, X11_BAD_VALUE, rel);
  for (size_t i = 0; i < n; ++i) {
    int ax = I16(12 + 4 * i), ay = I16(14 + 4 * i);
    if (rel && i) ax += px, ay += py;
    px = ax, py = ay;
    xr_point_set(t.surf, t.clip, &p, t.ox + ax, t.oy + ay);
  }
  touched(x, &t);
  return 0;
}

static size_t poly_line(ctx *x) {
  DRAW_PROLOGUE(12, 4, 2);
  int rel = U8(1);
  if (rel > 1) return error(x, X11_BAD_VALUE, rel);
  int lx = 0, ly = 0;
  int skip_last_seg = g->cap_style == 0;  /* CapNotLast */
  for (size_t i = 0; i < n; ++i) {
    int ax = I16(12 + 4 * i), ay = I16(14 + 4 * i);
    if (rel && i) ax += lx, ay += ly;
    if (i) {
      /* interior joints are drawn once: every segment but the last skips its end point */
      int last = i + 1 == n;
      xr_line(t.surf, t.clip, &p, t.ox + lx, t.oy + ly, t.ox + ax, t.oy + ay, g->line_width,
              g->line_width <= 1 ? (!last || skip_last_seg) : 0);
    } else if (n == 1) {
      xr_point_set(t.surf, t.clip, &p, t.ox + ax, t.oy + ay);
    }
    lx = ax, ly = ay;
  }
  touched(x, &t);
  return 0;
}

static size_t poly_segment(ctx *x) {
  DRAW_PROLOGUE(12, 8, 2);
  for (size_t i = 0; i < n; ++i) {
    const size_t o = 12 + 8 * i;
    xr_line(t.surf, t.clip, &p, t.ox + I16(o), t.oy + I16(o + 2), t.ox + I16(o + 4), t.oy + I16(o + 6),
            g->line_width, g->cap_style == 0);
    damage_commit(x->s);
  }
  touched(x, &t);
  return 0;
}

static size_t fill_poly(ctx *x) {
  REQ_MIN(16);
  if ((x->len - 16) % 4) return error(x, X11_BAD_LENGTH, 0);
  target t;
  x11_gc *g;
  size_t e = resolve(x, U32(4), U32(8), &t, &g, 1);
  if (e) return e;
  uint8_t shape = U8(12), mode = U8(13);
  if (shape > 2) return error(x, X11_BAD_VALUE, shape);
  if (mode > 1) return error(x, X11_BAD_VALUE, mode);
  size_t n = (x->len - 16) / 4;
  static xr_point pts[XR_MAX_POLY];
  if (n > XR_MAX_POLY) return error(x, X11_BAD_ALLOC, 0);  /* larger polygons: not supported here */
  int lx = 0, ly = 0;
  for (size_t i = 0; i < n; ++i) {
    int ax = I16(16 + 4 * i), ay = I16(18 + 4 * i);
    if (mode && i) ax += lx, ay += ly;
    lx = ax, ly = ay;
    pts[i].x = t.ox + ax, pts[i].y = t.oy + ay;
  }
  xr_paint p = paint_of(g, t.depth);
  xr_fill_polygon(t.surf, t.clip, &p, pts, (int)n, g->fill_rule);
  touched(x, &t);
  return 0;
}

static size_t poly_arc(ctx *x, int fill) {
  DRAW_PROLOGUE(12, 12, fill ? 1 : 2);
  for (size_t i = 0; i < n; ++i) {
    const size_t o = 12 + 12 * i;
    int ax = t.ox + I16(o), ay = t.oy + I16(o + 2), aw = U16(o + 4), ah = U16(o + 6), a1 = I16(o + 8), a2 = I16(o + 10);
    if (fill) xr_fill_arc(t.surf, t.clip, &p, ax, ay, aw, ah, a1, a2, g->arc_mode);
    else xr_arc(t.surf, t.clip, &p, ax, ay, aw, ah, a1, a2, g->line_width);
    damage_commit(x->s);
  }
  touched(x, &t);
  return 0;
}

static size_t clear_area(ctx *x) {
  REQ_LEN(4);
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  if (w->input_only) return error(x, X11_BAD_MATCH, U32(4));
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));  /* exposures: a BOOL */
  int cx = I16(8), cy = I16(10), cw = U16(12), ch = U16(14);
  if (!cw) cw = w->w - cx;  /* 0: to the right / bottom edge */
  if (!ch) ch = w->h - cy;
  int ax, ay;
  xw_abs(w, &ax, &ay);
  xr_region *area = xr_pool_get();
  if (!area) return error(x, X11_BAD_ALLOC, 0);
  xr_region_set(area, ax + cx, ay + cy, ax + cx + cw, ay + cy + ch);
  xw_clear(x->s, w, area, U8(1));
  return 0;
}

/* GraphicsExpose for the parts of a window source that were not visible, else NoExpose. */
static void graphics_exposures(ctx *x, x11_resource *src, uint32_t dst_id, int sx, int sy, int w, int h, int dx,
                               int dy) {
  xr_region *mp = xr_pool_get();
  if (!mp) return;  /* counted; the request answers BadAlloc */
  xr_region_clear(mp);
  if (src->type == X11_RES_WINDOW) {
    x11_window *sw = (x11_window *)src->obj;
    int ax, ay;
    xw_abs(sw, &ax, &ay);
    xr_region *vis = xr_pool_get();
    if (!vis) return;
    xw_visible(sw, vis, 0);
    xr_region_set(mp, ax + sx, ay + sy, ax + sx + w, ay + sy + h);
    for (int i = 0; i < vis->n; ++i) xr_region_subtract(mp, &vis->r[i]);
    xr_region_translate(mp, dx - (ax + sx), dy - (ay + sy));
  }
  const xr_region *missing = mp;
  int msb = x->msb;
  if (xr_region_empty(missing)) {
    uint8_t ev[32] = {14};  /* NoExpose */
    x_wr32(ev + 4, dst_id, msb);
    ev[10] = x->major;
    x_send_event(x->s, x->ci, ev);
    return;
  }
  for (int i = 0; i < missing->n; ++i) {
    uint8_t ev[32] = {13};  /* GraphicsExpose */
    const xr_rect *r = &missing->r[i];
    x_wr32(ev + 4, dst_id, msb);
    x_wr16(ev + 8, (uint16_t)r->x1, msb), x_wr16(ev + 10, (uint16_t)r->y1, msb);
    x_wr16(ev + 12, (uint16_t)(r->x2 - r->x1), msb), x_wr16(ev + 14, (uint16_t)(r->y2 - r->y1), msb);
    x_wr16(ev + 18, (uint16_t)(missing->n - 1 - i), msb);
    ev[20] = x->major;
    x_send_event(x->s, x->ci, ev);
  }
}

static size_t copy_area(ctx *x, int plane) {
  REQ_LEN(plane ? 8 : 7);
  x11_resource *src = x_find(x->s, U32(4));
  if (!src || (src->type != X11_RES_WINDOW && src->type != X11_RES_PIXMAP)) return error(x, X11_BAD_DRAWABLE, U32(4));
  target t;
  x11_gc *g;
  size_t e = resolve(x, U32(8), U32(12), &t, &g, 0);  /* copies: no fill or line style */
  if (e) return e;
  const xr_surface *ss;
  int sox = 0, soy = 0;
  if (src->type == X11_RES_WINDOW) {
    x11_window *sw = (x11_window *)src->obj;
    if (sw->input_only) return error(x, X11_BAD_MATCH, U32(4));
    ss = &x->s->fb;
    xw_abs(sw, &sox, &soy);
  } else {
    ss = &((x11_pixmap *)src->obj)->surf;
  }
  int sx = I16(16), sy = I16(18), dx = I16(20), dy = I16(22), w = U16(24), h = U16(26);
  xr_paint p = paint_of(g, t.depth);
  if (plane) {
    uint32_t bit = U32(28);
    if (!bit || (bit & (bit - 1)) || bit >= (1u << ss->depth)) return error(x, X11_BAD_VALUE, bit);
    if (ss->depth != 1) return error(x, X11_BAD_IMPLEMENTATION, bit);  /* planes of depth-16 sources: not yet */
    xr_copy_plane(t.surf, t.clip, &p, ss, sox + sx, soy + sy, w, h, t.ox + dx, t.oy + dy, 1);
  } else {
    if (ss->depth != t.depth) return error(x, X11_BAD_MATCH, 0);
    xr_copy(t.surf, t.clip, &p, ss, sox + sx, soy + sy, w, h, t.ox + dx, t.oy + dy);
  }
  touched(x, &t);
  if (g->graphics_exposures) graphics_exposures(x, src, U32(8), sx, sy, w, h, dx, dy);
  return 0;
}

static size_t put_image(ctx *x) {
  REQ_MIN(24);
  uint8_t format = U8(1), left = U8(20), depth = U8(21);
  int w = U16(12), h = U16(14), dx = I16(16), dy = I16(18);
  target t;
  x11_gc *g;
  size_t e = resolve(x, U32(4), U32(8), &t, &g, 0);
  if (e) return e;
  if (format > 2) return error(x, X11_BAD_VALUE, format);
  size_t row, need;
  int bits = format == 2 && depth == 16;
  if (format == 0 || depth == 1) {           /* one bit per pixel, LSBFirst, pad 32 */
    if (format == 0 ? depth != 1 : depth != t.depth) return error(x, X11_BAD_MATCH, depth);
    row = x_pad4(((size_t)w + left + 7) / 8);
  } else if (bits) {                         /* ZPixmap depth 16: 2 bytes per pixel, LSB */
    if (t.depth != 16) return error(x, X11_BAD_MATCH, depth);
    if (left) return error(x, X11_BAD_MATCH, left);
    row = x_pad4((size_t)w * 2);
  } else {
    return error(x, X11_BAD_IMPLEMENTATION, depth);  /* XYPixmap of depth 16 */
  }
  need = row * (size_t)h;
  if (x->len < 24 + need || x->len > 24 + x_pad4(need) + 3) return error(x, X11_BAD_LENGTH, 0);
  xr_paint p = paint_of(g, t.depth);
  const uint8_t *data = x->q + 24;
  for (int yy = 0; yy < h; ++yy) {
    const uint8_t *r = data + row * (size_t)yy;
    for (int xx = 0; xx < w; ++xx) {
      int px = t.ox + dx + xx, py = t.oy + dy + yy;
      if (bits) {
        xr_paint q = p;
        q.fg = (uint32_t)(r[2 * xx] | r[2 * xx + 1] << 8);
        xr_point_set(t.surf, t.clip, &q, px, py);
      } else {
        int b = xx + left;
        int v = (r[b >> 3] >> (b & 7)) & 1;
        if (format == 0) {  /* XYBitmap: 1 = foreground, 0 = background */
          xr_paint q = p;
          q.fg = v ? p.fg : p.bg;
          xr_point_set(t.surf, t.clip, &q, px, py);
        } else {            /* the bit is the depth-1 pixel value */
          xr_paint q = p;
          q.fg = (uint32_t)v;
          xr_point_set(t.surf, t.clip, &q, px, py);
        }
      }
    }
  }
  touched(x, &t);
  return 0;
}

/* ---- fonts and text ------------------------------------------------------------------- */

static int glob_ci(const char *pat, const char *s) {
  for (;; ++pat, ++s) {
    char p = *pat;
    if (p == '*') {
      while (*pat == '*') ++pat;
      if (!*pat) return 1;
      for (; *s; ++s)
        if (glob_ci(pat, s)) return 1;
      return 0;
    }
    char a = p >= 'A' && p <= 'Z' ? (char)(p + 32) : p, b = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
    if (!p) return !*s;
    if (!*s) return 0;
    if (p != '?' && a != b) return 0;
  }
}

/* Font names: the built-in fonts (x11_font.h), canonical name then aliases; host test
 * builds add the generated test fonts (-DX11_TEST_FONTS). */
static int font_count(void) {
#ifdef X11_TEST_FONTS
  return 2 + x11_test_font_count;
#else
  return 2;
#endif
}
static const x11_font *font_at(int k) {
#ifdef X11_TEST_FONTS
  if (k >= 2) return x11_test_fonts[k - 2];
#endif
  return x11_fonts[k];
}
static int font_name_count(const x11_font *f) { return 1 + f->naliases; }
static const char *font_name(const x11_font *f, int i) { return i ? f->aliases[i - 1] : f->name; }

static const x11_font *font_by_pattern(const uint8_t *name, size_t n) {
  char pat[256];
  if (n >= sizeof(pat)) return 0;
  memcpy(pat, name, n);
  pat[n] = 0;
  for (int k = 0; k < font_count(); ++k)
    for (int i = 0; i < font_name_count(font_at(k)); ++i)
      if (glob_ci(pat, font_name(font_at(k), i))) return font_at(k);
  return 0;
}

/* A font, or a GC's font (FONTABLE). */
static const x11_font *font_of(x11_server *s, uint32_t fid) {
  x11_resource *r = x_find(s, fid);
  if (r && r->type == X11_RES_GC) {
    const x11_font *f = ((x11_gc *)r->obj)->font;
    return f ? f : &x11_font_fixed;
  }
  return r && r->type == X11_RES_FONT ? (const x11_font *)r->obj : 0;
}

static const x11_font *gc_font(const x11_gc *g) { return g->font ? g->font : &x11_font_fixed; }

static size_t open_font(ctx *x) {
  REQ_MIN(12);
  uint32_t fid = U32(4);
  size_t n = U16(8);
  if (x->len != 12 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  if (!new_id_ok(x, fid)) return error(x, X11_BAD_IDCHOICE, fid);
  const x11_font *f = font_by_pattern(x->q + 12, n);
  if (!f) return error(x, X11_BAD_NAME, 0);  /* only the built-in fonts exist */
  if (!x_add(x->s, fid, x->ci, X11_RES_FONT, (void *)(uintptr_t)f)) return error(x, X11_BAD_ALLOC, 0);
  return 0;
}

/* CloseFont removes the ID; GCs that use the font keep it (the fonts are static). */
static size_t close_font(ctx *x) {
  REQ_LEN(2);
  x11_resource *r = x_find(x->s, U32(4));
  if (!r || r->type != X11_RES_FONT) return error(x, X11_BAD_FONT, U32(4));
  free_resource(x->s, r);
  return 0;
}

static void charinfo(uint8_t *p, int msb, const x11_glyph_metrics *m) {
  x_wr16(p, (uint16_t)m->lsb, msb), x_wr16(p + 2, (uint16_t)m->rsb, msb), x_wr16(p + 4, (uint16_t)m->width, msb);
  x_wr16(p + 6, (uint16_t)m->ascent, msb), x_wr16(p + 8, (uint16_t)m->descent, msb), x_wr16(p + 10, 0, msb);
}

static int font_nprops(const x11_font *f) { return f->props ? f->nprops : f->font_property ? 1 : 0; }

/* The properties: all of a generated font's, or FONT (the full name, as real servers report
 * it: Xlib reads it to learn the charset of a font opened by an alias such as "fixed").
 * Returns the number written at p (one fewer for each atom that could not be interned). */
static int font_props(x11_server *s, uint8_t *p, int msb, const x11_font *f) {
  int n = 0;
  for (int i = 0; i < font_nprops(f); ++i) {
    const char *name = f->props ? f->props[i].name : "FONT";
    const char *str = f->props ? f->props[i].str : f->font_property;
    uint32_t a = atom_intern(s, (const uint8_t *)name, strlen(name));
    uint32_t v = str ? atom_intern(s, (const uint8_t *)str, strlen(str)) : (uint32_t)f->props[i].value;
    if (!a || (str && !v)) continue;
    x_wr32(p + 8 * n, a, msb);
    x_wr32(p + 8 * n + 4, v, msb);
    ++n;
  }
  return n;
}

/* The font information common to QueryFont and ListFontsWithInfo (bytes 8..59). */
static void font_info(uint8_t *p, int msb, const x11_font *f) {
  charinfo(p + 8, msb, &f->min_bounds);
  charinfo(p + 24, msb, &f->max_bounds);
  x_wr16(p + 40, f->first, msb);
  x_wr16(p + 42, f->last, msb);
  x_wr16(p + 44, f->default_char, msb);
  x_wr16(p + 46, 0, msb);  /* number of properties: set by the caller */
  p[48] = f->direction;
  p[49] = f->min_byte1, p[50] = f->max_byte1;
  int all = 1;
  for (int i = 0; i < x11_font_slots(f) && all; ++i) {
    const x11_glyph_metrics *m = x11_font_slot_metrics(f, i);
    all = m->lsb || m->rsb || m->width || m->ascent || m->descent;
  }
  p[51] = (uint8_t)all;
  x_wr16(p + 52, (uint16_t)f->ascent, msb);
  x_wr16(p + 54, (uint16_t)f->descent, msb);
}

static size_t query_font(ctx *x) {
  REQ_LEN(2);
  const x11_font *f = font_of(x->s, U32(4));
  if (!f) return error(x, X11_BAD_FONT, U32(4));
  size_t nchars = (size_t)x11_font_slots(f);
  size_t extra = 28 + 8 * (size_t)font_nprops(f) + 12 * nchars;
  if (32 + extra > x->cap) return error(x, X11_BAD_ALLOC, 0);  /* not for any font this server has */
  uint8_t *p = reply(x, 0, extra);
  font_info(p, x->msb, f);
  int np = font_props(x->s, p + 60, x->msb, f);
  extra = 28 + 8 * (size_t)np + 12 * nchars;
  x_wr32(p + 4, (uint32_t)(extra / 4), x->msb);
  x_wr16(p + 46, (uint16_t)np, x->msb);
  x_wr32(p + 56, (uint32_t)nchars, x->msb);
  uint8_t *ci = p + 60 + 8 * np;
  for (size_t i = 0; i < nchars; ++i) charinfo(ci + 12 * i, x->msb, x11_font_slot_metrics(f, (int)i));
  return 32 + extra;
}

static size_t list_fonts(ctx *x) {
  REQ_MIN(8);
  size_t n = U16(6), max = U16(4);
  if (x->len != 8 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  char pat[256];
  if (n >= sizeof(pat)) n = sizeof(pat) - 1;
  memcpy(pat, x->q + 8, n);
  pat[n] = 0;
  /* the canonical names and aliases that match: counted, then written */
  size_t count = 0, bytes = 0;
  uint8_t *r = 0;
  for (int pass = 0; pass < 2; ++pass) {
    if (pass) {
      r = reply(x, 0, x_pad4(bytes));
      x_wr16(r + 8, (uint16_t)count, x->msb);
      count = bytes = 0;
    }
    for (int k = 0; k < font_count(); ++k)
      for (int i = 0; i < font_name_count(font_at(k)) && count < max; ++i) {
        const char *name = font_name(font_at(k), i);
        size_t l = strlen(name);
        if (!glob_ci(pat, name) || l > 255 || 32 + x_pad4(bytes + 1 + l) > x->cap) continue;
        if (pass) {
          r[32 + bytes] = (uint8_t)l;
          memcpy(r + 33 + bytes, name, l);
        }
        bytes += 1 + l;
        ++count;
      }
  }
  return 32 + x_pad4(bytes);
}

/* One reply per matching name (font information as QueryFont, without per-character
 * metrics, then the name), followed by the terminating reply with an empty name. */
static size_t list_fonts_with_info(ctx *x) {
  REQ_MIN(8);
  size_t n = U16(6), max = U16(4);
  if (x->len != 8 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  char pat[256];
  if (n >= sizeof(pat)) n = sizeof(pat) - 1;
  memcpy(pat, x->q + 8, n);
  pat[n] = 0;
  size_t total = 0, count = 0;
  int msb = x->msb;
  for (int k = 0; k < font_count(); ++k)
    for (int i = 0; i < font_name_count(font_at(k)) && count < max; ++i) {
      const x11_font *f = font_at(k);
      const char *name = font_name(f, i);
      if (!glob_ci(pat, name)) continue;
      size_t l = strlen(name), size = 60 + 8 * (size_t)font_nprops(f) + x_pad4(l);
      if (l > 255 || total + size + 60 > x->cap) break;
      uint8_t *p = x->out + total;
      memset(p, 0, size);
      p[0] = 1;
      p[1] = (uint8_t)l;
      x_wr16(p + 2, x->c->seq, msb);
      font_info(p, msb, f);
      int np = font_props(x->s, p + 60, msb, f);
      x_wr16(p + 46, (uint16_t)np, msb);
      x_wr32(p + 4, (uint32_t)(7 + 2 * np + x_pad4(l) / 4), msb);
      x_wr32(p + 56, 1, msb);  /* replies-hint */
      memcpy(p + 60 + 8 * np, name, l);
      total += 60 + 8 * (size_t)np + x_pad4(l);
      ++count;
    }
  uint8_t *last = x->out + total;  /* "last reply" marker */
  memset(last, 0, 60);
  last[0] = 1;
  x_wr16(last + 2, x->c->seq, msb);
  x_wr32(last + 4, 7, msb);
  return total + 60;
}

/* ---- text: 8-bit strings are characters (byte1 0), 16-bit ones CHAR2B (byte1, byte2) ---- */

typedef struct {
  const uint8_t *s;
  size_t n;  /* characters */
  int wide;  /* 2 bytes per character */
} text;

static int text_at(const text *t, size_t i) { return t->wide ? t->s[2 * i] << 8 | t->s[2 * i + 1] : t->s[i]; }

/* An undefined character is the default character, or nothing if that is undefined too. */
static const x11_glyph_metrics *text_glyph(const x11_font *f, int c, int *used) {
  const x11_glyph_metrics *m = x11_font_glyph(f, c);
  if (!m) m = x11_font_glyph(f, c = f->default_char);
  *used = c;
  return m;
}

typedef struct {
  int32_t width, left, right, ascent, descent;
} extents;

/* QueryTextExtents' overall values: width the sum of widths, left/right the extremes of
 * pen + lsb / pen + rsb, ascent/descent the largest (all 0 for an empty string). */
static extents text_extents(const x11_font *f, const text *t) {
  extents e = {0, 0, 0, 0, 0};
  int first = 1;
  for (size_t i = 0; i < t->n; ++i) {
    int c;
    const x11_glyph_metrics *m = text_glyph(f, text_at(t, i), &c);
    if (!m) continue;
    int32_t l = e.width + m->lsb, r = e.width + m->rsb;
    if (first || l < e.left) e.left = l;
    if (first || r > e.right) e.right = r;
    if (first || m->ascent > e.ascent) e.ascent = m->ascent;
    if (first || m->descent > e.descent) e.descent = m->descent;
    first = 0;
    e.width += m->width;
  }
  return e;
}

/* Draws the glyphs at pen position x0 on the baseline; returns the new pen position. */
static int draw_glyphs(target *tg, const xr_paint *p, const x11_font *f, int x0, int baseline, const text *t) {
  for (size_t i = 0; i < t->n; ++i) {
    int c;
    const x11_glyph_metrics *m = text_glyph(f, text_at(t, i), &c);
    if (!m) continue;
    int slot = x11_font_slot(f, c);
    for (int ry = 0; ry < m->ascent + m->descent; ++ry)
      for (int rx = 0; rx < m->rsb - m->lsb; ++rx)
        if (x11_font_pixel(f, slot, rx, ry)) xr_point_set(tg->surf, tg->clip, p, x0 + m->lsb + rx, baseline - m->ascent + ry);
    x0 += m->width;
  }
  return x0;
}

/* QueryTextExtents: a font or a GC; odd-length says the last CHAR2B is padding. */
static size_t query_text_extents(ctx *x) {
  REQ_MIN(8);
  size_t chars = (x->len - 8) / 2;
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));
  if (U8(1)) {
    if (!chars) return error(x, X11_BAD_LENGTH, 0);
    --chars;
  }
  const x11_font *f = font_of(x->s, U32(4));
  if (!f) return error(x, X11_BAD_FONT, U32(4));
  text t = {x->q + 8, chars, 1};
  extents e = text_extents(f, &t);
  uint8_t *r = reply(x, f->direction, 0);
  x_wr16(r + 8, (uint16_t)f->ascent, x->msb);
  x_wr16(r + 10, (uint16_t)f->descent, x->msb);
  x_wr16(r + 12, (uint16_t)e.ascent, x->msb);
  x_wr16(r + 14, (uint16_t)e.descent, x->msb);
  x_wr32(r + 16, (uint32_t)e.width, x->msb);
  x_wr32(r + 20, (uint32_t)e.left, x->msb);
  x_wr32(r + 24, (uint32_t)e.right, x->msb);
  return 32;
}

/* ImageText8/16: the background rectangle (x, y - font-ascent, overall-width, font-ascent +
 * font-descent) in the GC's background, then the glyphs in its foreground; the function and
 * fill style are ignored (always Copy, Solid). */
static size_t image_text(ctx *x, int wide) {
  REQ_MIN(16);
  size_t n = U8(1);
  if (x->len != 16 + x_pad4(n * (wide ? 2 : 1))) return error(x, X11_BAD_LENGTH, 0);
  target tg;
  x11_gc *g;
  size_t e = resolve(x, U32(4), U32(8), &tg, &g, 0);
  if (e) return e;
  const x11_font *f = gc_font(g);
  text t = {x->q + 16, n, wide};
  int tx = tg.ox + I16(12), ty = tg.oy + I16(14);
  xr_paint p = paint_of(g, tg.depth);
  xr_paint bgp = p;
  bgp.function = 3;
  bgp.fg = p.bg;
  xr_fill_rect(tg.surf, tg.clip, &bgp, tx, ty - f->ascent, text_extents(f, &t).width, f->ascent + f->descent);
  bgp.fg = p.fg;
  draw_glyphs(&tg, &bgp, f, tx, ty, &t);
  touched(x, &tg);
  return 0;
}

/* PolyText8/16: TEXTITEMs are a string (length 0..254, delta, characters) or a font shift
 * (255, then the font ID most significant byte first; it also sets the GC's font). Fewer
 * than 2 bytes left (or a zero length in the padding) end the list. */
static size_t poly_text(ctx *x, int wide) {
  REQ_MIN(16);
  target tg;
  x11_gc *g;
  size_t e = resolve(x, U32(4), U32(8), &tg, &g, 1);
  if (e) return e;
  xr_paint p = paint_of(g, tg.depth);
  int tx = tg.ox + I16(12), ty = tg.oy + I16(14);
  size_t o = 16, unit = wide ? 2 : 1;
  while (o + 1 < x->len) {
    uint8_t l = U8(o);
    if (l == 255) {
      if (o + 5 > x->len) return error(x, X11_BAD_LENGTH, 0);
      uint32_t fid = (uint32_t)U8(o + 1) << 24 | (uint32_t)U8(o + 2) << 16 | (uint32_t)U8(o + 3) << 8 | U8(o + 4);
      x11_resource *r = x_find(x->s, fid);
      if (!r || r->type != X11_RES_FONT) return error(x, X11_BAD_FONT, fid);
      g->font = (const x11_font *)r->obj;
      o += 5;
      continue;
    }
    if (o + 2 + l * unit > x->len) return error(x, X11_BAD_LENGTH, 0);
    tx += (int8_t)U8(o + 1);  /* a zero-length item only moves the pen */
    text t = {x->q + o + 2, l, wide};
    tx = draw_glyphs(&tg, &p, gc_font(g), tx, ty, &t);
    o += 2 + l * unit;
  }
  touched(x, &tg);
  return 0;
}

/* ---- colours (one TrueColor RGB565 colormap) ---------------------------------------- */

static uint32_t rgb565(uint16_t r, uint16_t g, uint16_t b) {
  return (uint32_t)((r >> 11) << 11 | (g >> 10) << 5 | (b >> 11));
}

static void exact(uint32_t px, uint16_t *r, uint16_t *g, uint16_t *b) {
  uint32_t r5 = px >> 11 & 31, g6 = px >> 5 & 63, b5 = px & 31;
  *r = (uint16_t)(r5 * 65535 / 31), *g = (uint16_t)(g6 * 65535 / 63), *b = (uint16_t)(b5 * 65535 / 31);
}


/* The channel entries of an RGB565 pixel in an x11_cmap_use count array. */
static int chan_index(int c, uint32_t px) {
  return c == 0 ? (int)(px >> 11 & 31) : c == 1 ? 32 + (int)(px >> 5 & 63) : 96 + (int)(px & 31);
}

/* Records that the requesting client allocated pixel px in cmap. 0: no memory for the record. */
static int cmap_alloc(ctx *x, uint32_t cmap, uint32_t px) {
  x11_cmap_use *u = cmap_use(x->s, x->ci, cmap, 1);
  if (!u) return 0;
  for (int c = 0; c < 3; ++c) {
    uint8_t *n = &u->count[chan_index(c, px)];
    if (*n < 255) ++*n;
  }
  return 1;
}

static size_t alloc_color(ctx *x) {
  REQ_LEN(4);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  uint32_t px = rgb565(U16(8), U16(10), U16(12));
  if (!cmap_alloc(x, U32(4), px)) return error(x, X11_BAD_ALLOC, 0);
  uint16_t r, g, b;
  exact(px, &r, &g, &b);
  uint8_t *p = reply(x, 0, 0);
  x_wr16(p + 8, r, x->msb), x_wr16(p + 10, g, x->msb), x_wr16(p + 12, b, x->msb);
  x_wr32(p + 16, px, x->msb);
  return 32;
}

static int lookup_name(const uint8_t *s, size_t n, uint16_t *r, uint16_t *g, uint16_t *b) {
  uint8_t rgb[3];
  if (!x11_color_lookup((const char *)s, n, rgb)) return 0;
  *r = (uint16_t)(rgb[0] * 257), *g = (uint16_t)(rgb[1] * 257), *b = (uint16_t)(rgb[2] * 257);
  return 1;
}

static size_t named_color(ctx *x, int alloc) {
  REQ_MIN(12);
  size_t n = U16(8);
  if (x->len != 12 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  uint16_t r, g, b;
  if (!lookup_name(x->q + 12, n, &r, &g, &b)) return error(x, X11_BAD_NAME, 0);
  uint32_t px = rgb565(r, g, b);
  if (alloc && !cmap_alloc(x, U32(4), px)) return error(x, X11_BAD_ALLOC, 0);
  uint16_t vr, vg, vb;
  exact(px, &vr, &vg, &vb);
  uint8_t *p = reply(x, 0, 0);
  int o = 8;
  if (alloc) x_wr32(p + 8, px, x->msb), o = 12;
  x_wr16(p + o, r, x->msb), x_wr16(p + o + 2, g, x->msb), x_wr16(p + o + 4, b, x->msb);        /* exact */
  x_wr16(p + o + 6, vr, x->msb), x_wr16(p + o + 8, vg, x->msb), x_wr16(p + o + 10, vb, x->msb);  /* visual */
  return 32;
}

/* FreeColors, as X.Org does for TrueColor: each channel separately, the pixel's entry with
 * every combination of the plane-mask bits in that channel (at most 32, 64, 32 entries). An
 * entry the client did not allocate is BadAccess, a pixel outside the visual BadValue; the
 * other entries are still freed, and the first error is reported. */
static size_t free_colors(ctx *x) {
  REQ_MIN(12);
  if (x->len % 4) return error(x, X11_BAD_LENGTH, 0);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  uint32_t mask = U32(8);
  x11_cmap_use *u = cmap_use(x->s, x->ci, U32(4), 0);
  static const uint8_t shift[3] = {11, 5, 0}, max[3] = {31, 63, 31};
  uint8_t code = 0;
  uint32_t bad = 0;
  for (size_t o = 12; o < x->len; o += 4) {
    uint32_t px = U32(o);
    if ((px | mask) > 0xFFFF) {
      if (!code) code = X11_BAD_VALUE, bad = px;
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      uint32_t base = px >> shift[c] & max[c], cm = mask >> shift[c] & max[c], sub = 0;
      do {
        int i = (c == 0 ? 0 : c == 1 ? 32 : 96) + (int)(base | sub);
        if (!u || !u->count[i]) {
          if (!code) code = X11_BAD_ACCESS, bad = px;
        } else if (u->count[i] < 255) {
          --u->count[i];
        }
        sub = (sub - cm) & cm;  /* the next subset of cm */
      } while (sub);
    }
  }
  return code ? error(x, code, bad) : 0;
}

/* CopyColormapAndFree: a new colormap for the same visual; the client's allocations move from
 * the source to it. (TrueColor: nothing else to copy.) */
static size_t copy_colormap_and_free(ctx *x) {
  REQ_LEN(3);
  uint32_t mid = U32(4), src = U32(8);
  if (!new_id_ok(x, mid)) return error(x, X11_BAD_IDCHOICE, mid);
  if (!cmap_ok(x, src)) return error(x, X11_BAD_COLORMAP, src);
  if (!x_add(x->s, mid, x->ci, X11_RES_COLORMAP, 0)) return error(x, X11_BAD_ALLOC, 0);
  x11_cmap_use *u = cmap_use(x->s, x->ci, src, 0);
  if (u) u->cmap = mid;
  return 0;
}

/* AllocColorCells / AllocColorPlanes: writable cells. The only visual is TrueColor, a static
 * class with none, so after the checks the answer is BadAlloc (as X.Org). */
static size_t alloc_color_cells(ctx *x, int planes) {
  REQ_LEN(planes ? 4 : 3);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  if (!U16(8)) return error(x, X11_BAD_VALUE, 0);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));
  return error(x, X11_BAD_ALLOC, 0);
}

/* StoreColors / StoreNamedColor: TrueColor cells are read-only: BadAccess after the checks
 * (StoreNamedColor: an unknown name is BadName first, as X.Org). */
static size_t store_colors(ctx *x) {
  REQ_MIN(8);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  if ((x->len - 8) % 12) return error(x, X11_BAD_LENGTH, 0);
  return error(x, X11_BAD_ACCESS, 0);
}

static size_t store_named_color(ctx *x) {
  REQ_MIN(16);
  size_t n = U16(12);
  if (x->len != 16 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  uint16_t r, g, b;
  if (!lookup_name(x->q + 16, n, &r, &g, &b)) return error(x, X11_BAD_NAME, 0);
  return error(x, X11_BAD_ACCESS, 0);
}

static size_t query_colors(ctx *x) {
  REQ_MIN(8);
  if (x->len % 4) return error(x, X11_BAD_LENGTH, 0);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  size_t n = (x->len - 8) / 4;
  if (32 + 8 * n > x->cap) return error(x, X11_BAD_ALLOC, 0);
  for (size_t i = 0; i < n; ++i)
    if (U32(8 + 4 * i) > 0xFFFF) return error(x, X11_BAD_VALUE, U32(8 + 4 * i));
  uint8_t *p = reply(x, 0, 8 * n);
  x_wr16(p + 8, (uint16_t)n, x->msb);
  for (size_t i = 0; i < n; ++i) {
    uint16_t r, g, b;
    exact(U32(8 + 4 * i), &r, &g, &b);
    x_wr16(p + 32 + 8 * i, r, x->msb), x_wr16(p + 34 + 8 * i, g, x->msb), x_wr16(p + 36 + 8 * i, b, x->msb);
  }
  return 32 + 8 * n;
}

/* ---- cursors (stored, not shown: the panel has no pointer image) -------------------- */

static size_t create_cursor(ctx *x, int glyph) {
  REQ_LEN(8);
  uint32_t cid = U32(4);
  if (!new_id_ok(x, cid)) return error(x, X11_BAD_IDCHOICE, cid);
  x11_resource *src = x_find(x->s, U32(8));
  if (glyph) {  /* source and mask characters must exist in their fonts */
    if (!src || src->type != X11_RES_FONT) return error(x, X11_BAD_FONT, U32(8));
    x11_resource *mask = U32(12) ? x_find(x->s, U32(12)) : 0;
    if (U32(12) && (!mask || mask->type != X11_RES_FONT)) return error(x, X11_BAD_FONT, U32(12));
    if (!x11_font_glyph((const x11_font *)src->obj, U16(16))) return error(x, X11_BAD_VALUE, U16(16));
    if (mask && !x11_font_glyph((const x11_font *)mask->obj, U16(18))) return error(x, X11_BAD_VALUE, U16(18));
  } else {  /* CreateCursor: source and mask of depth 1, the same size, the hotspot inside; as X.Org,
             * the far edge counts as inside (hotspot == width/height: GDK's 1x1 blank cursor) */
    if (!src || src->type != X11_RES_PIXMAP) return error(x, X11_BAD_PIXMAP, U32(8));
    const xr_surface *ss = &((x11_pixmap *)src->obj)->surf;
    x11_resource *mask = U32(12) ? x_find(x->s, U32(12)) : 0;
    if (U32(12) && (!mask || mask->type != X11_RES_PIXMAP)) return error(x, X11_BAD_PIXMAP, U32(12));
    if (ss->depth != 1) return error(x, X11_BAD_MATCH, U32(8));
    if (mask) {
      const xr_surface *ms = &((x11_pixmap *)mask->obj)->surf;
      if (ms->depth != 1 || ms->w != ss->w || ms->h != ss->h) return error(x, X11_BAD_MATCH, U32(12));
    }
    if (U16(28) > ss->w || U16(30) > ss->h) return error(x, X11_BAD_MATCH, 0);
  }
  if (!x_add(x->s, cid, x->ci, X11_RES_CURSOR, 0)) return error(x, X11_BAD_ALLOC, 0);
  return 0;
}

static size_t free_cursor(ctx *x) {
  REQ_LEN(2);
  x11_resource *r = x_find(x->s, U32(4));
  if (!r || r->type != X11_RES_CURSOR) return error(x, X11_BAD_CURSOR, U32(4));
  free_resource(x->s, r);
  return 0;
}

/* RecolorCursor: cursor, fore-red/green/blue, back-red/green/blue (CARD16 each). Validated in
 * full (length, cursor: any client's, as in Xorg). The new colours are not kept: this server
 * draws no pointer image (the panel is a touchscreen), and no core request reads a cursor's
 * colours back, so they could never be observed; CreateCursor does not keep them either. */
static size_t recolor_cursor(ctx *x) {
  REQ_LEN(5);
  x11_resource *r = x_find(x->s, U32(4));
  if (!r || r->type != X11_RES_CURSOR) return error(x, X11_BAD_CURSOR, U32(4));
  return 0;
}

/* ---- misc ----------------------------------------------------------------------------- */

static size_t query_best_size(ctx *x) {
  REQ_LEN(3);
  uint8_t cls = U8(1);
  uint32_t did = U32(4);
  uint16_t w = U16(8), h = U16(10);
  if (cls > 2) return error(x, X11_BAD_VALUE, cls);
  x11_resource *d = x_find(x->s, did);
  if (!d || (d->type != X11_RES_WINDOW && d->type != X11_RES_PIXMAP)) return error(x, X11_BAD_DRAWABLE, did);
  if (cls != 0 && d->type == X11_RES_WINDOW && ((x11_window *)d->obj)->input_only)
    return error(x, X11_BAD_MATCH, 0);  /* Tile, Stipple: not for an InputOnly window */
  if (cls == 0) {
    if (w > 32) w = 32;
    if (h > 32) h = 32;
  }
  uint8_t *r = reply(x, 0, 0);
  x_wr16(r + 8, w, x->msb);
  x_wr16(r + 10, h, x->msb);
  return 32;
}

static size_t query_extension(ctx *x) {
  REQ_MIN(8);
  size_t n = U16(4);
  if (x->len != 8 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  reply(x, 0, 0);  /* present = False: this server has no extensions */
  return 32;
}

static size_t list_extensions(ctx *x) {
  REQ_LEN(1);
  reply(x, 0, 0);
  return 32;
}

/* ---- selections ----------------------------------------------------------------------- */

static x11_selection *selection(x11_server *s, uint32_t atom) {
  for (int i = 0; i < X11_MAX_SELECTIONS; ++i) {
    x11_selection *e = &s->selections[i];
    if (e->atom != atom) continue;
    x11_resource *r = x_find(s, e->window);  /* the owner window was destroyed: owner None */
    if (!r || r->type != X11_RES_WINDOW) memset(e, 0, sizeof(*e));
    return e->atom ? e : 0;
  }
  return 0;
}

static size_t set_selection_owner(ctx *x) {
  REQ_LEN(4);
  uint32_t win = U32(4), atom = U32(8), t = U32(12), now = x_now(x->s);
  if (win && !find_window(x, win)) return error(x, X11_BAD_WINDOW, win);
  if (!x11_atom_name(x->s, atom)) return error(x, X11_BAD_ATOM, atom);
  if (!t) t = now;  /* CurrentTime */
  x11_selection *e = selection(x->s, atom);
  if ((e && t < e->time) || t > now) return 0;  /* older than the last change, or in the future */
  if (e && e->client != x->ci) {  /* the previous owner loses it */
    int c = e->client, msb = x->s->clients[c].msb;
    uint8_t ev[32] = {29};  /* SelectionClear */
    x_wr32(ev + 4, t, msb), x_wr32(ev + 8, e->window, msb), x_wr32(ev + 12, atom, msb);
    x_send_event(x->s, c, ev);
  }
  if (!win) {
    if (e) memset(e, 0, sizeof(*e));
    return 0;
  }
  if (!e)
    for (int i = 0; i < X11_MAX_SELECTIONS && !e; ++i)
      if (!x->s->selections[i].atom) e = &x->s->selections[i];
  if (!e) return error(x, X11_BAD_ALLOC, 0);
  e->atom = atom, e->window = win, e->time = t, e->client = (int16_t)x->ci;
  return 0;
}

static size_t get_selection_owner(ctx *x) {
  REQ_LEN(2);
  if (!x11_atom_name(x->s, U32(4))) return error(x, X11_BAD_ATOM, U32(4));
  x11_selection *e = selection(x->s, U32(4));
  uint8_t *r = reply(x, 0, 0);
  x_wr32(r + 8, e ? e->window : 0, x->msb);
  return 32;
}

static size_t convert_selection(ctx *x) {
  REQ_LEN(6);
  uint32_t req = U32(4), sel = U32(8), target = U32(12), prop = U32(16), t = U32(20);
  if (!find_window(x, req)) return error(x, X11_BAD_WINDOW, req);
  if (!x11_atom_name(x->s, sel)) return error(x, X11_BAD_ATOM, sel);
  if (!x11_atom_name(x->s, target)) return error(x, X11_BAD_ATOM, target);
  if (prop && !x11_atom_name(x->s, prop)) return error(x, X11_BAD_ATOM, prop);
  x11_selection *e = selection(x->s, sel);
  int c = e ? e->client : x->ci, msb = x->s->clients[c].msb;
  uint8_t ev[32] = {0};
  if (e) {  /* SelectionRequest to the owner */
    ev[0] = 30;
    x_wr32(ev + 4, t, msb), x_wr32(ev + 8, e->window, msb), x_wr32(ev + 12, req, msb);
    x_wr32(ev + 16, sel, msb), x_wr32(ev + 20, target, msb), x_wr32(ev + 24, prop, msb);
  } else {  /* nobody owns it: SelectionNotify with property None to the requestor */
    ev[0] = 31;
    x_wr32(ev + 4, t, msb), x_wr32(ev + 8, req, msb), x_wr32(ev + 12, sel, msb), x_wr32(ev + 16, target, msb);
  }
  x_send_event(x->s, c, ev);
  return 0;
}

/* ---- SendEvent ------------------------------------------------------------------------ */

/* Fields of each core event after the first 4 bytes, as 16-bit unit indexes (offset / 2):
 * bit i of l: a 32-bit field at offset 2i; of s: a 16-bit field. For byte-order conversion
 * between the sender and the receiver. ClientMessage data depends on its format. */
static const struct { uint16_t l, s; } EVENT_FIELDS[35] = {
    [2] = {0x154, 0x7C00}, [3] = {0x154, 0x7C00}, [4] = {0x154, 0x7C00}, [5] = {0x154, 0x7C00},
    [6] = {0x154, 0x7C00}, [7] = {0x154, 0x7C00}, [8] = {0x154, 0x7C00}, [9] = {0x4, 0}, [10] = {0x4, 0},
    [12] = {0x4, 0x1F0}, [13] = {0x4, 0x3F0}, [14] = {0x4, 0x10}, [15] = {0x4, 0}, [16] = {0x14, 0x7C0},
    [17] = {0x14, 0}, [18] = {0x14, 0}, [19] = {0x14, 0}, [20] = {0x14, 0}, [21] = {0x54, 0x300},
    [22] = {0x54, 0x1F00}, [23] = {0x54, 0x3F00}, [24] = {0x14, 0xC0}, [25] = {0x4, 0x30},
    [26] = {0x54, 0}, [27] = {0x54, 0}, [28] = {0x54, 0}, [29] = {0x54, 0}, [30] = {0x1554, 0},
    [31] = {0x554, 0}, [32] = {0x14, 0}, [33] = {0x14, 0}};

static void swap_event(uint8_t *ev) {
  uint8_t code = ev[0] & 0x7F, t;
  uint16_t l = code < 35 ? EVENT_FIELDS[code].l : 0, sh = code < 35 ? EVENT_FIELDS[code].s : 0;
  if (code == 33) {  /* ClientMessage data: format 8, 16 or 32 */
    if (ev[1] == 16) sh |= 0xFFC0;   /* ten 16-bit units from offset 12 */
    if (ev[1] == 32) l |= 0x5540;    /* five 32-bit units from offset 12 */
  }
  for (int i = 2; i < 16; ++i) {
    uint8_t *p = ev + 2 * i;
    if (l & (1u << i)) t = p[0], p[0] = p[3], p[3] = t, t = p[1], p[1] = p[2], p[2] = t;
    else if (sh & (1u << i)) t = p[0], p[0] = p[1], p[1] = t;
  }
}

/* SendEvent's copy of `in` to client c, in c's byte order. Returns 1 if c is active. */
static int send_copy(ctx *x, int c, const uint8_t *in) {
  if (!x->s->clients[c].active) return 0;
  uint8_t ev[32];
  memcpy(ev, in, 32);
  ev[0] |= 0x80;  /* sent by SendEvent */
  if (x->s->clients[c].msb != x->msb) swap_event(ev);
  x_send_event(x->s, c, ev);
  return 1;
}

static size_t send_event(ctx *x) {
  REQ_LEN(11);
  uint8_t propagate = U8(1);
  uint32_t dest = U32(4), mask = U32(8);
  const uint8_t *in = x->q + 12;
  uint8_t code = in[0] & 0x7F;
  if (propagate > 1) return error(x, X11_BAD_VALUE, propagate);
  if (mask & 0xFE000000u) return error(x, X11_BAD_VALUE, mask);
  if (code < 2 || code > 34) return error(x, X11_BAD_VALUE, in[0]);  /* core events only: no extensions */
  x11_window *w;
  int to_focus = dest == 1;
  if (dest == 0) {  /* PointerWindow: the window under the pointer */
    w = x->s->sprite;
  } else if (dest == 1) {  /* InputFocus */
    if (x->s->focus == 0) return 0;
    w = x->s->focus == 1 ? x->s->root : find_window(x, x->s->focus);
    if (!w) return 0;
  } else if (!(w = find_window(x, dest))) {
    return error(x, X11_BAD_WINDOW, dest);
  }
  int sent = 0;
  for (x11_window *t = w; t && !sent; t = propagate ? t->parent : 0) {
    if (!mask) {  /* event-mask 0: the window's creator only */
      if (t == w && t->owner < X11_SERVER_CLIENTS) sent = send_copy(x, t->owner, in);
      break;
    }
    XW_FOR_SEL(t, e)  /* every client that selected one of the bits */
      if (e->mask & mask) sent |= send_copy(x, e->client, in);
    if ((t->dont_propagate & mask) || (to_focus && t != w)) break;
  }
  return 0;
}

/* ---- server grabs, colormaps, keyboard ----------------------------------------------- */

static size_t grab_server(ctx *x, int grab) {
  REQ_LEN(1);
  if (grab) x->s->grab = (int16_t)x->ci;
  else if (x->s->grab == x->ci) x->s->grab = -1;
  return 0;
}

/* Pointer and keyboard grabs (x11_input.c). Pointer grabs are real: they redirect the
 * touchscreen's events. There is no keyboard, so keyboard grabs only record their owner
 * and key grabs can never activate (they are checked, not stored). */
static int cursor_ok(ctx *x, uint32_t id) {
  x11_resource *r = id ? x_find(x->s, id) : 0;
  return !id || (r && r->type == X11_RES_CURSOR);
}

static size_t grab_button(ctx *x) {  /* GrabButton, GrabPointer */
  int passive = x->major == 28;
  REQ_LEN(6);
  uint32_t win = U32(4), confine = U32(12), cursor = U32(16);
  uint16_t mask = U16(8);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));               /* owner-events */
  if (mask & 0x8003u) return error(x, X11_BAD_VALUE, mask);           /* pointer events only */
  if (U8(10) > 1 || U8(11) > 1) return error(x, X11_BAD_VALUE, U8(10) > 1 ? U8(10) : U8(11));
  x11_window *w = find_window(x, win), *cw = confine ? find_window(x, confine) : 0;
  if (!w) return error(x, X11_BAD_WINDOW, win);
  if (confine && !cw) return error(x, X11_BAD_WINDOW, confine);
  if (!cursor_ok(x, cursor)) return error(x, X11_BAD_CURSOR, cursor);
  if (passive) {
    if (U16(22) & 0x7F00u) return error(x, X11_BAD_VALUE, U16(22));  /* modifiers or AnyModifier */
    if (U8(20) > 5) return error(x, X11_BAD_VALUE, U8(20));           /* AnyButton, 1..5 */
    x11_button_grab g = {win, confine, cursor, U16(22), mask, U8(20), 0, U8(1), U8(10), U8(11)};
    int err = x_grab_button(x->s, x->ci, &g);
    return err ? error(x, (uint8_t)err, win) : 0;
  }
  int status = x_grab_pointer(x->s, x->ci, w, U8(1), mask, U8(10), cw, cursor, U32(20));
  reply(x, (uint8_t)status, 0);
  return 32;
}

static size_t grab_key(ctx *x) {  /* GrabKey, GrabKeyboard */
  int passive = x->major == 33;
  REQ_LEN(4);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));
  x11_window *w = find_window(x, U32(4));
  if (!w) return error(x, X11_BAD_WINDOW, U32(4));
  if (passive) {
    uint16_t mods = U16(8);
    uint8_t key = U8(10);
    if (mods & 0x7F00u) return error(x, X11_BAD_VALUE, mods);
    if (key && key < 8) return error(x, X11_BAD_VALUE, key);  /* AnyKey or 8..255 */
    if (U8(11) > 1 || U8(12) > 1) return error(x, X11_BAD_VALUE, U8(11) > 1 ? U8(11) : U8(12));
    return 0;
  }
  if (U8(12) > 1 || U8(13) > 1) return error(x, X11_BAD_VALUE, U8(12) > 1 ? U8(12) : U8(13));
  reply(x, (uint8_t)x_grab_keyboard(x->s, x->ci, w, U32(8)), 0);
  return 32;
}

static size_t ungrab_passive(ctx *x) {  /* UngrabButton, UngrabKey */
  REQ_LEN(3);
  if (!find_window(x, U32(4))) return error(x, X11_BAD_WINDOW, U32(4));
  if (U16(8) & 0x7F00u) return error(x, X11_BAD_VALUE, U16(8));
  if (x->major == 34 && U8(1) && U8(1) < 8) return error(x, X11_BAD_VALUE, U8(1));
  if (x->major == 29) {
    if (U8(1) > 5) return error(x, X11_BAD_VALUE, U8(1));
    x_ungrab_button(x->s, x->ci, U32(4), U8(1), U16(8));
  }
  return 0;
}

static size_t ungrab_active(ctx *x) {  /* UngrabPointer, UngrabKeyboard */
  REQ_LEN(2);
  if (x->major == 27) x_ungrab_pointer(x->s, x->ci, U32(4));
  else x_ungrab_keyboard(x->s, x->ci, U32(4));
  return 0;
}

static size_t change_active_pointer_grab(ctx *x) {
  REQ_LEN(4);
  if (!cursor_ok(x, U32(4))) return error(x, X11_BAD_CURSOR, U32(4));
  if (U16(12) & 0x8003u) return error(x, X11_BAD_VALUE, U16(12));
  x_change_active_grab(x->s, x->ci, U16(12), U32(4), U32(8));
  return 0;
}

static size_t allow_events(ctx *x) {
  REQ_LEN(2);
  if (U8(1) > 7) return error(x, X11_BAD_VALUE, U8(1));
  x_allow_events(x->s, x->ci, U8(1), U32(4));
  return 0;
}

static size_t warp_pointer(ctx *x) {
  REQ_LEN(6);
  uint32_t src = U32(4), dst = U32(8);
  x11_window *sw = src ? find_window(x, src) : 0, *dw = dst ? find_window(x, dst) : 0;
  if (src && !sw) return error(x, X11_BAD_WINDOW, src);
  if (dst && !dw) return error(x, X11_BAD_WINDOW, dst);
  x11_server *s = x->s;
  if (sw) {  /* only if the pointer is in the source rectangle of a viewable source window */
    int ax, ay, sx = I16(12), sy = I16(14), sww = U16(16), sh = U16(18);
    xw_abs(sw, &ax, &ay);
    if (!sww) sww = sw->w - sx;
    if (!sh) sh = sw->h - sy;
    int px = s->ptr_x - ax, py = s->ptr_y - ay;
    if (!xw_viewable(sw) || px < sx || py < sy || px >= sx + sww || py >= sy + sh) return 0;
  }
  int nx = s->ptr_x + I16(20), ny = s->ptr_y + I16(22);
  if (dw) {
    if (!xw_viewable(dw)) return 0;
    int ax, ay;
    xw_abs(dw, &ax, &ay);
    nx = ax + I16(20), ny = ay + I16(22);
  }
  x_warp_pointer(s, nx, ny);
  return 0;
}

static size_t get_motion_events(ctx *x) {  /* motion-buffer-size is 0: never any history */
  REQ_LEN(4);
  if (!find_window(x, U32(4))) return error(x, X11_BAD_WINDOW, U32(4));
  x_input_reset_hint(x->s, x->ci);
  reply(x, 0, 0);
  return 32;
}

/* SetCloseDownMode: what happens to the client's resources when its connection closes
 * (x11_client_close): Destroy, RetainPermanent or RetainTemporary. */
static size_t set_close_down_mode(ctx *x) {
  REQ_LEN(1);
  if (U8(1) > 2) return error(x, X11_BAD_VALUE, U8(1));
  x->c->close_mode = U8(1);
  return 0;
}

/* KillClient: a connected client is closed by the transport (x11_take_kill), in its own
 * close-down mode, so its resources may be retained; a retained client's resources are
 * destroyed now. AllTemporary (0) destroys every client retained in RetainTemporary mode.
 * As X.Org: a resource of the requesting client itself is allowed. */
static size_t kill_client(ctx *x) {
  REQ_LEN(2);
  uint32_t id = U32(4);
  x11_server *s = x->s;
  if (id == 0) {
    for (int c = 0; c < X11_SERVER_CLIENTS; ++c)
      if (s->clients[c].retained == 2) retained_destroy(s, c);
    return 0;
  }
  x11_resource *r = x_find(s, id);
  if (!r || r->owner >= X11_SERVER_CLIENTS) return error(x, X11_BAD_VALUE, id);
  if (s->clients[r->owner].retained) retained_destroy(s, r->owner);
  else x_bit_set(s->kills, r->owner);
  return 0;
}

/* CreateColormap: only the screen's one visual (TrueColor RGB565) exists, so a new colormap
 * is another name for the same read-only mapping (Qt's xcb plugin creates one for its
 * top-level windows). AllocAll is not allowed for a static class: BadMatch. */
static size_t create_colormap(ctx *x) {
  REQ_LEN(4);
  uint8_t alloc = U8(1);
  uint32_t mid = U32(4), visual = U32(12);
  if (alloc > 1) return error(x, X11_BAD_VALUE, alloc);
  if (!new_id_ok(x, mid)) return error(x, X11_BAD_IDCHOICE, mid);
  if (!find_window(x, U32(8))) return error(x, X11_BAD_WINDOW, U32(8));
  if (visual != X11_ID_VISUAL_TRUECOLOR16) return error(x, X11_BAD_MATCH, visual);
  if (alloc == 1) return error(x, X11_BAD_MATCH, alloc);  /* AllocAll on TrueColor */
  if (!x_add(x->s, mid, x->ci, X11_RES_COLORMAP, 0)) return error(x, X11_BAD_ALLOC, 0);
  return 0;
}

static size_t free_colormap(ctx *x) {  /* the default colormap is never freed */
  REQ_LEN(2);
  uint32_t id = U32(4);
  if (id == X11_ID_DEFAULT_COLORMAP) return 0;
  x11_resource *r = x_find(x->s, id);
  if (!r || r->type != X11_RES_COLORMAP) return error(x, X11_BAD_COLORMAP, id);
  free_resource(x->s, r);
  return 0;
}

static size_t install_colormap(ctx *x) {  /* Install, Uninstall: the one colormap stays installed */
  REQ_LEN(2);
  if (!cmap_ok(x, U32(4))) return error(x, X11_BAD_COLORMAP, U32(4));
  return 0;
}

static size_t list_installed_colormaps(ctx *x) {
  REQ_LEN(2);
  if (!find_window(x, U32(4))) return error(x, X11_BAD_WINDOW, U32(4));
  uint8_t *r = reply(x, 0, 4);
  x_wr16(r + 8, 1, x->msb);
  x_wr32(r + 32, X11_ID_DEFAULT_COLORMAP, x->msb);
  return 36;
}

/* ---- keyboard map and modifier map ---------------------------------------------------- */

/* There is no keyboard: no key events, no key is ever down. The core keyboard map and modifier
 * map are still client-visible state (the protocol: the keyboard map "is merely stored for
 * reading and writing by clients"), so ChangeKeyboardMapping and SetModifierMapping store
 * them, the Get requests report them and every client gets MappingNotify. Keycodes 8..255
 * (the setup's range). */

#define X11_KEYSYMS_MAX 8  /* keysyms per keycode stored; more is BadAlloc (X.Org's core maps use 7) */

/* MappingNotify to every client: request 0 Modifier, 1 Keyboard, 2 Pointer. */
static void mapping_notify(x11_server *s, uint8_t request, uint8_t first, uint8_t count) {
  for (int c = 0; c < X11_SERVER_CLIENTS; ++c) {
    if (!s->clients[c].active) continue;
    uint8_t ev[32] = {34};
    ev[4] = request, ev[5] = first, ev[6] = count;
    x_send_event(s, c, ev);
  }
}

/* The default core keymap: a US layout in X.Org's keycode numbering (evdev code + 8), two keysyms
 * per keycode (unshifted, shifted), as Xvfb reports it. There is no keyboard and no key event is
 * ever generated; the map describes keycodes for clients that look keysyms up. Without it,
 * XKeysymToKeycode fails for every keysym: mwm then rejects its window-menu accelerators
 * (Alt<Key>F5 ...) and posts an empty menu, Motif cannot set mnemonics. Keycodes 8..135 (the rest
 * NoSymbol); every keysym used fits in 16 bits. 512 bytes, const (flash on the panel). */
#define KB_DEFAULT_FIRST 8
#define KB_DEFAULT_COUNT 128
static const uint16_t kb_default[KB_DEFAULT_COUNT][2] = {
    [9 - 8] = {0xff1b, 0},                                                     /* Escape */
    [10 - 8] = {'1', '!'}, [11 - 8] = {'2', '@'}, [12 - 8] = {'3', '#'}, [13 - 8] = {'4', '$'}, [14 - 8] = {'5', '%'},
    [15 - 8] = {'6', '^'}, [16 - 8] = {'7', '&'}, [17 - 8] = {'8', '*'}, [18 - 8] = {'9', '('}, [19 - 8] = {'0', ')'},
    [20 - 8] = {'-', '_'}, [21 - 8] = {'=', '+'}, [22 - 8] = {0xff08, 0},       /* BackSpace */
    [23 - 8] = {0xff09, 0xfe20},                                               /* Tab, ISO_Left_Tab */
    [24 - 8] = {'q', 'Q'}, [25 - 8] = {'w', 'W'}, [26 - 8] = {'e', 'E'}, [27 - 8] = {'r', 'R'}, [28 - 8] = {'t', 'T'},
    [29 - 8] = {'y', 'Y'}, [30 - 8] = {'u', 'U'}, [31 - 8] = {'i', 'I'}, [32 - 8] = {'o', 'O'}, [33 - 8] = {'p', 'P'},
    [34 - 8] = {'[', '{'}, [35 - 8] = {']', '}'}, [36 - 8] = {0xff0d, 0},       /* Return */
    [37 - 8] = {0xffe3, 0},                                                    /* Control_L */
    [38 - 8] = {'a', 'A'}, [39 - 8] = {'s', 'S'}, [40 - 8] = {'d', 'D'}, [41 - 8] = {'f', 'F'}, [42 - 8] = {'g', 'G'},
    [43 - 8] = {'h', 'H'}, [44 - 8] = {'j', 'J'}, [45 - 8] = {'k', 'K'}, [46 - 8] = {'l', 'L'},
    [47 - 8] = {';', ':'}, [48 - 8] = {'\'', '"'}, [49 - 8] = {'`', '~'}, [50 - 8] = {0xffe1, 0},  /* Shift_L */
    [51 - 8] = {'\\', '|'},
    [52 - 8] = {'z', 'Z'}, [53 - 8] = {'x', 'X'}, [54 - 8] = {'c', 'C'}, [55 - 8] = {'v', 'V'}, [56 - 8] = {'b', 'B'},
    [57 - 8] = {'n', 'N'}, [58 - 8] = {'m', 'M'}, [59 - 8] = {',', '<'}, [60 - 8] = {'.', '>'}, [61 - 8] = {'/', '?'},
    [62 - 8] = {0xffe2, 0},                                                    /* Shift_R */
    [63 - 8] = {0xffaa, 0},                                                    /* KP_Multiply */
    [64 - 8] = {0xffe9, 0xffe7},                                               /* Alt_L, Meta_L */
    [65 - 8] = {' ', 0}, [66 - 8] = {0xffe5, 0},                                /* space, Caps_Lock */
    [67 - 8] = {0xffbe, 0}, [68 - 8] = {0xffbf, 0}, [69 - 8] = {0xffc0, 0}, [70 - 8] = {0xffc1, 0},  /* F1..F4 */
    [71 - 8] = {0xffc2, 0}, [72 - 8] = {0xffc3, 0}, [73 - 8] = {0xffc4, 0}, [74 - 8] = {0xffc5, 0},  /* F5..F8 */
    [75 - 8] = {0xffc6, 0}, [76 - 8] = {0xffc7, 0},                            /* F9, F10 */
    [77 - 8] = {0xff7f, 0}, [78 - 8] = {0xff14, 0},                            /* Num_Lock, Scroll_Lock */
    [79 - 8] = {0xff95, 0xffb7}, [80 - 8] = {0xff97, 0xffb8}, [81 - 8] = {0xff9a, 0xffb9},  /* keypad 7 8 9 */
    [82 - 8] = {0xffad, 0},                                                    /* KP_Subtract */
    [83 - 8] = {0xff96, 0xffb4}, [84 - 8] = {0xff9d, 0xffb5}, [85 - 8] = {0xff98, 0xffb6},  /* keypad 4 5 6 */
    [86 - 8] = {0xffab, 0},                                                    /* KP_Add */
    [87 - 8] = {0xff9c, 0xffb1}, [88 - 8] = {0xff99, 0xffb2}, [89 - 8] = {0xff9b, 0xffb3},  /* keypad 1 2 3 */
    [90 - 8] = {0xff9e, 0xffb0}, [91 - 8] = {0xff9f, 0xffae},                  /* keypad 0, . */
    [94 - 8] = {'<', '>'}, [95 - 8] = {0xffc8, 0}, [96 - 8] = {0xffc9, 0},     /* ISO key, F11, F12 */
    [104 - 8] = {0xff8d, 0}, [105 - 8] = {0xffe4, 0}, [106 - 8] = {0xffaf, 0},  /* KP_Enter, Control_R, KP_Divide */
    [107 - 8] = {0xff61, 0}, [108 - 8] = {0xffea, 0xffe8},                      /* Print, Alt_R Meta_R */
    [110 - 8] = {0xff50, 0}, [111 - 8] = {0xff52, 0}, [112 - 8] = {0xff55, 0},  /* Home, Up, Prior */
    [113 - 8] = {0xff51, 0}, [114 - 8] = {0xff53, 0}, [115 - 8] = {0xff57, 0},  /* Left, Right, End */
    [116 - 8] = {0xff54, 0}, [117 - 8] = {0xff56, 0}, [118 - 8] = {0xff63, 0},  /* Down, Next, Insert */
    [119 - 8] = {0xffff, 0}, [127 - 8] = {0xff13, 0},                          /* Delete, Pause */
    [133 - 8] = {0xffeb, 0}, [134 - 8] = {0xffec, 0}, [135 - 8] = {0xff67, 0},  /* Super_L, Super_R, Menu */
};

/* The default keysym `col` (0 or 1) of keycode kc. */
static uint32_t kb_default_sym(unsigned kc, unsigned col) {
  if (col > 1 || kc < KB_DEFAULT_FIRST || kc >= KB_DEFAULT_FIRST + KB_DEFAULT_COUNT) return 0;
  return kb_default[kc - KB_DEFAULT_FIRST][col];
}

/* ChangeKeyboardMapping, in X.Org's order: BadLength unless the list holds count x
 * keysyms-per-keycode keysyms; BadValue (the first keycode) below 8; BadValue
 * (keysyms-per-keycode) past 255 or for 0 keysyms per keycode. No keycodes: nothing changes and
 * no MappingNotify (as X.Org). The table widens to the widest change, NoSymbol-padded; it is
 * allocated on the first change, holding the default keymap (BadAlloc when that fails, nothing
 * changed). */
static size_t change_keyboard_mapping(ctx *x) {
  REQ_MIN(8);
  unsigned count = U8(1), first = U8(4), per = U8(5);
  if ((x->len - 8) / 4 != (size_t)count * per) return error(x, X11_BAD_LENGTH, 0);
  if (first < 8) return error(x, X11_BAD_VALUE, first);
  if (first + count - 1 > 255 || per == 0) return error(x, X11_BAD_VALUE, per);
  if (!count) return 0;
  x11_server *s = x->s;
  if (per > X11_KEYSYMS_MAX) return error(x, X11_BAD_ALLOC, 0);
  unsigned have = s->kb_map ? s->kb_width : 2;  /* no table yet: the default keymap, 2 wide */
  unsigned width = have > per ? have : per;
  if (!s->kb_map || width > s->kb_width) {
    uint32_t *m = (uint32_t *)x_alloc(s, sizeof(uint32_t) * 248 * width);
    if (!m) return error(x, X11_BAD_ALLOC, 0);
    memset(m, 0, sizeof(uint32_t) * 248 * width);
    for (int k = 0; k < 248; ++k)
      for (unsigned j = 0; j < have; ++j)
        m[k * width + j] = s->kb_map ? s->kb_map[k * s->kb_width + j] : kb_default_sym(8u + (unsigned)k, j);
    x_free(s, s->kb_map);
    s->kb_map = m, s->kb_width = (uint8_t)width;
  }
  for (unsigned k = 0; k < count; ++k) {
    uint32_t *row = s->kb_map + (first - 8 + k) * width;
    for (unsigned j = 0; j < width; ++j) row[j] = j < per ? U32(8 + 4 * (k * per + j)) : 0;
  }
  mapping_notify(s, 1, (uint8_t)first, (uint8_t)count);
  return 0;
}

static size_t get_keyboard_mapping(ctx *x) {
  REQ_LEN(2);
  unsigned first = U8(4), count = U8(5);
  if (first < 8 || first + count - 1 > 255 || !count)  /* the setup range: 8..255 */
    return error(x, X11_BAD_VALUE, first < 8 ? first : count);
  const x11_server *s = x->s;
  unsigned width = s->kb_map ? s->kb_width : 2;  /* no client changed it: the default keymap */
  uint8_t *r = reply(x, (uint8_t)width, 4 * (size_t)count * width);
  for (unsigned k = 0; k < count; ++k)
    for (unsigned j = 0; j < width; ++j)
      x_wr32(r + 32 + 4 * (k * width + j),
             s->kb_map ? s->kb_map[(first - 8 + k) * width + j] : kb_default_sym(first + k, j), x->msb);
  return 32 + 4 * (size_t)count * width;
}

/* SetModifierMapping, in X.Org's order (dix/devices.c, inpututils.c): BadLength unless
 * 8 x keycodes-per-modifier keycodes follow; BadValue for a keycode that appears twice (in one
 * modifier or in two: X.Org's conflict rule; X.Org leaves a stale bad value, this one names the
 * keycode), then for one below 8. Busy would be a modifier key (old or new) being down: no key
 * ever is. Failed: no restrictions here (X.Org never answers it either). Success: stored, then
 * MappingNotify (Modifier). */
static size_t set_modifier_mapping(ctx *x) {
  REQ_MIN(4);
  size_t per = U8(1);
  if (x->len != 4 + 8 * per) return error(x, X11_BAD_LENGTH, 0);
  const uint8_t *k = x->q + 4;
  uint8_t seen[32] = {0};
  for (size_t i = 0; i < 8 * per; ++i) {  /* X.Org: the duplicate check over the whole list first */
    unsigned kc = k[i];
    if (!kc) continue;
    if (seen[kc / 8] >> (kc % 8) & 1) return error(x, X11_BAD_VALUE, kc);
    seen[kc / 8] |= (uint8_t)(1u << (kc % 8));
  }
  for (int kc = 1; kc < 8; ++kc)  /* then the range (8..255) */
    if (seen[0] >> kc & 1) return error(x, X11_BAD_VALUE, (uint32_t)kc);
  x11_server *s = x->s;
  memset(s->mod_of, 0, sizeof(s->mod_of));
  for (size_t i = 0; i < 8 * per; ++i)
    if (k[i]) s->mod_of[k[i]] = (uint8_t)(1 + i / per);
  reply(x, 0, 0);  /* status Success */
  mapping_notify(s, 0, 0, 0);
  return 32;
}

/* GetModifierMapping: per modifier its keycodes in ascending order, keycodes-per-modifier the
 * largest set (as X.Org rebuilds it), at least 1: an empty map keeps the panel's long-standing
 * answer of one zero keycode per modifier (X.Org answers 0 per modifier; both are valid). */
static size_t get_modifier_mapping(ctx *x) {
  REQ_LEN(1);
  const x11_server *s = x->s;
  unsigned n[8] = {0}, per = 1;
  for (int kc = 8; kc < 256; ++kc)
    if (s->mod_of[kc] && ++n[s->mod_of[kc] - 1] > per) per = n[s->mod_of[kc] - 1];
  uint8_t *r = reply(x, (uint8_t)per, 8 * (size_t)per);
  unsigned at[8] = {0};
  for (int kc = 8; kc < 256; ++kc)
    if (s->mod_of[kc]) {
      unsigned m = s->mod_of[kc] - 1u;
      r[32 + m * per + at[m]++] = (uint8_t)kc;
    }
  return 32 + 8 * (size_t)per;
}

/* ---- access control --------------------------------------------------------------------- */

/* Connections are authorised by MIT-MAGIC-COOKIE-1 (cfg.cookie), or, in the host's loopback
 * test mode without a cookie, not at all. There is no host list. ListHosts reports none:
 * Enabled when the cookie is required (the empty list admits no host by itself), Disabled when
 * any client may connect. No client is granted permission to change host-based access, so
 * ChangeHosts and SetAccessControl answer BadAccess after the checks that come first in X.Org
 * (length, mode), as X.Org answers a client without that permission. A client can never relax
 * the cookie check. */
static size_t list_hosts(ctx *x) {
  REQ_LEN(1);
  reply(x, x->s->cfg.cookie ? 1 : 0, 0);  /* mode; no hosts */
  return 32;
}

static size_t change_hosts(ctx *x) {
  REQ_MIN(8);
  if (x->len != 8 + x_pad4(U16(6))) return error(x, X11_BAD_LENGTH, 0);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));  /* Insert, Delete */
  return error(x, X11_BAD_ACCESS, 0);
}

static size_t set_access_control(ctx *x) {
  REQ_LEN(1);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));  /* Disable, Enable */
  return error(x, X11_BAD_ACCESS, 0);
}

/* ---- screen saver --------------------------------------------------------------------- */

/* SetScreenSaver: the values are checked and kept for GetScreenSaver; -1 and Default restore
 * the defaults (timeout 0: disabled; blanking and exposures preferred). */
static size_t set_screen_saver(ctx *x) {
  REQ_LEN(3);
  int16_t timeout = I16(4), interval = I16(6);
  uint8_t blank = U8(8), expose = U8(9);
  if (timeout < -1) return error(x, X11_BAD_VALUE, (uint32_t)(int32_t)timeout);
  if (interval < -1) return error(x, X11_BAD_VALUE, (uint32_t)(int32_t)interval);
  if (blank > 2) return error(x, X11_BAD_VALUE, blank);
  if (expose > 2) return error(x, X11_BAD_VALUE, expose);
  x->s->ss_timeout = timeout == -1 ? 0 : timeout;
  x->s->ss_interval = interval == -1 ? 0 : interval;
  x->s->ss_blanking = blank == 2 ? 1 : blank;
  x->s->ss_exposures = expose == 2 ? 1 : expose;
  return 0;
}

static size_t get_screen_saver(ctx *x) {
  REQ_LEN(1);
  uint8_t *r = reply(x, 0, 0);
  x_wr16(r + 8, (uint16_t)x->s->ss_timeout, x->msb);
  x_wr16(r + 10, (uint16_t)x->s->ss_interval, x->msb);
  r[12] = x->s->ss_blanking;
  r[13] = x->s->ss_exposures;
  return 32;
}

/* ForceScreenSaver: Reset restarts the idle timer and Activate starts the saver. This server
 * has no screen saver, so neither changes what is shown; the mode is still checked. */
static size_t force_screen_saver(ctx *x) {
  REQ_LEN(1);
  if (U8(1) > 1) return error(x, X11_BAD_VALUE, U8(1));
  return 0;
}

/* ---- keyboard, pointer and font-path controls ------------------------------------------ */

/* QueryKeymap: no keyboard is attached, so no key is ever down. */
static size_t query_keymap(ctx *x) {
  REQ_LEN(1);
  reply(x, 0, 8);
  return 40;
}

/* Bell: the panel has no bell; the volume is still checked. */
static size_t bell(ctx *x) {
  REQ_LEN(1);
  int8_t percent = (int8_t)U8(1);
  if (percent < -100 || percent > 100) return error(x, X11_BAD_VALUE, (uint32_t)(int32_t)percent);
  return 0;
}

/* ChangeKeyboardControl: checked as a whole, then applied (all or nothing). */
static size_t change_keyboard_control(ctx *x) {
  REQ_MIN(8);
  uint32_t mask = U32(4);
  if (x->len != 8 + 4 * (size_t)popcount(mask)) return error(x, X11_BAD_LENGTH, 0);
  if (mask & ~0xFFu) return error(x, X11_BAD_VALUE, mask);
  int32_t v[8] = {0};
  const uint8_t *p = x->q + 8;
  for (int bit = 0; bit < 8; ++bit)
    if (mask & (1u << bit)) {
      uint32_t raw = x_rd32(p, x->msb);
      p += 4;
      /* INT8 and INT16 values are sign-extended from their low bytes, CARD8 ones are not */
      v[bit] = bit <= 1 ? (int8_t)raw : bit <= 3 ? (int16_t)raw : (int32_t)(raw & 0xFF);
      if (bit <= 1 && (v[bit] < -1 || v[bit] > 100)) return error(x, X11_BAD_VALUE, raw);
      if ((bit == 2 || bit == 3) && v[bit] < -1) return error(x, X11_BAD_VALUE, raw);
      if (bit == 4 && (v[bit] < 1 || v[bit] > 32)) return error(x, X11_BAD_VALUE, raw);
      if (bit == 5 && v[bit] > 1) return error(x, X11_BAD_VALUE, raw);
      if (bit == 6 && v[bit] < 8) return error(x, X11_BAD_VALUE, raw);  /* the setup's keycode range */
      if (bit == 7 && v[bit] > 2) return error(x, X11_BAD_VALUE, raw);
    }
  if ((mask & 0x10) && !(mask & 0x20)) return error(x, X11_BAD_MATCH, 0);  /* led without led-mode */
  if ((mask & 0x40) && !(mask & 0x80)) return error(x, X11_BAD_MATCH, 0);  /* key without auto-repeat-mode */
  x11_server *s = x->s;
  if (mask & 0x01) s->kb_click = (uint8_t)(v[0] < 0 ? 0 : v[0]);
  if (mask & 0x02) s->kb_bell = (uint8_t)(v[1] < 0 ? 50 : v[1]);
  if (mask & 0x04) s->kb_pitch = (int16_t)(v[2] < 0 ? 400 : v[2]);
  if (mask & 0x08) s->kb_duration = (int16_t)(v[3] < 0 ? 100 : v[3]);
  if (mask & 0x20) {
    uint32_t leds = mask & 0x10 ? 1u << (v[4] - 1) : 0xFFFFFFFFu;
    s->kb_leds = v[5] ? s->kb_leds | leds : s->kb_leds & ~leds;
  }
  if (mask & 0x80) {
    int on = v[7] == 2 ? 1 : v[7];  /* Default: on */
    if (mask & 0x40) {
      uint8_t bit = (uint8_t)(1u << (v[6] & 7));
      s->kb_repeats[v[6] >> 3] = on ? s->kb_repeats[v[6] >> 3] | bit : s->kb_repeats[v[6] >> 3] & ~bit;
    } else {
      s->kb_auto_repeat = (uint8_t)on;
    }
  }
  return 0;
}

static size_t get_keyboard_control(ctx *x) {
  REQ_LEN(1);
  x11_server *s = x->s;
  uint8_t *r = reply(x, s->kb_auto_repeat, 20);
  x_wr32(r + 8, s->kb_leds, x->msb);
  r[12] = s->kb_click;
  r[13] = s->kb_bell;
  x_wr16(r + 14, (uint16_t)s->kb_pitch, x->msb);
  x_wr16(r + 16, (uint16_t)s->kb_duration, x->msb);
  memcpy(r + 20, s->kb_repeats, 32);
  return 52;
}

/* ChangePointerControl: kept and reported; touch input is absolute, so it changes no motion. */
static size_t change_pointer_control(ctx *x) {
  REQ_LEN(3);
  int16_t num = I16(4), den = I16(6), thr = I16(8);
  uint8_t do_accel = U8(10), do_thr = U8(11);
  if (do_accel > 1) return error(x, X11_BAD_VALUE, do_accel);
  if (do_thr > 1) return error(x, X11_BAD_VALUE, do_thr);
  if (do_accel) {
    if (num < -1) return error(x, X11_BAD_VALUE, (uint32_t)(int32_t)num);
    if (den < -1 || den == 0) return error(x, X11_BAD_VALUE, (uint32_t)(int32_t)den);
  }
  if (do_thr && thr < -1) return error(x, X11_BAD_VALUE, (uint32_t)(int32_t)thr);
  if (do_accel) {
    x->s->ptr_accel_num = num == -1 ? 2 : num;
    x->s->ptr_accel_den = den == -1 ? 1 : den;
  }
  if (do_thr) x->s->ptr_threshold = thr == -1 ? 4 : thr;
  return 0;
}

static size_t get_pointer_control(ctx *x) {
  REQ_LEN(1);
  uint8_t *r = reply(x, 0, 0);
  x_wr16(r + 8, (uint16_t)x->s->ptr_accel_num, x->msb);
  x_wr16(r + 10, (uint16_t)x->s->ptr_accel_den, x->msb);
  x_wr16(r + 12, (uint16_t)x->s->ptr_threshold, x->msb);
  return 32;
}

/* GetPointerMapping: five physical buttons (the touch is Button1; gesture clicks 3, 4, 5) and
 * their logical numbers (SetPointerMapping). */
static size_t get_pointer_mapping(ctx *x) {
  REQ_LEN(1);
  uint8_t *r = reply(x, 5, 8);
  memcpy(r + 32, x->s->ptr_map, 5);
  return 40;
}

/* SetPointerMapping: one element per physical button (5; else BadValue), 0 disables a button,
 * a nonzero logical number may appear once (else BadValue). Busy, and nothing changes, if a
 * button whose mapping would change is down. On Success every client gets MappingNotify
 * (request Pointer). The map applies where physical buttons become events (x11_input.c). */
static size_t set_pointer_mapping(ctx *x) {
  REQ_MIN(4);
  size_t n = U8(1);
  if (x->len != 4 + x_pad4(n)) return error(x, X11_BAD_LENGTH, 0);
  if (n != 5) return error(x, X11_BAD_VALUE, (uint32_t)n);
  const uint8_t *m = x->q + 4;
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < i; ++j)
      if (m[i] && m[i] == m[j]) return error(x, X11_BAD_VALUE, m[i]);
  int busy = 0;
  for (int i = 0; i < 5; ++i)
    if (m[i] != x->s->ptr_map[i] && (x->s->in_buttons >> i & 1)) busy = 1;
  reply(x, (uint8_t)busy, 0);  /* status: Success 0, Busy 1 */
  if (busy) return 32;
  memcpy(x->s->ptr_map, m, 5);
  mapping_notify(x->s, 2, 0, 0);
  return 32;
}

/* The fonts are compiled in: the font path is the one element "built-ins" (X.Org's name for
 * its compiled-in fonts). SetFontPath accepts only that path, or the empty list (the default);
 * any other directory is BadValue, since no font could be read from it. */
static const char FONT_PATH[] = "built-ins";

static size_t get_font_path(ctx *x) {
  REQ_LEN(1);
  size_t n = sizeof(FONT_PATH) - 1, data = (1 + n + 3) & ~(size_t)3;
  uint8_t *r = reply(x, 0, data);
  x_wr16(r + 8, 1, x->msb);
  r[32] = (uint8_t)n;
  memcpy(r + 33, FONT_PATH, n);
  return 32 + data;
}

static size_t set_font_path(ctx *x) {
  REQ_MIN(8);
  unsigned count = U16(4);
  size_t off = 8;
  for (unsigned i = 0; i < count; ++i) {
    if (off >= x->len) return error(x, X11_BAD_LENGTH, 0);
    size_t n = x->q[off];
    if (off + 1 + n > x->len) return error(x, X11_BAD_LENGTH, 0);
    off += 1 + n;
  }
  if (((off + 3) & ~(size_t)3) != x->len) return error(x, X11_BAD_LENGTH, 0);
  for (unsigned i = 0, o = 8; i < count; ++i) {
    size_t n = x->q[o];
    if (n != sizeof(FONT_PATH) - 1 || memcmp(x->q + o + 1, FONT_PATH, n)) return error(x, X11_BAD_VALUE, 0);
    o += 1 + (unsigned)n;
  }
  return 0;
}

/* ---- GetImage ------------------------------------------------------------------------ */

static size_t get_image(ctx *x) {
  REQ_LEN(5);
  uint8_t format = U8(1);
  int gx = I16(8), gy = I16(10), w = U16(12), h = U16(14);
  uint32_t planes = U32(16);
  if (format != 1 && format != 2) return error(x, X11_BAD_VALUE, format);
  x11_resource *res = x_find(x->s, U32(4));
  if (!res || (res->type != X11_RES_WINDOW && res->type != X11_RES_PIXMAP)) return error(x, X11_BAD_DRAWABLE, U32(4));
  const xr_surface *src;
  int ox, oy, depth;
  uint32_t visual = 0;
  if (res->type == X11_RES_WINDOW) {
    x11_window *win = (x11_window *)res->obj;
    if (win->input_only) return error(x, X11_BAD_MATCH, U32(4));
    xw_abs(win, &ox, &oy);
    /* viewable; inside the outer edges; fully on the screen */
    if (!xw_viewable(win) || gx < -win->bw || gy < -win->bw || gx + w > win->w + win->bw ||
        gy + h > win->h + win->bw || ox + gx < 0 || oy + gy < 0 || ox + gx + w > x->s->fb.w ||
        oy + gy + h > x->s->fb.h)
      return error(x, X11_BAD_MATCH, U32(4));
    src = &x->s->fb, depth = 16, visual = X11_ID_VISUAL_TRUECOLOR16;
  } else {
    const x11_pixmap *pm = (const x11_pixmap *)res->obj;
    if (gx < 0 || gy < 0 || gx + w > pm->surf.w || gy + h > pm->surf.h) return error(x, X11_BAD_MATCH, U32(4));
    src = &pm->surf, ox = oy = 0, depth = pm->surf.depth;
  }
  planes &= depth == 16 ? 0xFFFFu : 1u;
  size_t bitmap_row = x_pad4(((size_t)w + 7) / 8), n;
  int nplanes = popcount(planes);
  size_t row_bytes = format == 2 && depth == 16 ? x_pad4((size_t)w * 2) : bitmap_row;
  if (h && row_bytes > SIZE_MAX / (size_t)h) return error(x, X11_BAD_ALLOC, 0);
  n = row_bytes * (size_t)h;
  if (format == 1) {
    if (nplanes && n > SIZE_MAX / (size_t)nplanes) return error(x, X11_BAD_ALLOC, 0);
    n *= (size_t)nplanes;
  }
  if (32 + n > x->cap) {
    if (!x->stream || n > UINT32_MAX || (n & 3u)) return error(x, X11_BAD_ALLOC, 0);
    uint8_t *head = reply(x, (uint8_t)depth, 0);
    x_wr32(head + 4, (uint32_t)(n / 4), x->msb);
    x_wr32(head + 8, visual, x->msb);
    x->stream->src = src;
    x->stream->src_x = ox + gx;
    x->stream->src_y = oy + gy;
    x->stream->width = (uint16_t)w;
    x->stream->height = (uint16_t)h;
    x->stream->format = format;
    x->stream->depth = (uint8_t)depth;
    x->stream->planes = planes;
    x->stream->bitmap_row = bitmap_row;
    x->stream->row_bytes = format == 2 && depth == 16 ? x_pad4((size_t)w * 2) : bitmap_row;
    x->stream->total = n;
    x->stream->offset = 0;
    x->stream->active = n != 0;
    return 32;
  }
  uint8_t *r = reply(x, (uint8_t)depth, n);
  x_wr32(r + 8, visual, x->msb);
  uint8_t *d = r + 32;
  for (int yy = 0; yy < h; ++yy)
    for (int xx = 0; xx < w; ++xx) {
      int px = ox + gx + xx, py = oy + gy + yy;
      uint32_t v = depth == 16 ? ((const uint16_t *)src->pix)[py * src->stride + px]
                               : ((const uint8_t *)src->pix)[py * src->stride + px];
      v &= planes;
      if (format == 2 && depth == 16) {  /* image byte order LSBFirst */
        uint8_t *q = d + x_pad4((size_t)w * 2) * yy + 2 * xx;
        q[0] = (uint8_t)v, q[1] = (uint8_t)(v >> 8);
      } else if (format == 2) {          /* depth 1: bit order LSBFirst */
        if (v) d[bitmap_row * yy + xx / 8] |= (uint8_t)(1u << (xx & 7));
      } else {                           /* XYPixmap: one bitmap per plane, most significant first */
        int k = 0;
        for (int b = depth - 1; b >= 0; --b) {
          if (!(planes & (1u << b))) continue;
          if (v & (1u << b)) d[bitmap_row * ((size_t)k * h + yy) + xx / 8] |= (uint8_t)(1u << (xx & 7));
          ++k;
        }
      }
    }
  return 32 + n;
}

static size_t dispatch(ctx *x) {
  switch (x->major) {
    case 1: return create_window(x);
    case 2: return change_window_attributes(x);
    case 3: return get_window_attributes(x);
    case 4: return destroy_window(x);
    case 5: return destroy_subwindows(x);
    case 6: return change_save_set(x);
    case 7: return reparent_window(x);
    case 8: return map_window(x, 0);
    case 9: return map_window(x, 1);
    case 10: return unmap_window(x);
    case 11: return unmap_subwindows(x);
    case 12: return configure_window(x);
    case 13: return circulate_window(x);
    case 14: return get_geometry(x);
    case 15: return query_tree(x);
    case 16: return intern_atom(x);
    case 17: return get_atom_name(x);
    case 18: return change_property(x);
    case 19: return delete_property(x);
    case 20: return get_property(x);
    case 21: return list_properties(x);
    case 22: return set_selection_owner(x);
    case 23: return get_selection_owner(x);
    case 24: return convert_selection(x);
    case 25: return send_event(x);
    case 26: case 28: return grab_button(x);
    case 27: case 32: return ungrab_active(x);
    case 29: case 34: return ungrab_passive(x);
    case 30: return change_active_pointer_grab(x);
    case 31: case 33: return grab_key(x);
    case 35: return allow_events(x);
    case 36: return grab_server(x, 1);
    case 37: return grab_server(x, 0);
    case 38: return query_pointer(x);
    case 39: return get_motion_events(x);
    case 40: return translate_coordinates(x);
    case 41: return warp_pointer(x);
    case 42: return set_input_focus(x);
    case 43: return get_input_focus(x);
    case 44: return query_keymap(x);
    case 45: return open_font(x);
    case 46: return close_font(x);
    case 47: return query_font(x);
    case 48: return query_text_extents(x);
    case 49: return list_fonts(x);
    case 50: return list_fonts_with_info(x);
    case 51: return set_font_path(x);
    case 52: return get_font_path(x);
    case 53: return create_pixmap(x);
    case 54: return free_pixmap(x);
    case 55: return create_gc(x);
    case 56: return change_gc(x);
    case 57: return copy_gc(x);
    case 58: return set_dashes(x);
    case 59: return set_clip_rectangles(x);
    case 60: return free_gc(x);
    case 61: return clear_area(x);
    case 62: return copy_area(x, 0);
    case 63: return copy_area(x, 1);
    case 64: return poly_point(x);
    case 65: return poly_line(x);
    case 66: return poly_segment(x);
    case 67: return poly_rectangle(x);
    case 68: return poly_arc(x, 0);
    case 69: return fill_poly(x);
    case 70: return poly_fill_rectangle(x);
    case 71: return poly_arc(x, 1);
    case 72: return put_image(x);
    case 73: return get_image(x);
    case 74: return poly_text(x, 0);
    case 75: return poly_text(x, 1);
    case 76: return image_text(x, 0);
    case 77: return image_text(x, 1);
    case 78: return create_colormap(x);
    case 79: return free_colormap(x);
    case 80: return copy_colormap_and_free(x);
    case 81: case 82: return install_colormap(x);
    case 83: return list_installed_colormaps(x);
    case 84: return alloc_color(x);
    case 85: return named_color(x, 1);
    case 86: return alloc_color_cells(x, 0);
    case 87: return alloc_color_cells(x, 1);
    case 88: return free_colors(x);
    case 89: return store_colors(x);
    case 90: return store_named_color(x);
    case 91: return query_colors(x);
    case 92: return named_color(x, 0);
    case 93: return create_cursor(x, 0);
    case 94: return create_cursor(x, 1);
    case 95: return free_cursor(x);
    case 96: return recolor_cursor(x);
    case 97: return query_best_size(x);
    case 98: return query_extension(x);
    case 99: return list_extensions(x);
    case 101: return get_keyboard_mapping(x);
    case 116: return set_pointer_mapping(x);
    case 117: return get_pointer_mapping(x);
    case 119: return get_modifier_mapping(x);
    case 102: return change_keyboard_control(x);
    case 103: return get_keyboard_control(x);
    case 104: return bell(x);
    case 105: return change_pointer_control(x);
    case 106: return get_pointer_control(x);
    case 107: return set_screen_saver(x);
    case 108: return get_screen_saver(x);
    case 115: return force_screen_saver(x);
    case 100: return change_keyboard_mapping(x);
    case 109: return change_hosts(x);
    case 110: return list_hosts(x);
    case 111: return set_access_control(x);
    case 118: return set_modifier_mapping(x);
    case 112: return set_close_down_mode(x);
    case 113: return kill_client(x);
    case 114: return rotate_properties(x);
    case 127: return 0;  /* NoOperation */
    default:
      return error(x, X11_BAD_REQUEST, 0);  /* not a core request (all 119 are handled); no extensions */
  }
}

size_t x11_client_request_too_long(x11_server *s, int ci, const uint8_t hdr[4], uint8_t *out, size_t out_cap,
                                   size_t *skip) {
  x11_client *c = &s->clients[ci];
  size_t units = x_rd16(hdr + 2, c->msb);
  *skip = 0;
  if (units <= s->cfg.max_request_units || out_cap < 32) return 0;
  ctx x = {s, ci, c, c->msb, out, out_cap, hdr, 4, hdr[0], 0};
  ++c->seq;
  ++c->requests;
  *skip = units * 4;
  return error(&x, X11_BAD_LENGTH, 0);
}

static size_t client_request(x11_server *s, int ci, const uint8_t *q, size_t len, uint8_t *out, size_t out_cap,
                             int *close_after, x11_reply_stream *stream) {
  x11_client *c = &s->clients[ci];
  ctx x = {s, ci, c, c->msb, out, out_cap, q, len, q[0], stream};
  *close_after = 0;
  if (out_cap < X11_REPLY_MAX) return 0;
  if (stream) memset(stream, 0, sizeof(*stream));
  ++c->seq;
  ++c->requests;
  if (len == 4 && x_rd16(q + 2, c->msb) == 0)  /* length 0: BIG-REQUESTS is not offered, so too short */
    return error(&x, X11_BAD_LENGTH, 0);
  if (len < 4 || len != (size_t)x_rd16(q + 2, c->msb) * 4) {  /* invalid framing: answer, then close */
    *close_after = 1;
    return error(&x, X11_BAD_LENGTH, 0);
  }
  /* Region temporaries of this request come from the pool and are all returned here. If
   * the pool ran out, the request did less than asked (never more): BadAlloc, unless a reply
   * was already written. */
  int mark = xr_pool_mark();
  uint32_t failures = s->pool.failures;
  size_t n = dispatch(&x);
  xr_fill_use(0);  /* a drawing request's fill style ends with it */
  if (s->retained_empty) {  /* another client freed a retained client's last resource */
    s->retained_empty = 0;
    for (int k = 0; k < X11_SERVER_CLIENTS; ++k)
      if (s->clients[k].retained && !s->clients[k].resources) client_destroy(s, k);
  }
  x_input_update(s);  /* the window under the pointer may have changed; grabs may have ended */
  damage_commit(s);
  if (s->pool.failures != failures && n == 0) n = error(&x, X11_BAD_ALLOC, 0);
  xr_pool_release(mark);
  return n;
}

size_t x11_client_request(x11_server *s, int ci, const uint8_t *q, size_t len, uint8_t *out, size_t out_cap,
                          int *close_after) {
  return client_request(s, ci, q, len, out, out_cap, close_after, 0);
}

size_t x11_client_request_stream(x11_server *s, int ci, const uint8_t *q, size_t len, uint8_t *out,
                                 size_t out_cap, int *close_after, x11_reply_stream *stream) {
  if (!stream) return client_request(s, ci, q, len, out, out_cap, close_after, 0);
  return client_request(s, ci, q, len, out, out_cap, close_after, stream);
}

static uint32_t stream_pixel(const x11_reply_stream *s, int x, int y) {
  int px = s->src_x + x, py = s->src_y + y;
  if (s->depth == 16) return ((const uint16_t *)s->src->pix)[(size_t)py * s->src->stride + px];
  return ((const uint8_t *)s->src->pix)[(size_t)py * s->src->stride + px];
}

size_t x11_reply_stream_read(x11_reply_stream *s, uint8_t *out, size_t cap) {
  if (!s || !s->active || !out || !cap) return 0;
  size_t n = s->total - s->offset;
  if (n > cap) n = cap;
  memset(out, 0, n);
  for (size_t i = 0; i < n; ++i) {
    size_t at = s->offset + i;
    if (s->format == 2 && s->depth == 16) {
      size_t y = at / s->row_bytes, col = at % s->row_bytes;
      if (col < (size_t)s->width * 2) {
        uint32_t v = stream_pixel(s, (int)(col / 2), (int)y) & s->planes;
        out[i] = (uint8_t)(col & 1 ? v >> 8 : v);
      }
    } else if (s->format == 2) {
      size_t y = at / s->bitmap_row, col = at % s->bitmap_row;
      if (col < ((size_t)s->width + 7) / 8)
        for (int b = 0; b < 8; ++b) {
          size_t x = col * 8 + (size_t)b;
          if (x < s->width && (stream_pixel(s, (int)x, (int)y) & s->planes)) out[i] |= (uint8_t)(1u << b);
        }
    } else {
      size_t plane_bytes = s->bitmap_row * s->height;
      size_t plane_index = at / plane_bytes, within = at % plane_bytes;
      size_t y = within / s->bitmap_row, col = within % s->bitmap_row;
      int bit = -1, plane = 0;
      for (int b = s->depth - 1; b >= 0; --b) if (s->planes & (1u << b)) {
        if ((size_t)plane++ == plane_index) { bit = b; break; }
      }
      if (bit >= 0 && col < ((size_t)s->width + 7) / 8)
        for (int b = 0; b < 8; ++b) {
          size_t x = col * 8 + (size_t)b;
          if (x < s->width && (stream_pixel(s, (int)x, (int)y) & (1u << bit))) out[i] |= (uint8_t)(1u << b);
        }
    }
  }
  s->offset += n;
  if (s->offset == s->total) s->active = 0;
  return n;
}

/* The platform sets a root property (public API, see x11_server.h). At the end of the file and
 * only where it is used (host builds, ESP32 builds with the touch/gesture code), so a normal
 * ESP32 image stays byte-identical (its ELF hash includes the debug line table). */
#if !defined(ARDUINO) || defined(X11_TOUCH_DIAG)
int x11_root_property_set(x11_server *s, const char *name, const uint8_t *data, size_t n) {
  if (!s || !s->root || !name || n > X11_MAX_PROPERTY) return -1;
  if (!strcmp(name, X11_DIAG_ATOM)) return -1;  /* generated on demand, never stored */
  uint32_t atom = atom_intern(s, (const uint8_t *)name, strlen(name));
  if (!atom) return -1;
  x11_window *w = s->root;
  uint8_t *d = (uint8_t *)x_alloc(s, n + 1);
  if (!d) return -1;
  if (n) memcpy(d, data, n);
  x11_prop *p = prop_find(w, atom);
  if (!p) {
    p = (x11_prop *)x_alloc(s, sizeof(x11_prop));
    if (!p) { x_free(s, d); return -1; }
    memset(p, 0, sizeof(*p));
    p->name = atom;
    p->next = w->props;
    w->props = p;
  } else {
    x_free(s, p->data);
  }
  p->type = 31, p->format = 8, p->data = d, p->nbytes = (uint32_t)n;  /* 31: STRING */
  XW_FOR_SEL(w, e) {  /* PropertyNotify, NewValue, as change_property sends it */
    int c = e->client;
    if (!s->clients[c].active || !(e->mask & X_EV_PROPERTY)) continue;
    int msb = s->clients[c].msb;
    uint8_t ev[32] = {28};
    x_wr32(ev + 4, w->id, msb);
    x_wr32(ev + 8, atom, msb);
    x_wr32(ev + 12, x_now(s), msb);
    x_send_event(s, c, ev);
  }
  return 0;
}
#endif
