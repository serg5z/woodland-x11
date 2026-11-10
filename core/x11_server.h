/* X11 request handling after connection setup. Portable C99, no I/O and no host
 * dependency: memory, time, event delivery and the framebuffer come from the platform
 * (x11_platform). The transport frames requests with x11_request_size() and passes each
 * complete request to x11_client_request(); the reply or error goes to `out`, events (to
 * any client) through platform.deliver.
 *
 * Milestone 1: QueryExtension, ListExtensions, GetProperty, GetInputFocus, QueryBestSize,
 * InternAtom, GetAtomName, NoOperation, CreateGC/FreeGC.
 * Milestone 2A (first graphical client, xclock): windows (create, attributes, map, unmap,
 * configure, destroy, geometry, tree, coordinates), properties, pixmaps, the full GC with
 * clip rectangles, drawing (rectangles, lines, segments, points, polygons, arcs, images,
 * copies, text with the built-in font), colours for the TrueColor visual, the "fixed" font,
 * and the events Expose, GraphicsExpose, NoExpose, MapNotify, UnmapNotify, ConfigureNotify,
 * DestroyNotify, CreateNotify, PropertyNotify, FocusIn/FocusOut.
 * Milestone 2C (window managers as clients): SubstructureRedirect (MapRequest,
 * ConfigureRequest), ReparentWindow, ChangeSaveSet, stacking relative to a sibling,
 * GrabServer, selections, SendEvent, GetImage, the colour database, colormap installation
 * and an empty keyboard mapping (there is no keyboard).
 * Milestone 3A (pointer, x11_input.c): one core pointer fed by the platform (the GT911
 * touchscreen on the ESP32): MotionNotify (with hints), ButtonPress/ButtonRelease with
 * propagation, EnterNotify/LeaveNotify, active, passive and automatic pointer grabs
 * (owner-events, confine-to, synchronous freezing, AllowEvents with replay), QueryPointer,
 * WarpPointer, GetMotionEvents.
 * Everything else: BadImplementation (core requests not done) or BadRequest (not core).
 */
#ifndef X11_SERVER_H
#define X11_SERVER_H

#include "x11_render.h"
#include "x11_setup.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef X11_SERVER_CLIENTS
#define X11_SERVER_CLIENTS 192      /* client slots compiled in (ESP32 build: set by build_esp32.sh) */
#endif
#if X11_SERVER_CLIENTS < 1 || X11_SERVER_CLIENTS > 254
#error "X11_SERVER_CLIENTS must be 1..254 (slot 255 marks server-owned resources)"
#endif
#define X11_CLIENT_WORDS ((X11_SERVER_CLIENTS + 31) / 32)  /* one bit per client slot */
#define X11_MAX_RESOURCES 512       /* default resource table: windows, pixmaps, GCs, fonts, cursors of all clients */
#define X11_MAX_RESOURCES_LIMIT 16384  /* largest configurable table (16-bit index) */
#define X11_MAX_SAVE_SET 128        /* ChangeSaveSet entries of all clients */
#ifndef X11_MAX_RETAINED
/* Clients closed in RetainPermanent/RetainTemporary mode whose resources are kept at once.
 * A closing client beyond it is destroyed as in Destroy mode (core.retain_refused): retained
 * resources hold memory with no connection to account for it (a full-screen pixmap is 750 KiB).
 * Root-background setters retain one pixmap and KillClient the previous one (Esetroot). */
#define X11_MAX_RETAINED 4
#endif
/* Atoms made at run time: names of any length (0 to a request's maximum) are kept in one pool
 * of X11_ATOM_POOL bytes, each with its own NUL, found through an offset/length table. Both are
 * allocated once with the server; InternAtom answers BadAlloc when either is full (atoms are
 * never freed, as in X.Org). */
