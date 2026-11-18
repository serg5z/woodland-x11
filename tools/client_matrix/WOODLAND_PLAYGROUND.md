# Woodland X11 Playground

A compact 320×240 RGB565 demo built with core Xlib and libX11. It creates one window, one GC and one core font. It does not use pixmaps, XRender, toolkit libraries, XTEST, or extensions. The **Click** button increments the counter and cycles the ink color; **Clear** removes recorded strokes; **Quit** closes cleanly. Drag inside the light canvas to draw. Pointer coordinates are shown at the top right. Expose redraw reconstructs the full interface and saved strokes from bounded in-process point arrays.

The drawing history is bounded to 64 strokes and 512 points; when full it stops accepting points until Clear. Pointer movement updates only the coordinate field and the affected stroke segment.

## Build

On a system with a C99 compiler, `pkg-config`, and Xlib development files:

```sh
tools/client_matrix/build_woodland_playground.sh
```

This builds `build/woodland_x11_playground`. The only library dependency is X11 (`pkg-config x11`). Runtime requires a 16-bit TrueColor X server with the core `fixed` or `6x13` font available.

## Run on the CYD

Ensure no window manager is running, use the existing Xauthority settings, then:

```sh
DISPLAY="$CYD_IP:0" XAUTHORITY=/path/to/existing.Xauthority \
  build/woodland_x11_playground
```

The window is fixed at 320×240 and positioned at the origin. Touch **Click** to increment the counter and cycle drawing ink, draw with a finger in the canvas, **Clear** to erase strokes, and **Quit** to exit. No firmware or server change is needed.

## Host regression

Run the Xvfb and Woodland host-server smoke test (no window manager):

```sh
python3 tools/client_matrix/run_woodland_playground_smoke.py \
  --out-dir reports/woodland-playground
```

The test verifies map, click, drag, clear, quit, clean window removal and saves root screenshots for both environments. Xvfb must support depth 16 and listen on loopback TCP; the test uses the host server's `--input` pointer path. All test processes are cleaned up in a `finally` block.
