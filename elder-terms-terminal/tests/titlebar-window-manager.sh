#!/bin/bash
# Run the lifecycle test with an isolated real X11 window manager.
set -eu
wm_log=$(mktemp)
KWIN_COMPOSE=N kwin_x11 --replace >"$wm_log" 2>&1 &
wm_pid=$!
trap 'kill "$wm_pid" 2>/dev/null || true; wait "$wm_pid" 2>/dev/null || true; rm -f "$wm_log"' EXIT
if ! gdbus wait --session --timeout=20 org.kde.KWin; then
  cat "$wm_log"
  exit 1
fi
"$@" --window-manager
