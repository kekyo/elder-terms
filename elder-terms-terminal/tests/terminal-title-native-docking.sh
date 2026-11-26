#!/bin/bash
# Exercise native title movement and docking with real X11 windows and PTYs.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
scratch=$(mktemp -d)
pids=()
recorders=()
succeeded=0
finish() {
  if ((${#recorders[@]})); then kill -INT "${recorders[@]}" 2>/dev/null || true; fi
  if ((${#pids[@]})); then kill "${pids[@]}" 2>/dev/null || true; fi
  wait 2>/dev/null || true
  if ((succeeded)); then rm -rf "$scratch"; else echo "Title docking artifacts: $scratch" >&2; fi
}
trap finish EXIT
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" GDK_BACKEND=x11
export GSETTINGS_BACKEND=memory G_DEBUG=fatal-criticals
unset ELDER_TERMS_LAUNCH_ID ELDER_TERMS_LAUNCHER_BUS
KWIN_COMPOSE=N kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=20 org.kde.KWin
wait_until() {
  for ((attempt=0;attempt<180;attempt++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  cat "$scratch/"{Alpha,Beta,Gamma}.log >&2 || true
  return 1
}
launch() {
  local name=$1 color=$2
  cat >"$scratch/$name.sh" <<SCRIPT
#!/bin/sh
echo \$\$ > "$scratch/$name.pid"
while IFS= read -r line; do
  printf '%s:%s\n' "\$\$" "\$line" >> "$scratch/$name.input"
done
SCRIPT
  chmod +x "$scratch/$name.sh"
  cat >"$scratch/$name.ini" <<SETTINGS
[general]
name=$name
type=local
auto_close=false
show_border=true
border_width=7
exterior_background=$color
[local]
command_line=$scratch/$name.sh
[terminal]
width=60
height=20
SETTINGS
  "$binary" -c "$scratch/$name.ini" >"$scratch/$name.log" 2>&1 & pids+=($!)
}
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
origin() { xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs; }
at_origin() { [[ $(origin "$1") == "$2 $3" ]]; }
alive() { [[ -s "$scratch/$1.pid" ]] && kill -0 "$(cat "$scratch/$1.pid")" 2>/dev/null; }
record() {
  ffmpeg -nostdin -loglevel error -y -f x11grab -video_size 1800x1000 -framerate 15 -i "$DISPLAY" \
    -c:v libx264 -threads 2 -preset ultrafast -pix_fmt yuv420p "$scratch/$1.mp4" \
    >"$scratch/$1.ffmpeg.log" 2>&1 & recorder=$!; recorders+=($recorder)
  sleep .3
}
finish_record() {
  sleep .3
  kill -INT "$recorder"
  wait "$recorder" || true
}
assert_video_move() {
  local label=$1 sample_x=$2 sample_y=$3
  ffmpeg -nostdin -loglevel error -y -i "$scratch/$label.mp4" \
    -vf "crop=2:2:$sample_x:$sample_y,scale=1:1" -pix_fmt rgb24 -f rawvideo "$scratch/$label.rgb"
  # Blue frame pixels must leave the starting position in the recorded frames.
  od -v -An -tu1 -w3 "$scratch/$label.rgb" | awk '
    NR == 1 { if ($1 < 20 || $1 > 85 || $2 < 70 || $2 > 140 || $3 < 125 || $3 > 205) exit 1 }
    { if ($1 > 90 || $2 > 160 || $3 < 90) moved = 1; frames++ }
    END { if (frames < 4 || !moved) exit 1 }
  '
}
launch Alpha '#A51D2D'
launch Beta '#2D6AA5'
wait_until alive Alpha
wait_until alive Beta
wait_until window Alpha
wait_until window Beta
alpha=$(window Alpha)
beta=$(window Beta)
xdotool windowmove --sync "$alpha" 40 100 windowmove --sync "$beta" 950 100
read -r ax ay < <(origin "$alpha")
read -r bx by < <(origin "$beta")
reset_beta() {
  xdotool windowmove --sync "$beta" 950 100
  wait_until at_origin "$beta" 950 100
  read -r bx by < <(origin "$beta")
}
begin_title_move() {
  local handle=$1 target_x=$2 target_y=$3 name=$4
  record "$name"
  xdotool mousemove "$((bx+handle))" "$((by+27))" mousedown 1
  for ((step=1;step<=16;step++)); do
    xdotool mousemove "$((bx+handle+(target_x-bx-handle)*step/16))" \
      "$((by+27+(target_y-by-27)*step/16))"
    sleep .025
  done
  sleep .15
  read -r moved_x moved_y < <(origin "$beta")
  if ((moved_x >= bx-80 && moved_x <= bx+80 && moved_y >= by-80 && moved_y <= by+80)); then
    echo "The real window did not follow the title $name while dragging" >&2
    return 1
  fi
  [[ $(xdotool getwindowname "$beta") == *elder-terms* ]]
  [[ $(window Alpha) == "$alpha" ]]
}
end_title_move() {
  xdotool mouseup 1
  finish_record
  assert_video_move "$1" "$((bx+7))" "$((by+200))"
}
begin_title_move 25 1300 500 icon
end_title_move icon
[[ $(window Beta) == "$beta" ]]
reset_beta
begin_title_move 115 1300 500 text
end_title_move text
[[ $(window Beta) == "$beta" ]]
reset_beta
begin_title_move 45 1300 500 whitespace
end_title_move whitespace
[[ $(window Beta) == "$beta" ]]
reset_beta

# Escape cancels docking even with the pointer over another terminal.
begin_title_move 115 "$((ax+200))" "$((ay+150))" cancelled
xdotool key Escape
end_title_move cancelled
[[ $(window Beta) == "$beta" ]]
reset_beta

# The foreign window is the topmost destination under the moved terminal.
xmessage -geometry 300x180+90+130 -title Foreign-Window 'Foreign window' >"$scratch/foreign.log" 2>&1 & pids+=($!)
wait_until xdotool search --onlyvisible --name '^Foreign-Window$'
foreign=$(xdotool search --onlyvisible --name '^Foreign-Window$' | tail -1)
xdotool windowraise "$foreign"
begin_title_move 115 "$((ax+200))" "$((ay+150))" foreign
end_title_move foreign
[[ $(window Beta) == "$beta" ]]
kill "${pids[-1]}"
reset_beta

# An entire tray should move after the target accepts a release.
launch Gamma '#2D6AA5'
wait_until alive Gamma
wait_until window Gamma
gamma=$(window Gamma)
xdotool windowmove --sync "$gamma" 950 500
read -r gx gy < <(origin "$gamma")
xdotool mousemove "$((gx+25))" "$((gy+27))" mousedown 1
for ((step=1;step<=16;step++)); do
  xdotool mousemove "$((gx+25+(bx+200-gx-25)*step/16))" \
    "$((gy+27+(by+150-gy-27)*step/16))"
  sleep .025
done
[[ $(window Gamma) == "$gamma" ]]
xdotool mouseup 1
grouped() { [[ $(window Gamma) == "$beta" ]]; }
wait_until grouped
reset_beta
begin_title_move 45 "$((ax+200))" "$((ay+150))" group
[[ $(window Gamma) == "$beta" ]]
end_title_move group
docked() { [[ $(window Gamma) == "$alpha" ]]; }
wait_until docked
for row in Alpha Beta Gamma; do
  case $row in Alpha) index=0;; Beta) index=1;; Gamma) index=2;; esac
  xdotool mousemove "$((ax+50))" "$((ay+67+40*index))" click 1
  xdotool windowactivate --sync "$alpha" key ctrl+u
  xdotool type "native-$row"
  xdotool key Return
  wait_until grep -qx "$(cat "$scratch/$row.pid"):native-$row" "$scratch/$row.input"
done
xdotool windowactivate --sync "$alpha" key alt+F4
for pid in "${pids[@]:1}"; do wait "$pid" || true; done
if grep -E 'Gtk-CRITICAL|Gtk-WARNING' "$scratch/"{Alpha,Beta,Gamma}.log; then exit 1; fi
succeeded=1
