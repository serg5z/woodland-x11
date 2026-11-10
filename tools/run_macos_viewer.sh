#!/bin/sh
# One-command local launch. Close the native window to stop its private host server.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
make -s -C "$here" build/x11_host_server build/macos_viewer
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/woodland-viewer.XXXXXX")
display_number=${WOODLAND_VIEWER_DISPLAY:-196}
server_pid=
cleanup() {
  if [ -n "$server_pid" ]; then kill -TERM "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; fi
  rm -f "$run_dir/viewer.sock"
  rmdir "$run_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
"$here/build/x11_host_server" -d "$display_number" --viewer-socket "$run_dir/viewer.sock" --clients 8 -q &
server_pid=$!
sleep 0.2
if ! kill -0 "$server_pid" 2>/dev/null; then wait "$server_pid"; exit 1; fi
printf 'Woodland X11: DISPLAY=127.0.0.1:%s.0\n' "$display_number"
printf 'Linux VM tunnel: ssh -R %s:127.0.0.1:%s MAC_HOST\n' "$((6000 + display_number))" "$((6000 + display_number))"
"$here/build/macos_viewer" "$run_dir/viewer.sock"
