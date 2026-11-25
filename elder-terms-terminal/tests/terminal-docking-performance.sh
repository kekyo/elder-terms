#!/bin/bash
# Measure one local or SSH shell before docking, while docked, and after undocking.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$2
mode=$3
port=$4
username=$5
client_key=$6
known_hosts=$7
scratch=$8
peer=$9
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
if [[ $mode == local ]]; then
  cat >"$scratch/Remote.ini" <<SETTINGS
[general]
name=Remote
type=local
auto_close=false
[local]
command_line=$peer
[terminal]
width=60
height=20
SETTINGS
else
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
fi
"$binary" -c "$scratch/Local.ini" >"$scratch/Local.log" 2>&1 & pids+=($!)
if [[ $mode == local ]]; then
  "$binary" -c "$scratch/Remote.ini" >"$scratch/Remote.log" 2>&1 & pids+=($!)
else
  "$binary" "--test-ssh-known-hosts-file=$known_hosts" -c "$scratch/Remote.ini" >"$scratch/Remote.log" 2>&1 & pids+=($!)
fi
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
wait_until() {
  for ((attempt=0;attempt<600;attempt++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  xdotool search --onlyvisible --name . getwindowname 2>&1 | tail -30 >&2 || true
  import -window root "$scratch/timeout.png" 2>/dev/null || true
  cat "$scratch/"{Local,Remote}.log >&2
  return 1
}
measured() { grep -q "\"phase\":\"$1\"" "$scratch/measurements.ndjson" 2>/dev/null; }
benchmark() {
  xdotool windowactivate --sync "$1"
  xdotool type --clearmodifiers "bench-$2"
  xdotool key Return
  wait_until measured "$2"
}
wait_until window Remote
if [[ $mode == ssh ]]; then
  xdotool windowactivate --sync "$(window Remote)"
  sleep .3
  xdotool key Return
fi
wait_until test -s "$scratch/ready"
wait_until window Local
local_window=$(window Local)
remote_window=$(window Remote)
benchmark "$remote_window" before
xdotool windowmove "$local_window" 40 100 windowmove "$remote_window" 950 100
origin() { xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs; }
read -r lx ly < <(origin "$local_window")
read -r rx ry < <(origin "$remote_window")
xdotool mousemove "$((rx+26))" "$((ry+28))" mousedown 1
for ((i=1;i<=16;i++)); do
  xdotool mousemove "$((rx+26+(lx+200-rx-26)*i/16))" "$((ry+28+(ly+150-ry-28)*i/16))"
  sleep .025
done
xdotool mouseup 1
merged() {
  ! xdotool getwindowname "$remote_window" >/dev/null 2>&1 &&
    xdotool getwindowname "$local_window" >/dev/null 2>&1
}
wait_until merged
benchmark "$local_window" docked
xdotool windowactivate --sync "$local_window" key ctrl+shift+u
undocked() {
  local candidate
  candidate=$(window Remote || true)
  [[ -n $candidate && $candidate != "$local_window" ]] &&
    xdotool getwindowname "$candidate" >/dev/null 2>&1
}
wait_until undocked
benchmark "$(window Remote)" undocked
