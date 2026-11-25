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
dbus-monitor --session "interface='net.kekyo.ElderTerms.SftpOrigin'" "member='GetNameOwner'" "type='error'" >"$scratch/sftp-bus.log" 2>&1 & pids+=($!)
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
"$binary" "--test-ssh-known-hosts-file=$known_hosts" -c "$scratch/Remote.ini" >"$scratch/Remote.log" 2>&1 & pids+=($!)
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
wait_until() {
  for ((attempt=0;attempt<200;attempt++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  xdotool search --onlyvisible --name . getwindowname 2>&1 | tail -30 >&2 || true
  remote_id=$(window Remote || true)
  if [[ -n $remote_id ]]; then xdotool windowactivate --sync "$remote_id" || true; fi
  import -window root "$scratch/timeout.png" 2>/dev/null || true
  cat "$scratch/"{Local,Remote}.log >&2
  return 1
}
ready() { [[ -s "$scratch/shell.pid" ]] && window "$1" >/dev/null; }
wait_until window Remote
xdotool windowactivate --sync "$(window Remote)"
sleep .3
xdotool key Return
wait_until ready Remote
wait_until window Local
local_window=$(window Local)
remote_window=$(window Remote)
open_sftp_menu() {
  local width
  width=$(xwininfo -id "$1" | awk '/Width:/{print $2}')
  xdotool mousemove --window "$1" "$((width-205))" 27 click 1
  xdotool mousemove --window "$1" "$((width-150))" 108 click 1
}
open_sftp_menu "$remote_window"
sftp_window() { xdotool search --onlyvisible --name 'Remote .* SFTP' | tail -1; }
wait_until sftp_window
sftp_id=$(sftp_window)
sftp_active() { [[ $(xdotool getactivewindow) = "$sftp_id" ]]; }
mkdir -p "$scratch/download"
xdotool windowactivate --sync "$sftp_id"
sleep .5
xdotool mousemove --window "$sftp_id" 120 110 click 1 key ctrl+a
xdotool type --clearmodifiers "$scratch/download"
xdotool key Return
sleep .3
xdotool mousemove --window "$sftp_id" 650 110 click 1 key ctrl+a
xdotool type --clearmodifiers "$scratch/remote"
xdotool key Return
sleep .3
receive_shared() {
  xdotool windowactivate --sync "$sftp_id"
  xdotool mousemove --window "$sftp_id" 650 174 click 3
  xdotool mousemove --window "$sftp_id" 705 193 click 1
  wait_until cmp -s "$scratch/remote/shared.txt" "$scratch/download/shared.txt"
}
receive_shared
show_count_at_least() {
  local count
  count=$(grep -c 'member=Show' "$scratch/sftp-bus.log" || true)
  ((count >= $1))
}
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
merged() {
  ! xdotool getwindowname "$remote_window" >/dev/null 2>&1 &&
    xdotool getwindowname "$local_window" >/dev/null 2>&1
}
wait_until merged
xdotool windowactivate --sync "$local_window"
open_sftp_menu "$local_window"
wait_until show_count_at_least 1
wait_until sftp_active
test "$(sftp_window)" = "$sftp_id"
rm "$scratch/download/shared.txt"
receive_shared
xdotool windowactivate --sync "$local_window" type 'docked-ssh'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/shell.pid"):docked-ssh" "$scratch/shell.input"
xdotool key ctrl+shift+u
undocked() {
  undocked_window=$(window Remote) && [[ -n $undocked_window && $undocked_window != "$local_window" ]]
}
wait_until undocked
xdotool windowactivate --sync "$undocked_window"
open_sftp_menu "$undocked_window"
wait_until show_count_at_least 2
wait_until sftp_active
test "$(sftp_window)" = "$sftp_id"
! grep -Eq 'NotTabOwner|ShowFailed' "$scratch/sftp-bus.log"
rm "$scratch/download/shared.txt"
receive_shared
xdotool windowactivate --sync "$(window Remote)" type 'undocked-ssh'
xdotool key Return
wait_until grep -qx "$(cat "$scratch/shell.pid"):undocked-ssh" "$scratch/shell.input"