#define X11_MAX_DYNAMIC_ATOMS 512
#define X11_ATOM_POOL 16384
#define X11_PREDEFINED_ATOMS 68
#define X11_MAX_PROPERTY 65536      /* bytes per property value */
#define X11_MAX_CLIP_RECTS 64       /* SetClipRectangles, clip-mask bitmaps as rectangles */
#define X11_DAMAGE_RECTS 16        /* changed screen areas kept for the display */
#define X11_MAX_SELECTIONS 16      /* selections with an owner (SetSelectionOwner) */
#define X11_REPLY_MAX (32 + X11_MAX_PROPERTY)  /* largest reply: GetProperty of a full property */
#define X11_MAX_BUTTON_GRABS 64    /* passive button grabs (GrabButton) of all clients */
#define X11_INPUT_QUEUE 32         /* pointer events held while a synchronous grab freezes it */
#define X11_REPLY_STREAM_CHUNK 4096 /* bounded GetImage producer chunk */

/* A reply payload generated incrementally from an immutable-for-the-duration drawable. The
 * transport serializes X requests while a stream is active, but continues polling touch and
 * draining the stream. Image byte/bit order follows the server setup, not client byte order. */
typedef struct {
  const xr_surface *src;
  size_t offset, total, row_bytes, bitmap_row;
  uint32_t planes;
  int src_x, src_y;
  uint16_t width, height;
  uint8_t format, depth, active;
} x11_reply_stream;

/* Error codes (protocol encoding appendix). */
enum {
  X11_BAD_REQUEST = 1, X11_BAD_VALUE = 2, X11_BAD_WINDOW = 3, X11_BAD_PIXMAP = 4, X11_BAD_ATOM = 5,
  X11_BAD_CURSOR = 6, X11_BAD_FONT = 7, X11_BAD_MATCH = 8, X11_BAD_DRAWABLE = 9, X11_BAD_ACCESS = 10,
  X11_BAD_ALLOC = 11, X11_BAD_COLORMAP = 12, X11_BAD_GC = 13, X11_BAD_IDCHOICE = 14, X11_BAD_NAME = 15,
  X11_BAD_LENGTH = 16, X11_BAD_IMPLEMENTATION = 17
};

/* Resource types. */
enum { X11_RES_FREE = 0, X11_RES_GC = 1, X11_RES_WINDOW = 2, X11_RES_PIXMAP = 3, X11_RES_FONT = 4, X11_RES_CURSOR = 5,
       X11_RES_COLORMAP = 6 };

typedef struct {
  void *(*alloc)(void *ctx, size_t n);   /* NULL: malloc (ESP32: PSRAM via heap_caps_malloc) */
  void (*release)(void *ctx, void *p);
  uint32_t (*now_ms)(void *ctx);         /* server time for events; NULL: 0 */
  /* Writes an event (32 bytes) to client slot `client`. NULL: events are dropped. */
  void (*deliver)(void *ctx, int client, const uint8_t *data, size_t len);
  void *ctx;
  uint16_t *framebuffer;                 /* width*height RGB565; NULL: allocated by the core */
  /* Runtime diagnostics, served as the read-only root property _WOODLAND_DIAG (generated at
   * each read; never stored, never in ListProperties, no PropertyNotify). Writes "key=value"
   * lines into out (at most cap bytes), returns the length. Counters are cumulative: reading
   * never resets them, and GetProperty's delete flag does not apply to this property.
   * NULL: the property does not exist. Must not include credentials or cookies. */
  size_t (*diag)(void *ctx, char *out, size_t cap);
} x11_platform;

typedef struct {
  uint8_t active;                 /* setup completed */
  uint8_t msb;                    /* client byte order */
  uint16_t seq;                   /* last request's sequence number */
  uint32_t id_base, id_mask;
  uint32_t requests, errors;      /* diagnostics */
  uint16_t resources;             /* resources owned (quota: cfg.max_resources_per_client) */
  uint8_t close_mode;             /* SetCloseDownMode: 0 Destroy, 1 RetainPermanent, 2 RetainTemporary */
  uint8_t retained;               /* closed in a Retain mode (1 or 2): resources kept, slot taken, no connection */
} x11_client;

