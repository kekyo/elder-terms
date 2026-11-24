#!/bin/bash
# Exercise real pointer and keyboard input inside an isolated X server with KWin.
set -euo pipefail
cd "$(dirname "$0")"
out="${DOCK_RESULTS:-$(realpath ../../../test-results)/dock-pilot/x11}"
mkdir -p "$out"
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1440x900x24' dbus-run-session -- bash "$0" --inside
fi
export GDK_BACKEND=x11
KWIN_COMPOSE=N kwin_x11 --replace >"$out/kwin.log" 2>&1 & wm=$!
gdbus wait --session --timeout=20 org.kde.KWin
build/dock-pilot >"$out/pilot.log" 2>&1 & pilot=$!
build/dock-peer >"$out/peer.log" 2>&1 & peer=$!
recorder=
cleanup() {
  if [[ -n $recorder ]]; then kill -INT "$recorder" 2>/dev/null || true; wait "$recorder" || true; fi
  kill "$pilot" "$peer" "$wm" 2>/dev/null || true
  wait "$pilot" "$peer" "$wm" 2>/dev/null || true
}
trap cleanup EXIT
log="$out/pilot.log"
from=1
expect() {
  for ((i=0; i<120; i++)); do
    if tail -n +"$from" "$log" | grep -E "$1" >/dev/null; then return; fi
    kill -0 "$pilot" 2>/dev/null || break
    sleep .05
  done
  echo "Missing /$1/ since line $from" >&2; tail -50 "$log" >&2; return 1
}
mark() { from=$(( $(wc -l <"$log") + 1 )); }
wid() { xdotool search --onlyvisible --name "^Dock pilot $1$" | tail -1; }
origin() { xwininfo -id "$(wid "$1")" | awk '/Absolute upper-left [XY]:/{print $NF}'; }
point() {
  local gx gy x y w h
  read -r gx gy < <(origin "$1" | xargs)
  read -r x y w h < <(awk -v id="$1" -v name="$2" '$1=="rect" && $2==id && $3==name {v=$4" "$5" "$6" "$7} END{print v}' "$log")
  echo "$((gx+x+w/2)) $((gy+y+h/2))"
}
click() { xdotool mousemove $(point "$1" "$2") click 1; }
drag_points() {
  local sx=$1 sy=$2 tx=$3 ty=$4
  xdotool mousemove "$sx" "$sy" mousedown 1
  for step in {1..16}; do
    xdotool mousemove "$((sx+(tx-sx)*step/16))" "$((sy+(ty-sy)*step/16))"
    sleep .025
  done
  sleep .1
  if [[ ${5:-} == cancel ]]; then xdotool key Escape; fi
  if [[ ${5:-} == title ]]; then
    # A successful GTK drop uses a drag icon while the source toplevel stays put.
    [[ "$(origin 1 | xargs)" == "$source_origin" ]]
    import -window root "$out/title-in-flight.png"
  fi
  xdotool mouseup 1
}
drag() {
  local sx sy
  read -r sx sy < <(point "$1" "$2")
  drag_points "$sx" "$sy" "$3" "$4" "${5:-}"
}
input() { xdotool type "$1"; xdotool key Return; }
expect '^backend GdkX11Display$'
expect '^state 1 tabs=AB active=A sidebar=1$'
expect '^state 2 tabs=C active=C sidebar=0$'
expect '^rect 2 content '
for id in 1 2; do timeout 10 xdotool search --sync --onlyvisible --name "^Dock pilot $id$" >/dev/null; done
xdotool windowmove "$(wid 1)" 40 100 windowmove "$(wid 2)" 760 140
ffmpeg -nostdin -loglevel error -y -f x11grab -video_size 1440x900 -framerate 15 -i "$DISPLAY" -c:v libx264 -threads 2 -preset ultrafast -pix_fmt yuv420p "$out/gestures.mp4" 2>"$out/ffmpeg.log" & recorder=$!
peerid=$(timeout 10 xdotool search --sync --onlyvisible --name '^Dock pilot peer$' | tail -1)
xdotool windowmove "$peerid" 900 570
read -r px py < <(origin peer | xargs)
mark
drag_points "$((px+55))" "$((py+45))" $(point 1 content)
expect '^file-drop 1 1$'
mark
drag 1 tabA "$((px+185))" "$((py+45))"
for ((i=0; i<100; i++)); do
  if grep -qx refused "$out/peer.log"; then break; fi
  sleep .05
