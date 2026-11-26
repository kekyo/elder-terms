#!/bin/bash
# Inspect KWin's actual blur region as a live terminal changes windows.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 2000x1050x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
scratch=$(mktemp -d)
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" GDK_BACKEND=x11
export LIBGL_ALWAYS_SOFTWARE=1 GSETTINGS_BACKEND=memory G_DEBUG=fatal-criticals
unset ELDER_TERMS_LAUNCH_ID ELDER_TERMS_LAUNCHER_BUS
mkdir -p "$XDG_CONFIG_HOME"
pids=()
finish() {
  kill "${pids[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
  if [[ $1 != 0 ]]; then cat "$scratch/"{Solid,Blur,wm}.log >&2; fi
  rm -rf "$scratch"
}
trap 'finish $?' EXIT
cat >"$XDG_CONFIG_HOME/kwinrc" <<'CONFIG'
[Compositing]
Enabled=true
[Plugins]
blurEnabled=true
CONFIG
KWIN_COMPOSE=O2 kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=20 org.kde.KWin
wait_until() {
  for ((i=0;i<160;i++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  return 1
}
property=_KDE_NET_WM_BLUR_BEHIND_REGION
supports_blur() { [[ $(xprop -root "$property") == *' = '* ]]; }
wait_until supports_blur
for tab in Solid Blur; do
  mode=opaque
  if [[ $tab == Blur ]]; then mode=blur; fi
  cat >"$scratch/$tab.ini" <<CONFIG
[general]
name=$tab
type=local
auto_close=false
exterior_background=#A51D2D
[local]
command_line=/bin/sh
[terminal]
width=60
height=20
background_mode=$mode
CONFIG
  "$binary" -c "$scratch/$tab.ini" >"$scratch/$tab.log" 2>&1 & pids+=($!)
done
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
wait_until window Solid
wait_until window Blur
solid=$(window Solid)
blur=$(window Blur)
# KWin includes the CSD shadow in the X11 window's coordinates.
content_origin() {
  local id=$1 left right top bottom x y
  read -r x y < <(xwininfo -id "$id" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs)
  read -r left right top bottom < <(xprop -id "$id" _GTK_FRAME_EXTENTS | sed -n 's/.* = //p' | tr -d ',')
  echo "$((x+left)) $((y+top))"
}
region() { xprop -id "$1" "$property" | sed -n 's/.* = //p' | tr -d ','; }
# Region coordinates are relative to the whole native window. Only the grouped
# blur page starts after the tab list. Opaque pages withdraw the request.
has_region() {
  local x y w h
  read -r x y w h <<<"$(region "$1")"
  [[ -n $h && $y -gt 0 && $w -gt 0 && $h -gt 0 && $x -ge $2 && $x -lt $3 ]]
}
no_region() { [[ -z $(region "$1") ]]; }
wait_until has_region "$blur" 0 150
wait_until no_region "$solid"
xdotool windowmove --sync "$solid" 40 100 windowmove --sync "$blur" 980 100
read -r sx sy < <(content_origin "$solid")
read -r bx by < <(content_origin "$blur")
xdotool mousemove "$((bx+26))" "$((by+27))" mousedown 1
for ((j=1;j<=16;j++)); do
  xdotool mousemove "$((bx+26+(sx+220-bx-26)*j/16))" "$((by+27+(sy+150-by-27)*j/16))"
  sleep .025
done
xdotool mouseup 1
merged() { [[ $(window Blur) == "$solid" ]]; }
wait_until merged
wait_until has_region "$solid" 150 400
read -r _ _ old_width _ <<<"$(region "$solid")"
xdotool windowsize --sync "$solid" 900 550
region_resized() { has_region "$solid" 150 400 && [[ $(region "$solid" | cut -d' ' -f3) != "$old_width" ]]; }
wait_until region_resized
xdotool mousemove "$((sx+50))" "$((sy+67))" click 1
wait_until no_region "$solid"
xdotool mousemove "$((sx+50))" "$((sy+107))" click 1
wait_until has_region "$solid" 150 400
xdotool key ctrl+shift+u
split() { separate=$(window Blur) && [[ $separate != "$solid" ]]; }
wait_until split
wait_until has_region "$separate" 0 150
wait_until no_region "$solid"
xdotool windowactivate --sync "$separate" key alt+F4
closed() { ! xdotool getwindowname "$separate" >/dev/null 2>&1; }
wait_until closed
xdotool windowactivate --sync "$solid" type "echo alive > '$scratch/remaining'"
xdotool key Return
wait_until test -s "$scratch/remaining"
xdotool windowactivate --sync "$solid" key alt+F4
for pid in "${pids[@]:1}"; do wait "$pid"; done
if grep -E 'Failed to (restore terminal background|resize terminal background effect)|Gtk-CRITICAL' "$scratch/"{Solid,Blur}.log; then exit 1; fi