struct x11_window;
struct x11_pixmap;
struct x11_gc;

typedef struct {
  uint32_t id;
  uint8_t owner;                  /* client slot */
  uint8_t type;                   /* X11_RES_* */
  void *obj;                      /* window, pixmap, GC (NULL for fonts and cursors) */
} x11_resource;

typedef struct {
  uint32_t atom;                  /* 0: free entry */
  uint32_t window;                /* owner window */
  uint32_t time;                  /* last-change time */
  int16_t client;                 /* owning client slot */
} x11_selection;

/* ChangeSaveSet entry. */
typedef struct {
  uint32_t window;                /* 0: free entry */
  uint8_t client;
} x11_saveset;

/* Passive button grab (GrabButton). */
typedef struct {
  uint32_t window;                /* 0: free entry */
  uint32_t confine, cursor;
  uint16_t modifiers;             /* 0x8000: AnyModifier */
  uint16_t event_mask;
  uint8_t button;                 /* 0: AnyButton */
  uint8_t client, owner_events, pointer_mode, keyboard_mode;
} x11_button_grab;

enum { X11_GRAB_ACTIVE = 0, X11_GRAB_PASSIVE = 1, X11_GRAB_AUTOMATIC = 2 };

/* The pointer grab in force. */
typedef struct {
  int16_t client;                 /* -1: none */
  uint8_t kind;                   /* X11_GRAB_* */
  uint8_t owner_events, pointer_mode;  /* pointer_mode: 0 Synchronous, 1 Asynchronous */
  uint16_t event_mask;
  uint32_t window, confine, cursor;
} x11_pointer_grab;

/* A pointer event from the platform, before the server processed it. */
typedef struct {
  uint8_t type;                   /* 4 ButtonPress, 5 ButtonRelease, 6 MotionNotify */
  uint8_t button;
  int16_t x, y;
  uint32_t time;
} x11_device_event;

