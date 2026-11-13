/* Internal structures shared by x11_server.c and x11_window.c (not a public API). */
#ifndef X11_INTERNAL_H
#define X11_INTERNAL_H

#include <stdlib.h>
#include <string.h>

#include "x11_font.h"
#include "x11_server.h"

/* Client bit sets (one bit per slot). */
static inline int x_bit(const uint32_t *set, int c) { return (int)(set[c >> 5] >> (c & 31) & 1u); }
static inline void x_bit_set(uint32_t *set, int c) { set[c >> 5] |= 1u << (c & 31); }
static inline void x_bit_clear(uint32_t *set, int c) { set[c >> 5] &= ~(1u << (c & 31)); }

/* ---- objects ----------------------------------------------------------------------------- */

typedef struct x11_prop {
  struct x11_prop *next;
  uint32_t name, type;
  uint8_t format;               /* 8, 16, 32 */
  uint32_t nbytes;
  uint8_t *data;                /* little-endian units */
} x11_prop;

/* One client's event selection on a window (ChangeWindowAttributes event-mask). A window
 * keeps a list sorted by client slot, so memory grows with actual selections, not with the
 * number of client slots, and events go to clients in slot order. */
typedef struct x11_sel {
  struct x11_sel *next;
  uint32_t mask;
  uint8_t client;
} x11_sel;

typedef struct x11_window {
  uint32_t id;
  uint8_t owner;
  struct x11_window *parent, *first, *last, *below, *above;  /* children bottom..top */
  int16_t x, y;                 /* outer top-left, relative to the parent's inside */
  uint16_t w, h, bw;            /* inside size, border width */
  uint8_t input_only, mapped, override_redirect, save_under, backing_store, bit_gravity, win_gravity;
  uint8_t bg_mode;              /* 0 None, 1 pixel, 2 ParentRelative, 3 pixmap (bg_tile) */
  uint32_t bg_pixel, border_pixel, colormap, cursor;
  uint32_t backing_planes, backing_pixel;  /* stored and reported (no backing store is kept) */
  x11_sel *sels;                /* event selections, by client slot */
  uint16_t dont_propagate;
  x11_prop *props;
  xr_surface *bg_tile, *border_tile;  /* copies of background/border pixmaps, or NULL */
} x11_window;

typedef struct x11_pixmap {
  xr_surface surf;
} x11_pixmap;

/* Colours a client allocated in a colormap (AllocColor), for FreeColors; see x11_server.c. */
typedef struct x11_cmap_use {
  struct x11_cmap_use *next;
  uint32_t cmap;
  uint8_t client;
  uint8_t count[128];  /* red 0..31, green 32..95, blue 96..127 */
} x11_cmap_use;

typedef struct x11_gc {
  uint8_t depth;
  uint8_t function, line_style, cap_style, join_style, fill_style, fill_rule, subwindow_mode, graphics_exposures,
      arc_mode;
  uint8_t ndash;                /* dash list length 1..XR_MAX_DASHES; 0: longer (kept by count only) */
  uint8_t dash[XR_MAX_DASHES];
  uint32_t plane_mask, fg, bg, clip_mask;
  xr_surface *tile, *stipple;   /* copies taken when set (the protocol allows it); NULL: default */
  const x11_font *font;         /* NULL: the default font; kept after CloseFont (fonts are static) */
  uint16_t line_width, dash_offset;
  int16_t ts_x, ts_y, clip_x, clip_y;
  int nclip;                    /* -1: no clip rectangles (clip-mask None); else count */
  xr_rect clip[X11_MAX_CLIP_RECTS];
} x11_gc;

/* Event selections. */
static inline uint32_t xw_mask(const x11_window *w, int c) {
  for (const x11_sel *e = w->sels; e && e->client <= c; e = e->next)
    if (e->client == c) return e->mask;
  return 0;
}
static inline uint32_t xw_all_masks(const x11_window *w) {
  uint32_t all = 0;
  for (const x11_sel *e = w->sels; e; e = e->next) all |= e->mask;
  return all;
}
#define XW_FOR_SEL(w, e) for (const x11_sel *e = (w)->sels; e; e = e->next)

/* ---- byte order helpers ------------------------------------------------------------------ */

