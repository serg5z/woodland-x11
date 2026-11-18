#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 320
#define WIN_H 240
#define MAX_STROKES 64
#define MAX_POINTS 512

enum { BTN_NONE, BTN_CLICK, BTN_CLEAR, BTN_QUIT };

typedef struct {
    unsigned short first;
    unsigned short length;
    unsigned char ink;
} Stroke;

static const char *const INK_NAMES[] = {"Green", "Blue", "Orange", "Magenta", "Red"};
static const unsigned char INK_RGB[][3] = {
    {35, 210, 125}, {65, 155, 255}, {255, 175, 55}, {230, 100, 230}, {255, 90, 85}
};
static const unsigned char BG_RGB[] = {8, 24, 28};
static const unsigned char PANEL_RGB[] = {18, 48, 52};
static const unsigned char CANVAS_RGB[] = {237, 242, 229};
static const unsigned char TEXT_RGB[] = {240, 247, 238};
static const unsigned char DARK_RGB[] = {8, 28, 31};
static const unsigned char BUTTON_RGB[] = {37, 87, 85};
static const unsigned char BUTTON_ACTIVE_RGB[] = {65, 129, 112};
static const unsigned char BORDER_RGB[] = {98, 156, 145};

static Display *dpy;
static Window win;
static GC gc;
static XFontStruct *font;
static unsigned long colors[13];
static XPoint points[MAX_POINTS];
static Stroke strokes[MAX_STROKES];
static unsigned point_count, stroke_count;
static unsigned clicks, ink_index;
static int active_button, active_stroke = -1, overflowed;
static int pointer_x = -1, pointer_y = -1;

static unsigned long component(unsigned char value, unsigned long mask) {
    unsigned shift = 0, bits = 0;
    if (!mask) return 0;
    while (((mask >> shift) & 1UL) == 0) ++shift;
    while ((mask >> (shift + bits)) & 1UL) ++bits;
    unsigned long maximum = bits >= sizeof(unsigned long) * 8 ? ~0UL : ((1UL << bits) - 1UL);
    return (((unsigned long)value * maximum + 127UL) / 255UL) << shift;
}

static unsigned long rgb(unsigned char r, unsigned char g, unsigned char b) {
    Visual *v = DefaultVisual(dpy, DefaultScreen(dpy));
    return component(r, v->red_mask) | component(g, v->green_mask) | component(b, v->blue_mask);
}

static int inside(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void fill(int x, int y, unsigned w, unsigned h, unsigned long color) {
    XSetForeground(dpy, gc, color);
    XFillRectangle(dpy, win, gc, x, y, w, h);
}

static void text(int x, int baseline, const char *s, unsigned long color) {
    XSetForeground(dpy, gc, color);
    XDrawString(dpy, win, gc, x, baseline, s, (int)strlen(s));
}

static void button(int which, int x, int y, int w, int h, const char *label) {
    XSetForeground(dpy, gc, active_button == which ? colors[6] : colors[5]);
    XFillRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, gc, colors[7]);
    XDrawRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
    int label_width = XTextWidth(font, label, (int)strlen(label));
    int tx = x + (w - label_width) / 2;
    int ty = y + (h + font->ascent - font->descent) / 2;
    text(tx, ty, label, colors[4]);
}

static void draw_pointer(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Pointer %03d,%03d", pointer_x < 0 ? 0 : pointer_x,
             pointer_y < 0 ? 0 : pointer_y);
    fill(184, 26, 126, 17, colors[0]);
    text(187, 39, buf, colors[3]);
}

static void draw_counter(void) {
    char buf[80];
    snprintf(buf, sizeof(buf), "Clicks: %u     Ink: %s", clicks,
             INK_NAMES[ink_index % (sizeof(INK_NAMES) / sizeof(INK_NAMES[0]))]);
    fill(10, 78, 300, 15, colors[0]);
    text(12, 89, buf, colors[3]);
}

static void canvas_point(unsigned short *x, unsigned short *y, int px, int py) {
    if (px < 13) px = 13;
    if (px > 306) px = 306;
    if (py < 115) py = 115;
    if (py > 226) py = 226;
    *x = (unsigned short)px;
    *y = (unsigned short)py;
}