typedef struct x11_server {
  x11_server_config cfg;
  x11_platform plat;
  x11_client clients[X11_SERVER_CLIENTS];
  int max_clients;                /* runtime limit (cfg.max_clients), <= X11_SERVER_CLIENTS */
  int nclients;                   /* slots in use (setup pending or active) */
  /* Resource table: entries never move (pointers stay valid); found through an open-addressing
   * index of entry numbers (0 empty, 0xFFFF deleted). */
  x11_resource *resources;
  int max_resources;
  int resources_used;
  int res_hint;                   /* where the search for a free entry starts */
  uint16_t *rindex;
  uint32_t rindex_mask, rindex_deleted;
  x11_saveset save_sets[X11_MAX_SAVE_SET];
  char *atom_pool;                /* names, each followed by a NUL */
  uint16_t *atom_off, *atom_len;  /* atom 69 + 1 + i: its name in atom_pool */
  int atom_count;
  uint32_t atom_pool_used;
  struct x11_window *root;
  xr_surface fb;                  /* the screen */
  uint32_t fb_rev;                /* incremented whenever the screen changes */
  xr_damage fb_op;                /* pixels written by the current request */
  xr_rect damage[X11_DAMAGE_RECTS];  /* changed areas not yet taken by the display */
  int ndamage;
  uint32_t focus;                 /* window, or 0 None, 1 PointerRoot */
  uint8_t focus_revert;
  uint8_t oom;                    /* an allocation failed (diagnostics) */
  int16_t grab;                   /* GrabServer: client slot, or -1 */
  uint32_t kills[X11_CLIENT_WORDS];  /* KillClient: bit c = close client c */
  /* SetScreenSaver values, as reported by GetScreenSaver. The server never activates a
   * screen saver (the panel's backlight is managed outside X11). */
  int16_t ss_timeout, ss_interval;
  uint8_t ss_blanking, ss_exposures;
  /* ChangeKeyboardControl / ChangePointerControl values, reported by the Get requests. There
   * is no keyboard or bell; the touch pointer has no acceleration. */
  int16_t kb_pitch, kb_duration;
  uint8_t kb_click, kb_bell, kb_auto_repeat;
  uint32_t kb_leds;
  uint8_t kb_repeats[32];          /* auto-repeat bit per keycode */
  /* ChangeKeyboardMapping / SetModifierMapping: client-visible maps only (there is no keyboard,
   * nothing interprets them). kb_map: keysyms of keycodes 8..255, kb_width per keycode
   * (allocated by the first change, from the default keymap; NULL: the default keymap, width 2). mod_of[k]: modifier
   * 1..8 (Shift..Mod5) of keycode k, 0 none (default: X.Org's, mod_defaults). */
  uint32_t *kb_map;
  uint8_t kb_width;
  uint8_t mod_of[256];
  uint32_t retain_refused;        /* Retain-mode closes destroyed because X11_MAX_RETAINED were kept */
  uint8_t retained_empty;         /* a retained client's last resource was freed (client_request) */
  int16_t ptr_accel_num, ptr_accel_den, ptr_threshold;
  x11_selection selections[X11_MAX_SELECTIONS];
  struct x11_cmap_use *cmap_uses; /* colours each client allocated, per colormap (FreeColors) */
  /* pointer (x11_input.c) */
  int16_t ptr_x, ptr_y;           /* root coordinates, as processed */
  uint16_t ptr_buttons;           /* button state bits: 0x100 Button1 .. 0x1000 Button5 */
  int16_t in_x, in_y;             /* the platform's last input (ahead while frozen) */
  uint16_t in_buttons;
  struct x11_window *sprite;      /* the deepest viewable window containing the pointer */
  x11_pointer_grab pgrab;
  uint32_t pgrab_time;            /* last-pointer-grab time */
  int16_t kgrab_client;            /* GrabKeyboard: client slot, or -1 (there is no keyboard) */
  uint32_t kgrab_window, kgrab_time;
  uint8_t frozen;                 /* a synchronous pointer grab froze event processing */
  uint8_t sync_next;              /* AllowEvents SyncPointer: freeze at the next press/release */
  uint8_t replay_valid;           /* the event that froze the pointer can be replayed */
  uint32_t hints[X11_CLIENT_WORDS];  /* bit c: a MotionNotify hint went to client c */
  uint16_t swallowed;             /* physical buttons (bit 0 = 1) whose press was dropped: drop the release */
  uint8_t ptr_map[5];             /* SetPointerMapping: physical button i+1 -> logical (0: disabled) */
  uint8_t hi_down;                /* logical buttons above 5 down (no state bit; the grab waits for them) */
  uint8_t draining;
  x11_device_event replay;
  x11_device_event queue[X11_INPUT_QUEUE];
  uint8_t qhead, qlen;
  uint32_t input_events, input_dropped;  /* diagnostics */
  x11_button_grab button_grabs[X11_MAX_BUTTON_GRABS];
  xr_pool pool;                   /* region temporaries (XR_POOL_REGIONS x 776 bytes, here not on the stack) */
} x11_server;

/* Returns 0, or -1 when memory for the framebuffer or tables is missing. */
int x11_server_init(x11_server *s, const x11_server_config *cfg);
int x11_server_init_platform(x11_server *s, const x11_server_config *cfg, const x11_platform *plat);
void x11_server_free(x11_server *s);

/* Takes a free client slot (setup pending), or -1 when max_clients slots are in use. */
int x11_client_open(x11_server *s);
size_t x11_client_setup(x11_server *s, int c, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                        x11_setup_result *result);
size_t x11_request_size(const x11_server *s, int c, const uint8_t hdr[4]);
/* A request whose length field exceeds max_request_units (x11_request_size returns 0 for it):
 * counts the request and writes its BadLength error (32 bytes, out_cap >= 32). The client
 * stays connected; the transport discards the request's bytes. Returns the bytes to discard,
 * header included, in *skip. Returns 0 (and *skip 0) when the length is not too large. */