static inline uint16_t x_rd16(const uint8_t *p, int msb) {
  return msb ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]);
}
static inline uint32_t x_rd32(const uint8_t *p, int msb) {
  return msb ? (uint32_t)x_rd16(p, 1) << 16 | x_rd16(p + 2, 1) : (uint32_t)x_rd16(p + 2, 0) << 16 | x_rd16(p, 0);
}
static inline void x_wr16(uint8_t *p, uint16_t v, int msb) {
  if (msb) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
  else { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
}
static inline void x_wr32(uint8_t *p, uint32_t v, int msb) {
  if (msb) { x_wr16(p, (uint16_t)(v >> 16), 1); x_wr16(p + 2, (uint16_t)v, 1); }
  else { x_wr16(p, (uint16_t)v, 0); x_wr16(p + 2, (uint16_t)(v >> 16), 0); }
}
static inline size_t x_pad4(size_t n) { return (n + 3u) & ~(size_t)3u; }

/* ---- shared server helpers --------------------------------------------------------------- */

void *x_alloc(x11_server *s, size_t n);
void x_free(x11_server *s, void *p);
x11_resource *x_find(x11_server *s, uint32_t id);
/* NULL when the table is full or the owner's quota is used up (callers answer BadAlloc). */
x11_resource *x_add(x11_server *s, uint32_t id, int owner, int type, void *obj);
/* Drops a table entry (the caller frees its object): index, owner's count, entry. */
void x_res_remove(x11_server *s, x11_resource *r);
/* Removes w from every client's save-set (w is being destroyed). */
void x_saveset_forget(x11_server *s, uint32_t window);
uint32_t x_now(x11_server *s);
void x_prop_free_all(x11_server *s, x11_window *w);

/* Sends a 32-byte event to `client`, with that client's current sequence number. */
void x_send_event(x11_server *s, int client, uint8_t *ev);

/* ---- windows (x11_window.c) ---------------------------------------------------------------- */

#define X_EV_BUTTON_PRESS 0x00000004u
#define X_EV_EXPOSURE 0x00008000u
#define X_EV_VISIBILITY 0x00010000u
#define X_EV_STRUCTURE 0x00020000u
#define X_EV_RESIZE_REDIRECT 0x00040000u
#define X_EV_SUBSTRUCTURE 0x00080000u
#define X_EV_SUBSTRUCTURE_REDIRECT 0x00100000u
#define X_EV_FOCUS 0x00200000u
#define X_EV_PROPERTY 0x00400000u
#define X_EV_COLORMAP_CHANGE 0x00800000u

x11_window *xw_root_create(x11_server *s);
void xw_root_reset(x11_server *s);
void xw_repaint_border(x11_server *s, x11_window *w);
int xw_viewable(const x11_window *w);
void xw_abs(const x11_window *w, int *ax, int *ay);          /* inside origin on screen */
/* Visible part of the window's inside, screen coordinates. include_inferiors: do not clip
 * by mapped children (GC subwindow-mode IncludeInferiors). */
void xw_visible(const x11_window *w, xr_region *out, int include_inferiors);
void xw_map(x11_server *s, x11_window *w);
void xw_unmap(x11_server *s, x11_window *w, int send_events);
void xw_destroy(x11_server *s, x11_window *w);              /* with children, events, exposures */
/* stack: XW_STACK_NONE, or Above/Below relative to `sib` (a sibling; NULL: top/bottom). */
enum { XW_STACK_NONE = -1, XW_STACK_ABOVE = 0, XW_STACK_BELOW = 1 };
void xw_configure(x11_server *s, x11_window *w, int x, int y, int wd, int ht, int bw, int stack, x11_window *sib);
/* Moves w (unmapping it first if mapped) under new_parent at (x, y), with ReparentNotify.
 * The caller maps it again if it was mapped (MapWindow semantics, redirect included). */
void xw_reparent(x11_server *s, x11_window *w, x11_window *new_parent, int x, int y);
void xw_circulate(x11_server *s, x11_window *child, int place);  /* place 0 Top, 1 Bottom */
/* The client other than `except` that selected `bit` (a redirect mask) on w, or -1. */
int xw_redirect_client(const x11_server *s, const x11_window *w, uint32_t bit, int except);
/* Focus: FocusOut to the old window, FocusIn to the new one (0 None, 1 PointerRoot). */
void xw_set_focus(x11_server *s, uint32_t focus, uint8_t revert_to);
/* Paint the background of `area` (screen coords) in w's visible part; optionally Expose. */
void xw_clear(x11_server *s, x11_window *w, const xr_region *area, int send_expose);
void xw_link_top(x11_window *parent, x11_window *w);
void xw_unlink(x11_window *w);
/* A copy of a pixmap's contents for a window background or border (NULL: out of memory). */
xr_surface *xw_tile_copy(x11_server *s, const xr_surface *src);
void xw_tile_free(x11_server *s, xr_surface **tile);
/* Sets client c's event mask on w (0 removes the selection). Returns 0, or -1 when memory
 * for a new selection is missing (the caller answers BadAlloc). */
int xw_select(x11_server *s, x11_window *w, int c, uint32_t mask);
void xw_free_sels(x11_server *s, x11_window *w);

/* ---- pointer (x11_input.c) ------------------------------------------------------------- */

/* GrabPointer / GrabKeyboard statuses */
enum { X11_GRAB_SUCCESS = 0, X11_ALREADY_GRABBED = 1, X11_GRAB_INVALID_TIME = 2, X11_GRAB_NOT_VIEWABLE = 3,
       X11_GRAB_FROZEN = 4 };

void x_input_init(x11_server *s);
/* After windows changed (map, unmap, configure, restack, reparent, end of each request):
 * releases grabs whose window became unviewable, finds the window under the pointer again
 * and sends crossing events if it changed, then processes queued pointer events. */
void x_input_update(x11_server *s);
void x_input_window_destroyed(x11_server *s, x11_window *w);  /* passive grabs on w, sprite */
void x_input_client_gone(x11_server *s, int c);               /* its grabs (no events to it) */
/* The child of w that contains the pointer, or NULL. */
x11_window *x_input_child(x11_server *s, x11_window *w);
void x_input_reset_hint(x11_server *s, int c);                /* QueryPointer, GetMotionEvents */
int x_grab_pointer(x11_server *s, int c, x11_window *w, int owner_events, uint16_t mask, int pointer_mode,
                   x11_window *confine, uint32_t cursor, uint32_t time);
void x_ungrab_pointer(x11_server *s, int c, uint32_t time);
void x_change_active_grab(x11_server *s, int c, uint16_t mask, uint32_t cursor, uint32_t time);
/* GrabButton: 0, or an error code (BadAccess: another client's grab conflicts; BadAlloc). */
int x_grab_button(x11_server *s, int c, const x11_button_grab *g);
void x_ungrab_button(x11_server *s, int c, uint32_t window, uint8_t button, uint16_t modifiers);
void x_allow_events(x11_server *s, int c, int mode, uint32_t time);
int x_grab_keyboard(x11_server *s, int c, x11_window *w, uint32_t time);
void x_ungrab_keyboard(x11_server *s, int c, uint32_t time);
/* WarpPointer: moves the pointer (root coordinates) as if the user moved it. */
void x_warp_pointer(x11_server *s, int x, int y);

#endif
