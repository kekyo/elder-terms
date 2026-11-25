#!/bin/bash
# Drive two independently launched product terminals and their original shells.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
scratch=$(mktemp -d)
pids=()
finish() {
  if ((${#pids[@]})); then kill "${pids[@]}" 2>/dev/null || true; fi
  wait 2>/dev/null || true
  rm -rf "$scratch"
}
trap finish EXIT
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" GDK_BACKEND=x11
export GSETTINGS_BACKEND=memory
unset ELDER_TERMS_LAUNCH_ID ELDER_TERMS_LAUNCHER_BUS
KWIN_COMPOSE=N kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=20 org.kde.KWin
for tab in Alpha Beta; do
  cat >"$scratch/$tab.sh" <<SCRIPT
#!/bin/sh
echo \$\$ > "$scratch/$tab.pid"
echo 'Select this line before docking'
while IFS= read -r line; do
  case "\$line" in
    save-title) printf '\033]2;Saved title\007\033[22;2t\033]2;elder-terms\007';;
    pop-title) printf '\033[23;2t';;
    reset-title) printf '\033]2;elder-terms\007';;
  esac
  echo "\$\$:\$line" >> "$scratch/$tab.input"
done
SCRIPT
  chmod +x "$scratch/$tab.sh"
  cat >"$scratch/$tab.ini" <<SETTINGS
[general]
name=$tab
type=local
auto_close=false
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
  for ((attempt=0;attempt<150;attempt++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  cat "$scratch/"{Alpha,Beta}.log >&2
  return 1
}
ready() { [[ -s "$scratch/$1.pid" ]] && window "$1" >/dev/null; }
wait_until ready Alpha
wait_until ready Beta
alpha=$(window Alpha); beta=$(window Beta)
xdotool windowmove "$alpha" 40 100 windowmove "$beta" 950 100
origin() { xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs; }
read -r ax ay < <(origin "$alpha")
read -r bx by < <(origin "$beta")
drag() {
  xdotool mousemove "$1" "$2" mousedown 1
  for ((i=1;i<=16;i++)); do xdotool mousemove "$(($1+($3-$1)*i/16))" "$(($2+($4-$2)*i/16))"; sleep .025; done
  xdotool mouseup 1
}
# The connection icon starts a whole-window move. Drop on the terminal surface.
xdotool mousemove --window "$beta" 25 67 click --repeat 3 1
drag "$((bx+26))" "$((by+28))" "$((ax+200))" "$((ay+150))"
xdotool windowactivate --sync "$beta" type 'selection-kept'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/Beta.pid"):selection-kept" "$scratch/Beta.input"
xdotool mousemove --window "$beta" 25 67 click 1
xdotool windowactivate --sync "$beta" type 'save-title'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/Beta.pid"):save-title" "$scratch/Beta.input"
drag "$((bx+26))" "$((by+28))" "$((ax+200))" "$((ay+150))"
merged() { [[ -z $(window Alpha || true) && $(window Beta) == "$alpha" ]]; }
wait_until merged
xdotool windowactivate --sync "$alpha" type 'pop-title'
xdotool key Return
wait_until xdotool search --onlyvisible --name 'Saved title .* Beta$'
xdotool type 'reset-title'
xdotool key Return
wait_until merged
xdotool windowactivate --sync "$alpha" type 'docked'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/Beta.pid"):docked" "$scratch/Beta.input"
# Select a different live session before moving the second row onto the desktop.
xdotool mousemove "$((ax+50))" "$((ay+67))" click 1
wait_until window Alpha
xdotool type 'selected'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/Alpha.pid"):selected" "$scratch/Alpha.input"
drag "$((ax+50))" "$((ay+107))" 1550 800
wait_until ready Beta
wait_until ready Alpha
beta=$(window Beta)
xdotool windowmove "$beta" 950 100
read -r bx by < <(origin "$beta")
drag "$((bx+26))" "$((by+28))" "$((ax+200))" "$((ay+150))"
wait_until merged
xdotool windowactivate --sync "$alpha" key ctrl+shift+u
wait_until ready Alpha
wait_until ready Beta
xdotool windowactivate --sync "$(window Beta)" type 'separate'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/Beta.pid"):separate" "$scratch/Beta.input"
