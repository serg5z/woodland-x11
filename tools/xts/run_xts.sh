#!/bin/bash
# Runs an XTS scenario against Xvfb (reference) or the Woodland host server.
#   run_xts.sh xvfb|woodland|woodland-fonts OUTDIR [SCENARIO]      (default scenario: Xproto)
#   run_xts.sh font-diff OUTDIR                                    (differential font check)
# The Woodland source is expected at /src/x11 (bind mount of experiments/x11-esp32, read-only),
# or at $SRC: a copy of any commit's experiments/x11-esp32 (`git archive`), to test that commit.
#   xvfb            Xvfb :1 (800x480x16, like the panel), XTS's test fonts on its font path
#   woodland        the ASan/UBSan host server on :2 (built-in fonts only)
#   woodland-fonts  the same with XTS's test fonts compiled in (-DX11_TEST_FONTS) on :2
#   font-diff       QueryFont/ListFontsWithInfo/QueryTextExtents of every XTS font: Xvfb
#                   against the test-font server (font_diff.py)
# Scenarios: Xproto (122 core protocol tests), XprotoFont (the 15 font and text tests,
# added to tet_scen here), any single test such as pOpenFont, or a comma-separated list of
# test directories (Xproto/pFreeColors,Xlib9/XDrawString).
# Results: OUTDIR/xts.log (one line per test purpose), OUTDIR/<date>/journal
# (xts-report -f journal), OUTDIR/server.log (the Woodland server's request log).
set -e
target=$1 out=$2 scen=${3:-Xproto} SRC=${SRC:-/src/x11}
mkdir -p "$out"
export TET_ROOT=/opt/xts-src TET_EXECUTE=/opt/xts-src/xts5 XSET=/usr/bin/xset XDPYINFO=/usr/bin/xdpyinfo
export TCC=/opt/xts-src/src/tet3/tcc/tcc XTS_REPORT=/opt/xts-src/xts5/src/bin/reports/xts-report
export XTS_CONFIG=/opt/xts-src/xts5/bin/xts-config
FONTS=/opt/xts-src/xts5/fonts
cd /opt/xts-src
grep -q '^XprotoFont' xts5/tet_scen || {
  printf 'XprotoFont\n' >> xts5/tet_scen
  for t in OpenFont CloseFont QueryFont QueryTextExtents ListFonts ListFontsWithInfo SetFontPath GetFontPath \
           PolyText8 PolyText16 ImageText8 ImageText16 CreateGlyphCursor CreateGC ChangeGC; do
    printf '\t/Xproto/p%s\n' $t >> xts5/tet_scen
  done
  printf '\n' >> xts5/tet_scen
}
# A comma-separated list (e.g. Xproto/pRotateProperties,Xlib9/XDrawString) is run as one
# scenario made for it.
if [[ "$scen" == *,* || "$scen" == */* ]]; then
  list=$scen
  scen="List$(echo "$list" | cksum | cut -d' ' -f1)"
  grep -q "^$scen\$" xts5/tet_scen || {
    printf '%s\n' "$scen" >> xts5/tet_scen
    for t in ${list//,/ }; do printf '\t/%s\n' "$t" >> xts5/tet_scen; done
    printf '\n' >> xts5/tet_scen
  }
fi
# Xvfb is always needed: xts-config derives the configuration from a live server.
if ! xdpyinfo -display 127.0.0.1:1 >/dev/null 2>&1; then
  Xvfb :1 -screen 0 800x480x16 -nolisten unix -listen tcp -ac >/tmp/xvfb.log 2>&1 &
  sleep 2
fi
DISPLAY=127.0.0.1:1 perl xts5/bin/xts-config < xts5/tetexec.cfg.in > /opt/xvfb.cfg
# XTS sets XT_FONTPATH at the start of each test. For a TCP display xts-config takes it from
# the running server, but Xvfb resets its font path when its last client leaves, so `xset fp+`
# cannot be used: the test fonts go into the configuration directly.
sed -i -e "s#^XT_FONTPATH=.*#XT_FONTPATH=/usr/share/fonts/X11/misc,built-ins,$FONTS#" \
       -e "s#^XT_FONTPATH_GOOD=.*#XT_FONTPATH_GOOD=/usr/share/fonts/X11/misc,built-ins#" /opt/xvfb.cfg
cfg=/opt/xvfb.cfg
if [ "$target" != xvfb ]; then
  for p in /proc/[0-9]*; do  # any host server still running (it may hold :2)
    prog=$(tr '\0' '\n' < $p/cmdline 2>/dev/null | head -1)
    case "$(basename "$prog" 2>/dev/null)" in x11_host_server*) kill ${p#/proc/};; esac
  done
  sleep 1
  rm -rf /opt/x11 && cp -r "$SRC" /opt/x11
  server=/opt/x11/build/x11_host_server_san
  if [ "$target" = woodland-fonts ] || [ "$target" = font-diff ]; then
    make -C /opt/x11 build/x11_host_server_testfonts TEST_FONT_DIR=$FONTS >/dev/null
    server=/opt/x11/build/x11_host_server_testfonts
  else
    make -C /opt/x11 build/x11_host_server_san >/dev/null
  fi
  # --reset: the protocol's server reset at the last close, as X.Org (the panel runs without it)
  ASAN_OPTIONS=detect_leaks=0 $server -d 2 --reset > "$out/server.log" 2>&1 &
  sleep 1
  if [ "$target" = font-diff ]; then
    python3 "$SRC"/tools/xts/font_diff.py $FONTS 6001 6002 --fontpath $FONTS | tee "$out/font-diff.log"
    exit 0
  fi
  # xts-config needs `xset q`; the values below are what the Woodland server reports (xdpyinfo).
  sed -e "s/^DISPLAY=.*/DISPLAY=127.0.0.1:2/" -e "s/^XT_VISUAL_CLASSES=.*/XT_VISUAL_CLASSES=TrueColor(16)/" \
      -e "s/^XT_PIXMAP_DEPTHS=.*/XT_PIXMAP_DEPTHS=16 1/" -e "s/^XT_DISPLAYMOTIONBUFFERSIZE=.*/XT_DISPLAYMOTIONBUFFERSIZE=0/" \
      -e "s/^XT_HEIGHT_MM=.*/XT_HEIGHT_MM=54/" -e "s/^XT_WIDTH_MM=.*/XT_WIDTH_MM=95/" \
      -e "s/^XT_SERVER_VENDOR=.*/XT_SERVER_VENDOR=Woodland Lab experimental X11 (ESP32-S3)/" \
      -e "s/^XT_VENDOR_RELEASE=.*/XT_VENDOR_RELEASE=1/" -e "s/^XT_EXTENSIONS=.*/XT_EXTENSIONS=No/" \
      -e "s#^XT_FONTPATH=.*#XT_FONTPATH=built-ins#" -e "s#^XT_FONTPATH_GOOD=.*#XT_FONTPATH_GOOD=built-ins#" \
      /opt/xvfb.cfg > /opt/woodland.cfg
  cfg=/opt/woodland.cfg
fi
export DISPLAY=$(sed -n "s/^DISPLAY=//p" $cfg)
sh xts5/bin/xts-run -c "$cfg" -o "$out" "$scen" > "$out/xts.log" 2>&1 || true
grep -oE ": [A-Z]+$" "$out/xts.log" | sort | uniq -c