static void draw_canvas(void) {
    fill(10, 97, 300, 132, colors[2]);
    XSetForeground(dpy, gc, colors[7]);
    XDrawRectangle(dpy, win, gc, 10, 97, 299, 131);
    text(15, 111, "Touch and drag here", colors[1]);
    for (unsigned i = 0; i < stroke_count; ++i) {
        Stroke *s = &strokes[i];
        if (!s->length) continue;
        XSetForeground(dpy, gc, colors[8 + s->ink]);
        if (s->length == 1) {
            XFillArc(dpy, win, gc, points[s->first].x - 2, points[s->first].y - 2, 5, 5, 0, 360 * 64);
        } else {
            XDrawLines(dpy, win, gc, points + s->first, s->length, CoordModeOrigin);
        }
    }
    if (overflowed) text(163, 111, "Trail limit reached - press Clear", colors[1]);
}

static void draw_all(void) {
    fill(0, 0, WIN_W, WIN_H, colors[0]);
    text(12, 20, "Woodland X11 Playground", colors[3]);
    text(12, 41, "Hello from Woodland X11!", colors[3]);
    draw_pointer();
    button(BTN_CLICK, 10, 51, 90, 27, "Click");
    button(BTN_CLEAR, 105, 51, 90, 27, "Clear");
    button(BTN_QUIT, 200, 51, 110, 27, "Quit");
    draw_counter();
    draw_canvas();
}

static void update_pointer(int x, int y) {
    if (pointer_x == x && pointer_y == y) return;
    pointer_x = x;
    pointer_y = y;
    draw_pointer();
}

static void start_stroke(int x, int y) {
    if (stroke_count >= MAX_STROKES || point_count >= MAX_POINTS) {
        overflowed = 1;
        draw_canvas();
        return;
    }
    unsigned index = stroke_count++;
    Stroke *s = &strokes[index];
    s->first = (unsigned short)point_count;
    s->length = 0;
    s->ink = (unsigned char)ink_index;
    active_stroke = (int)index;
    unsigned short px, py;
    canvas_point(&px, &py, x, y);
    points[point_count].x = (short)px;
    points[point_count].y = (short)py;
    ++point_count;
    s->length = 1;
    XSetForeground(dpy, gc, colors[8 + s->ink]);
    XFillArc(dpy, win, gc, (int)px - 2, (int)py - 2, 5, 5, 0, 360 * 64);
}

static void extend_stroke(int x, int y) {
    if (active_stroke < 0 || point_count >= MAX_POINTS) {
        if (active_stroke >= 0 && point_count >= MAX_POINTS && !overflowed) {
            overflowed = 1;
            draw_counter();
        }
        return;
    }
    unsigned short px, py;
    canvas_point(&px, &py, x, y);
    XPoint *previous = &points[point_count - 1];
    if (previous->x == (short)px && previous->y == (short)py) return;
    XSetForeground(dpy, gc, colors[8 + strokes[active_stroke].ink]);
    XDrawLine(dpy, win, gc, previous->x, previous->y, (int)px, (int)py);
    points[point_count].x = (short)px;
    points[point_count].y = (short)py;
    ++point_count;
    ++strokes[active_stroke].length;
}

static void activate(int which) {
    if (which == BTN_CLICK) {
        ++clicks;
        ink_index = (ink_index + 1) % (sizeof(INK_NAMES) / sizeof(INK_NAMES[0]));
        draw_counter();
        printf("CLICK count=%u ink=%s\n", clicks, INK_NAMES[ink_index]);
        fflush(stdout);
    } else if (which == BTN_CLEAR) {
        point_count = stroke_count = 0;
        active_stroke = -1;
        overflowed = 0;
        draw_counter();
        draw_canvas();
        puts("CLEAR points=0 strokes=0");
        fflush(stdout);
    } else if (which == BTN_QUIT) {
        puts("QUIT");
        fflush(stdout);
    }
}

