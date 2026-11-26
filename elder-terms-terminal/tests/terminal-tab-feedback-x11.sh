#!/bin/bash
# Observe the actual GTK drag icon while an active or inactive tab crosses each drop area.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
scratch=$(mktemp -d)
pids=()
finish() {
  local status=$?
  if ((${#pids[@]})); then kill "${pids[@]}" 2>/dev/null || true; fi
  wait 2>/dev/null || true
  if ((status == 0)); then rm -rf "$scratch"
  else echo "Evidence retained at $scratch" >&2; fi
}
trap finish EXIT
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data"
export GDK_BACKEND=x11 GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1
unset ELDER_TERMS_LAUNCH_ID ELDER_TERMS_LAUNCHER_BUS
KWIN_COMPOSE=N kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=20 org.kde.KWin
start() {
  local name=$1
  cat >"$scratch/$name.sh" <<SCRIPT
#!/bin/sh
echo ready > "$scratch/$name.ready"
while read x; do echo "\$x" >> "$scratch/$name.input"; done
SCRIPT
  chmod +x "$scratch/$name.sh"
  cat >"$scratch/$name.ini" <<SETTINGS
[general]
name=$name
type=local
auto_close=false
[local]
command_line=$scratch/$name.sh
[terminal]
width=60
height=20
SETTINGS
  "$binary" -c "$scratch/$name.ini" >"$scratch/$name.log" 2>&1 & pids+=($!)
  wait_until window "$name" >/dev/null
  wait_until test -f "$scratch/$name.ready"
}
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
wait_until() {
  for ((attempt=0;attempt<150;attempt++)); do
    if "$@"; then return; fi
    sleep .05
  done
  echo "Timed out waiting for $*" >&2
  return 1
}
origin() { xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs; }
move() {
  local sx=$1 sy=$2 dx=$3 dy=$4
  for ((step=1;step<=16;step++)); do
    xdotool mousemove "$((sx+(dx-sx)*step/16))" "$((sy+(dy-sy)*step/16))"
    sleep .025
  done
}
capture() {
  local name=$1 x=$2 y=$3
  import -window root "$scratch/$name.png"
  printf '%s %s %s\n' "$name" "$x" "$y" >>"$scratch/points"
}

start Alpha
start Beta
alpha=$(window Alpha); beta=$(window Beta)
xdotool windowmove "$alpha" 40 100 windowmove "$beta" 950 100
read -r ax ay < <(origin "$alpha")
xdotool windowactivate --sync "$beta" type ready
xdotool key Return
wait_until grep -qx ready "$scratch/Beta.input"
read -r bx by < <(origin "$beta")
xdotool mousemove "$((bx+26))" "$((by+28))" mousedown 1
move "$((bx+26))" "$((by+28))" "$((ax+200))" "$((ay+150))"
sleep .6
xdotool mouseup 1
merged() { [[ $(window Beta || true) == "$alpha" ]]; }
wait_until merged

start Gamma
gamma=$(window Gamma)
xdotool windowmove "$gamma" 950 450
read -r cx cy < <(origin "$gamma")
xmessage -geometry 300x200+1400+100 'Other application' >"$scratch/foreign.log" 2>&1 & pids+=($!)
wait_until xdotool search --onlyvisible --name xmessage >/dev/null
sx=$((ax+50)); sy=$((ay+107))

# The active row starts with the detach badge; Escape leaves both tabs in place.
xdotool mousemove "$sx" "$sy" mousedown 1
move "$sx" "$sy" "$((sx+30))" "$sy"
capture active-row "$((sx+30))" "$sy"
xdotool key Escape
xdotool mouseup 1
test "$(window Beta)" = "$alpha"

# Select Alpha, then drag the inactive Beta row through the source surface,
# wallpaper, another terminal, a foreign application, and back to the terminal.
xdotool mousemove "$((ax+50))" "$((ay+67))" click 1
xdotool mousemove "$sx" "$sy" mousedown 1
move "$sx" "$sy" "$((sx+30))" "$sy"
capture inactive-row "$((sx+30))" "$sy"
move "$((sx+30))" "$sy" "$((ax+350))" "$((ay+280))"
capture source-terminal "$((ax+350))" "$((ay+280))"
move "$((ax+350))" "$((ay+280))" 1600 800
capture wallpaper 1600 800
move 1600 800 "$((cx+200))" "$((cy+150))"
capture dock "$((cx+200))" "$((cy+150))"
move "$((cx+200))" "$((cy+150))" 1500 200
capture foreign 1500 200
move 1500 200 "$((cx+200))" "$((cy+150))"
capture dock-again "$((cx+200))" "$((cy+150))"
xdotool mouseup 1
joined_gamma() { [[ $(window Beta || true) == "$gamma" ]]; }
wait_until joined_gamma

node - "$scratch" <<'NODE'
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { PNG } = require('pngjs');
const directory = process.argv[2];
const points = readFileSync(join(directory, 'points'), 'utf8').trim().split('\n');
for (const point of points) {
  const [name, sx, sy] = point.split(' ');
  const image = PNG.sync.read(readFileSync(join(directory, `${name}.png`)));
  const x = Number(sx), y = Number(sy);
  let undock = 0, dock = 0;
  for (let py = y + 14; py < y + 34; py++) for (let px = x + 14; px < x + 34; px++) {
    const index = (py * image.width + px) * 4;
    const r = image.data[index], g = image.data[index + 1], b = image.data[index + 2];
    if (b > g * 1.2 && g > r * 1.1 && r > 25) undock++;
    if (g > r * 2 && g > b * 1.15 && g > 60) dock++;
  }
  const expected = name.startsWith('dock') ? 'dock' : 'undock';
  if (expected === 'dock' ? dock < 30 || undock > 5 : undock < 30 || dock > 5)
    throw new Error(`${name}: expected ${expected} badge; pixels undock=${undock} dock=${dock}`);
}
NODE
