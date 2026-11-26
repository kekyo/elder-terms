#!/bin/bash
# Check whole-tray migration and closing individual tabs in the product UI.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
scratch=$(mktemp -d)
pids=()
succeeded=0
finish() {
  if ((${#pids[@]})); then kill "${pids[@]}" 2>/dev/null || true; fi
  wait 2>/dev/null || true
  if ((succeeded)); then rm -rf "$scratch"; else echo "Group docking artifacts: $scratch" >&2; fi
}
trap finish EXIT
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" GDK_BACKEND=x11
export GSETTINGS_BACKEND=memory
unset ELDER_TERMS_LAUNCH_ID ELDER_TERMS_LAUNCHER_BUS
KWIN_COMPOSE=N kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=20 org.kde.KWin
for tab in Alpha Beta Gamma Delta; do
  cat >"$scratch/$tab.sh" <<SCRIPT
#!/bin/sh
echo \$\$ > "$scratch/$tab.pid"
while IFS= read -r line; do
  printf '%s:%s\n' "\$\$" "\$line" >> "$scratch/$tab.input"
done
SCRIPT
  chmod +x "$scratch/$tab.sh"
  cat >"$scratch/$tab.ini" <<SETTINGS
[general]
name=$tab
type=local
auto_close=false
${ELDER_TERMS_GROUP_COLOR_CLOSE:+exterior_background=#A51D2D}
[local]
command_line=$scratch/$tab.sh
[terminal]
width=60
height=20
SETTINGS
  "$binary" -c "$scratch/$tab.ini" >"$scratch/$tab.log" 2>&1 & pids+=($!)
done
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
wait_until() {
  for ((attempt=0;attempt<200;attempt++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  xdotool search --onlyvisible --name . getwindowname 2>&1 | tail -30 >&2 || true
  import -window root "$scratch/timeout.png" 2>/dev/null || true
  for tab in Alpha Beta Gamma Delta; do
    if [[ -s "$scratch/$tab.pid" ]]; then ps -o pid,ppid,stat,cmd -p "$(cat "$scratch/$tab.pid")" >&2 || true; fi
  done
  cat "$scratch/"{Alpha,Beta,Gamma,Delta}.log >&2
  return 1
}
ready() { [[ -s "$scratch/$1.pid" ]] && window "$1" >/dev/null; }
gone() { ! xdotool getwindowname "$1" >/dev/null 2>&1; }
shell_gone() { ! kill -0 "$(cat "$scratch/$1.pid")" 2>/dev/null; }
selected() { [[ $(window "$1" || true) == "$delta" ]]; }
for tab in Alpha Beta Gamma Delta; do wait_until ready "$tab"; done
alpha=$(window Alpha)
beta=$(window Beta)
gamma=$(window Gamma)
delta=$(window Delta)
xdotool windowmove "$alpha" 40 100 windowmove "$beta" 950 100
xdotool windowmove "$gamma" 950 500 windowmove "$delta" 40 500
origin() { xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs; }
drag() {
  xdotool mousemove "$1" "$2" mousedown 1
  for ((i=1;i<=16;i++)); do
    xdotool mousemove "$(($1+($3-$1)*i/16))" "$(($2+($4-$2)*i/16))"
    sleep .025
  done
  xdotool mouseup 1
}
dock_from_to() {
  local sx sy tx ty
  read -r sx sy < <(origin "$1")
  read -r tx ty < <(origin "$2")
  drag "$((sx+26))" "$((sy+28))" "$((tx+200))" "$((ty+150))"
  wait_until gone "$1"
}
dock_from_to "$beta" "$alpha"
dock_from_to "$gamma" "$alpha"
# The titlebar handle offers all three pages as one transfer.
dock_from_to "$alpha" "$delta"
read -r dx dy < <(origin "$delta")
row_y() { echo "$((dy+67+40*$1))"; }
select_row() { xdotool mousemove "$((dx+55))" "$(row_y "$1")" click 1; }
close_row() {
  xdotool mousemove "$((dx+205))" "$(row_y "$1")" click 1
}
send_to() {
  local tab=$1
  xdotool windowactivate --sync "$delta"
  xdotool type --clearmodifiers "group-$tab"
  xdotool key Return
  wait_until grep -qx "$(cat "$scratch/$tab.pid"):group-$tab" "$scratch/$tab.input"
}
# The moved pages and the original destination all retain their shell process.
for row in 0 1 2 3; do
  case "$row" in
    0) tab=Delta;; 1) tab=Alpha;; 2) tab=Beta;; 3) tab=Gamma;;
  esac
  select_row "$row"
  wait_until selected "$tab"
  send_to "$tab"
done
# Gamma remains selected while the Beta row is closed.
close_row 2
wait_until shell_gone Beta
wait_until selected Gamma
send_to Gamma
# Close another non-selected page, then the final visible row close button.
close_row 1
wait_until shell_gone Alpha
send_to Gamma
close_row 0
wait_until shell_gone Delta
wait_until selected Gamma
send_to Gamma
# A single remaining terminal has no tab list and closes with its window.
xdotool windowactivate --sync "$delta"
width=$(xwininfo -id "$delta" | awk '/Width:/{print $2}')
xdotool mousemove --window "$delta" "$((width-25))" 27 click 1
wait_until shell_gone Gamma
wait_until gone "$delta"
if [[ -n ${ELDER_TERMS_GROUP_COLOR_CLOSE:-} ]]; then
  # Session disposal runs after GTK has removed the final window.
  for pid in "${pids[@]:1}"; do wait "$pid"; done
  if grep -nE 'Gtk-CRITICAL|GTK_IS_WIDGET' "$scratch/"{Alpha,Beta,Gamma,Delta}.log; then
    echo "A colored terminal used a destroyed widget during close" >&2
    exit 1
  fi
fi
succeeded=1