static int button_at(int x, int y) {
    if (inside(x, y, 10, 51, 90, 27)) return BTN_CLICK;
    if (inside(x, y, 105, 51, 90, 27)) return BTN_CLEAR;
    if (inside(x, y, 200, 51, 110, 27)) return BTN_QUIT;
    return BTN_NONE;
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fputs("ERROR: cannot open DISPLAY\n", stderr);
        return 2;
    }
    int screen = DefaultScreen(dpy);
    Visual *visual = DefaultVisual(dpy, screen);
    if (DefaultDepth(dpy, screen) != 16 || visual->class != TrueColor || !visual->red_mask ||
        !visual->green_mask || !visual->blue_mask) {
        fprintf(stderr, "ERROR: expected a 16-bit TrueColor display; got depth=%d class=%d\n",
                DefaultDepth(dpy, screen), visual->class);
        XCloseDisplay(dpy);
        return 2;
    }

    colors[0] = rgb(BG_RGB[0], BG_RGB[1], BG_RGB[2]);
    colors[1] = rgb(PANEL_RGB[0], PANEL_RGB[1], PANEL_RGB[2]);
    colors[2] = rgb(CANVAS_RGB[0], CANVAS_RGB[1], CANVAS_RGB[2]);
    colors[3] = rgb(TEXT_RGB[0], TEXT_RGB[1], TEXT_RGB[2]);
    colors[4] = rgb(DARK_RGB[0], DARK_RGB[1], DARK_RGB[2]);
    colors[5] = rgb(BUTTON_RGB[0], BUTTON_RGB[1], BUTTON_RGB[2]);
    colors[6] = rgb(BUTTON_ACTIVE_RGB[0], BUTTON_ACTIVE_RGB[1], BUTTON_ACTIVE_RGB[2]);
    colors[7] = rgb(BORDER_RGB[0], BORDER_RGB[1], BORDER_RGB[2]);
    for (unsigned i = 0; i < sizeof(INK_RGB) / sizeof(INK_RGB[0]); ++i)
        colors[8 + i] = rgb(INK_RGB[i][0], INK_RGB[i][1], INK_RGB[i][2]);

    font = XLoadQueryFont(dpy, "fixed");
    if (!font) font = XLoadQueryFont(dpy, "6x13");
    if (!font) {
        fputs("ERROR: neither fixed nor 6x13 core font is available\n", stderr);
        XCloseDisplay(dpy);
        return 2;
    }
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, WIN_W, WIN_H, 0, colors[7], colors[0]);
    XStoreName(dpy, win, "Woodland X11 Playground");
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = WIN_W;
    hints.min_height = hints.max_height = WIN_H;
    XSetWMNormalHints(dpy, win, &hints);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);
    XMapWindow(dpy, win);
    XFlush(dpy);

    printf("CONNECTED depth=%d masks=%lx/%lx/%lx window=%ux%u\n", DefaultDepth(dpy, screen),
           visual->red_mask, visual->green_mask, visual->blue_mask, WIN_W, WIN_H);
    fflush(stdout);
    int running = 1;
    while (running) {
        XEvent e;
        XNextEvent(dpy, &e);
        switch (e.type) {
        case MapNotify:
            printf("MAPPED %ux%u\n", WIN_W, WIN_H);
            fflush(stdout);
            break;
        case Expose:
            if (e.xexpose.count == 0) draw_all();
            break;
        case MotionNotify:
            update_pointer(e.xmotion.x_root, e.xmotion.y_root);
            if (active_stroke >= 0 && (e.xmotion.state & Button1Mask))
                extend_stroke(e.xmotion.x, e.xmotion.y);
            break;
        case ButtonPress:
            update_pointer(e.xbutton.x_root, e.xbutton.y_root);
            if (e.xbutton.button != Button1) break;
            active_button = button_at(e.xbutton.x, e.xbutton.y);
            if (active_button != BTN_NONE) {
                if (active_button == BTN_CLICK) button(BTN_CLICK, 10, 51, 90, 27, "Click");
                else if (active_button == BTN_CLEAR) button(BTN_CLEAR, 105, 51, 90, 27, "Clear");
                else button(BTN_QUIT, 200, 51, 110, 27, "Quit");
            } else if (inside(e.xbutton.x, e.xbutton.y, 12, 99, 296, 128)) {
                start_stroke(e.xbutton.x, e.xbutton.y);
            }
            break;
        case ButtonRelease:
            update_pointer(e.xbutton.x_root, e.xbutton.y_root);
            if (e.xbutton.button != Button1) break;
            if (active_button != BTN_NONE) {
                int selected = active_button;
                active_button = BTN_NONE;
                draw_all();
                if (button_at(e.xbutton.x, e.xbutton.y) == selected) {
                    activate(selected);
                    if (selected == BTN_QUIT) running = 0;
                }
            } else if (active_stroke >= 0) {
                extend_stroke(e.xbutton.x, e.xbutton.y);
                /* Make the drag completion message a server-side drawing barrier. */
                XSync(dpy, False);
                printf("DRAG points=%u strokes=%u\n", point_count, stroke_count);
                fflush(stdout);
                active_stroke = -1;
            }
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == wm_delete) running = 0;
            break;
        default:
            break;
        }
        XFlush(dpy);
    }

    XDestroyWindow(dpy, win);
    XFreeGC(dpy, gc);
    XFreeFont(dpy, font);
    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