done
grep -qx refused "$out/peer.log"
test -z "$(tail -n +"$from" "$log" | grep -E '^state 3 |^desktop-drop' || true)"
click 1 tabB
expect '^state 1 tabs=AB active=B sidebar=1$'
read -r cx cy < <(point 1 closeA)
capture_close() { import -window root -crop "18x16+$((cx-9))+$((cy-8))" "$1"; }
xdotool mousemove 690 400
capture_close "$out/close-hidden.png"
xdotool mousemove $(point 1 tabA)
# Wait for GTK's hover transition and require a contrasting glyph in its centre.
for ((i=0; i<45; i++)); do
  capture_close "$out/close-hover.png"
  if convert "$out/close-hover.png" -colorspace Gray -format '%[fx:maxima-minima > 0.32]' info: | grep -qx 1; then break; fi
  sleep .05
done
convert "$out/close-hover.png" -colorspace Gray -format '%[fx:maxima-minima > 0.32]' info: | grep -qx 1
convert "$out/close-hidden.png" -colorspace Gray -format '%[fx:maxima-minima < 0.12]' info: | grep -qx 1
input before
expect '^pty B ack-[0-9]+-before$'
pid=$(sed -nE 's/^pty B ack-([0-9]+)-before$/\1/p' "$log" | tail -1)
# Hidden B keeps consuming numbered output from the shell.
input burst
click 1 tabA
expect '^pty B tick-5 visible=0$'
for n in {1..5}; do test "$(grep -c "^pty B tick-$n " "$log")" = 1; done
mark
drag 1 tabA 1300 750 cancel
expect '^drag-failed .*'
test -z "$(tail -n +"$from" "$log" | grep '^state 3 ' || true)"
mark
drag 1 tabA $(point 1 content)
expect '^drop self '
mark
drag 1 tabB $(point 2 content)
expect '^state 1 tabs=A active=A sidebar=0$'
expect '^state 2 tabs=CB active=B sidebar=1$'
input after
expect "^pty B ack-$pid-after$"
mark
xdotool key ctrl+shift+u
expect '^state 3 tabs=B active=B sidebar=0$'
expect '^state 2 tabs=C active=C sidebar=0$'
xdotool windowmove "$(wid 3)" 320 520
mark
drag 3 title $(point 1 content)
expect '^state 1 tabs=AB active=B sidebar=1$'
expect '^closed 3$'
# A native move must move the real toplevel even when released over the peer.
xdotool windowactivate --sync "$(wid 1)"
read -r before_x before_y < <(origin 1 | xargs)
mark
drag 1 native $(point 2 content)
read -r after_x after_y < <(origin 1 | xargs)
(( after_x != before_x || after_y != before_y ))
test -z "$(tail -n +"$from" "$log" | grep -E '^closed|^drop ' || true)"
xdotool windowmove "$(wid 1)" 40 100
mark
source_origin=$(origin 1 | xargs)
drag 1 title $(point 2 content) title
expect '^state 2 tabs=CAB active=B sidebar=1$'
expect '^closed 1$'
mark
drag 2 tabA 1350 780
expect '^state 4 tabs=A active=A sidebar=0$'
expect '^state 2 tabs=CB active=B sidebar=1$'
xdotool windowmove "$(wid 4)" 30 420
mark
drag 4 title $(point 2 content)
expect '^state 2 tabs=CBA active=A sidebar=1$'
mark
xdotool windowsize "$(wid 2)" 580 425
expect '^grid A '
read -r rows cols < <(awk '/^grid A / {size=$3" "$4} END{print size}' "$log")
input size
expect "^pty A size-$rows $cols$"
mark
xdotool windowsize "$(wid 2)" 420 300
expect '^grid A '
read -r smaller_rows smaller_cols < <(awk '/^grid A / {size=$3" "$4} END{print size}' "$log")
(( smaller_rows < rows && smaller_cols < cols ))
input size
expect "^pty A size-$smaller_rows $smaller_cols$"
test -z "$(tail -n +"$from" "$log" | grep -E '^grid [BC] ' || true)"
click 2 closeB
expect '^state 2 tabs=CA active=A sidebar=1$'
if kill -0 "$pid" 2>/dev/null; then echo "Closed tab still owns shell $pid" >&2; exit 1; fi
click 2 closeA
expect '^state 2 tabs=C active=C sidebar=0$'
xdotool windowactivate --sync "$(wid 2)" key alt+F4
expect '^closed 2$'
wait "$pilot"
if grep -Eq 'CRITICAL|WARNING|ERROR' "$log"; then cat "$log" >&2; exit 1; fi
echo "PASS X11 docking gestures, shell PID $pid, hidden output, resizing and close"