size_t x11_client_request_too_long(x11_server *s, int c, const uint8_t hdr[4], uint8_t *out, size_t out_cap,
                                   size_t *skip);
/* out_cap >= X11_REPLY_MAX. Events caused by the request go through plat.deliver before
 * this returns (to the requester and to other clients). */
size_t x11_client_request(x11_server *s, int c, const uint8_t *req, size_t len, uint8_t *out, size_t out_cap,
                          int *close_after);
/* Network transports use this variant to stream GetImage payloads larger than the reply
 * scratch buffer. `stream` must be zeroed before each request. Other replies are unchanged. */
size_t x11_client_request_stream(x11_server *s, int c, const uint8_t *req, size_t len, uint8_t *out,
                                 size_t out_cap, int *close_after, x11_reply_stream *stream);
/* Produces up to cap payload bytes and advances the stream; returns zero when complete. */
size_t x11_reply_stream_read(x11_reply_stream *stream, uint8_t *out, size_t cap);
/* GrabServer: while one client holds the server, the transport processes no request of
 * any other client (it keeps reading them). */
static inline int x11_client_may_run(const x11_server *s, int c) { return s->grab < 0 || s->grab == c; }

/* Next client to close because of KillClient (its bit is cleared), or -1 when none. The
 * transport closes that connection, which calls x11_client_close. */
int x11_take_kill(x11_server *s);

/* Connection closed: destroys the client's windows (with their events and exposures) and
 * frees all its resources (close-down mode Destroy). */
void x11_client_close(x11_server *s, int c);

/* Screen areas changed since the last call (merged into at most `max` rectangles); clears
 * them. The display pushes exactly these to the LCD. Returns the count. */
int x11_take_damage(x11_server *s, xr_rect *out, int max);

/* Pointer input from the platform (touchscreen, test harness): root position and button
 * state (bit 0 = Button1 .. bit 4 = Button5). Generates MotionNotify, crossing events and
 * ButtonPress/ButtonRelease as needed, through plat.deliver. Coordinates are clamped to the
 * screen. While a synchronous grab freezes the pointer the events wait in a bounded queue;
 * a press that does not fit is dropped together with its release, so the logical button
 * state never stays down. Call between requests (not from inside plat.deliver). */
void x11_pointer_input(x11_server *s, int x, int y, unsigned buttons);

const char *x11_atom_name(const x11_server *s, uint32_t atom);
int x11_resources_used(const x11_server *s);

/* Core counters for diagnostics: clients, resources, atoms, input. Appends "key=value" lines
 * to out (cap bytes, NUL-terminated); returns the length written. */
size_t x11_server_stats(const x11_server *s, char *out, size_t cap);
#define X11_DIAG_ATOM "_WOODLAND_DIAG"
#define X11_DIAG_MAX 16384          /* largest diagnostics text (generated into the reply buffer) */

/* The platform sets a property on the root window (not a client request): the value replaces
 * any earlier one (type STRING, format 8), and clients that select PropertyChange on the root
 * get PropertyNotify (NewValue) through plat.deliver, exactly as for ChangeProperty. Used for
 * _WOODLAND_GESTURE (pinch). Returns 0, or -1 (no memory, too large, the atom table full, or
 * the diagnostics atom). Call between requests (not from inside plat.deliver). */
#if !defined(ARDUINO) || defined(X11_TOUCH_DIAG)  /* (normal ESP32 images: none of it, see the .c) */
int x11_root_property_set(x11_server *s, const char *name, const uint8_t *data, size_t n);
#endif

#ifdef __cplusplus
}
#endif
#endif
