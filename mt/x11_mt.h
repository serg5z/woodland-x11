/* Multi-touch contact tracker (P6). Portable C99, no I/O.
 *
 * The platform passes every controller read: the points the GT9xx reported (track ID, x, y,
 * size). The tracker keeps one contact per track ID and gives each a state:
 *
 *   NEW    first report of this ID
 *   MOVE   reported again, moved more than move_px since the last report
 *   STILL  reported again, not moved
 *   UP     not reported for up_after reads in a row (one missing report is not a release);
 *          shown for one read, then the slot is freed
 *
 * The controller sends no explicit up/down: a contact exists while it is reported. Counters
 * describe the controller's behaviour: points per report, malformed points, duplicate IDs,
 * reports in a different ID order, dropouts (a contact missing from a report but back
 * before release), ID changes (a contact released as a new ID appears close to it) and
 * contacts lost to failed reads. Points outside 0..width-1 / 0..height-1 are malformed. */
#ifndef X11_MT_H
#define X11_MT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X11_MT_MAX 5        /* the GT9xx family maximum; this panel is configured for 2 */
#define X11_MT_ID_MAX 15    /* track IDs are 4 bits */

typedef struct {
  uint8_t id;
  int16_t x, y;
  uint16_t size;
} x11_mt_point;

enum { X11_MT_FREE, X11_MT_NEW, X11_MT_MOVE, X11_MT_STILL, X11_MT_UP };

typedef struct {
  uint8_t state, id, missed, failed_end;
  int16_t x, y, x0, y0;
  uint16_t size;
  uint32_t t_down, last_ms, reports;
} x11_mt_contact;

typedef struct {
  uint16_t width, height;
  uint8_t up_after;   /* reads without the ID before UP (default 2) */
  uint8_t move_px;    /* movement above this is MOVE (default 1) */
  uint8_t near_px;    /* an ID change: a new contact this close to one just released (default 40) */
  x11_mt_contact c[X11_MT_MAX];
  uint8_t last_order[X11_MT_MAX], last_n;
  uint32_t seq;       /* incremented whenever anything visible changes */
  uint32_t last_change_ms;
  /* counters */
  uint32_t reads, failed_reads, touched_reads, by_count[X11_MT_MAX + 2], max_seen;
  uint32_t news, ups, moves, malformed, duplicate_ids, reorders, dropouts, id_changes, lost;
  uint32_t overflow;  /* a new ID with no free slot (it is taken when one frees) */
  uint32_t multi_ms_max;  /* longest time two or more contacts were down together */
  uint32_t multi_since;
} x11_mt;

void x11_mt_init(x11_mt *t, uint16_t width, uint16_t height);

/* One controller read at now_ms. ok = 0: the read failed (p, n ignored). Returns 1 if a
 * contact appeared, moved, was released or changed state. */
int x11_mt_sample(x11_mt *t, uint32_t now_ms, int ok, const x11_mt_point *p, int n);

/* Contacts down now (states NEW, MOVE, STILL). */
int x11_mt_count(const x11_mt *t);

/* Diagnostics lines ("mt.*", one per contact "mt.c<slot>=id,x,y,size,STATE"). */
size_t x11_mt_text(const x11_mt *t, char *out, size_t cap);

const char *x11_mt_state_name(int state);

#ifdef __cplusplus
}
#endif
#endif
