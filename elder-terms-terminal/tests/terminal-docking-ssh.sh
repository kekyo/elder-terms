#!/bin/bash
# Check that a live SSH shell survives product-window docking and undocking.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$2
port=$3
username=$4
client_key=$5
known_hosts=$6
scratch=$7
pids=()
finish() {
  if ((${#pids[@]})); then kill "${pids[@]}" 2>/dev/null || true; fi
  wait 2>/dev/null || true
}
trap finish EXIT
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" GDK_BACKEND=x11
export GSETTINGS_BACKEND=memory
KWIN_COMPOSE=N kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=20 org.kde.KWin
cat >"$scratch/Local.ini" <<SETTINGS
[general]
name=Local
type=local
auto_close=false
[local]
command_line=/bin/sh
[terminal]
width=60
height=20
SETTINGS
cat >"$scratch/Remote.ini" <<SETTINGS
[general]
name=Remote
type=ssh
auto_close=false
[ssh]
address=127.0.0.1
port=$port
username=$username
identity_file=$client_key
[terminal]
width=60
height=20
SETTINGS
"$binary" -c "$scratch/Local.ini" >"$scratch/Local.log" 2>&1 & pids+=($!)
"$binary" --test-fixture "--test-ssh-known-hosts-file=$known_hosts" -c "$scratch/Remote.ini" >"$scratch/Remote.log" 2>&1 & pids+=($!)
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
wait_until() {
  for ((attempt=0;attempt<200;attempt++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  cat "$scratch/"{Local,Remote}.log >&2
  return 1
}
ready() { [[ -s "$scratch/shell.pid" ]] && window "$1" >/dev/null; }
wait_until ready Remote
wait_until window Local
local_window=$(window Local)
remote_window=$(window Remote)
xdotool windowmove "$local_window" 40 100 windowmove "$remote_window" 950 100
origin() { xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs; }
read -r lx ly < <(origin "$local_window")
read -r rx ry < <(origin "$remote_window")
drag() {
  xdotool mousemove "$1" "$2" mousedown 1
  for ((i=1;i<=16;i++)); do xdotool mousemove "$(($1+($3-$1)*i/16))" "$(($2+($4-$2)*i/16))"; sleep .025; done
  xdotool mouseup 1
}
drag "$((rx+26))" "$((ry+28))" "$((lx+200))" "$((ly+150))"
merged() { [[ -z $(window Remote || true) && $(window Local) == "$local_window" ]]; }
wait_until merged
xdotool windowactivate --sync "$local_window" type 'docked-ssh'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/shell.pid"):docked-ssh" "$scratch/shell.input"
xdotool key ctrl+shift+u
wait_until ready Remote
xdotool windowactivate --sync "$(window Remote)" type 'undocked-ssh'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/shell.pid"):undocked-ssh" "$scratch/shell.input"
