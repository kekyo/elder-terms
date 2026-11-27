#!/bin/bash
# Drop a connected CP932 terminal onto two local tabs and exercise the original peer.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
kind=$3
scratch=$(mktemp -d)
pids=()
finish() {
  kill "${pids[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
  if (( $1 == 0 )); then rm -rf "$scratch"; else echo "Docking artifacts: $scratch" >&2; fi
}
trap 'finish $?' EXIT
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" GDK_BACKEND=x11
export GSETTINGS_BACKEND=memory GIO_USE_VFS=local G_DEBUG=fatal-criticals
unset ELDER_TERMS_LAUNCH_ID ELDER_TERMS_LAUNCHER_BUS
KWIN_COMPOSE=N kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=15 org.kde.KWin
wait_for() {
  for ((attempt=0;attempt<200;attempt++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  cat "$scratch"/*.terminal.log >&2
  return 1
}
window() { xdotool search --onlyvisible --name " $1$" | tail -1; }
gone() { ! xdotool getwindowname "$1" >/dev/null 2>&1; }
origin() { xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs; }
dock() {
  local sx sy tx ty
  read -r sx sy < <(origin "$1")
  read -r tx ty < <(origin "$2")
  sx=$((sx+72)); sy=$((sy+27)); tx=$((tx+240)); ty=$((ty+150))
  xdotool windowactivate --sync "$1" mousemove "$sx" "$sy" mousedown 1
  for ((i=1;i<=16;i++)); do
    xdotool mousemove "$((sx+(tx-sx)*i/16))" "$((sy+(ty-sy)*i/16))"
    sleep .025
  done
  xdotool mouseup 1
  wait_for gone "$1"
}
for tab in Alpha Beta; do
  cat > "$scratch/$tab.ini" <<INI
[general]
name=$tab
type=local
[local]
command_line=/bin/cat
[terminal]
width=60
height=20
INI
  "$binary" -c "$scratch/$tab.ini" >"$scratch/$tab.terminal.log" 2>&1 & pids+=($!)
  wait_for window "$tab" >/dev/null
done
alpha=$(window Alpha)
beta=$(window Beta)
xdotool windowmove --sync "$alpha" 60 100 windowmove --sync "$beta" 930 100
dock "$beta" "$alpha"
[[ $(window Beta) == "$alpha" ]]

mkfifo "$scratch/input"
exec 3<>"$scratch/input"
if [[ $kind == telnet ]]; then
  # One accept only: reconnecting cannot satisfy any assertion after the drop.
  socat -d -d TCP4-LISTEN:0,bind=127.0.0.1 STDIO <"$scratch/input" >"$scratch/output" 2>"$scratch/peer.log" & pids+=($!)
  wait_for grep -q 'listening on' "$scratch/peer.log"
  peer="address=127.0.0.1
port=$(sed -n '/listening on/s/.*://p' "$scratch/peer.log")"
elif [[ $kind == serial ]]; then
  socat PTY,raw,echo=0,wait-slave,link="$scratch/port" STDIO <"$scratch/input" >"$scratch/output" 2>"$scratch/peer.log" & pids+=($!)
  wait_for test -L "$scratch/port"
  peer="device=$scratch/port
baudrate=19200"
else
  cat > "$scratch/shell.sh" <<SHELL
#!/bin/sh
stty raw -echo
tty > "$scratch/tty"
exec cat > "$scratch/output"
SHELL
  chmod +x "$scratch/shell.sh"
  peer="command_line=$scratch/shell.sh"
fi
cat > "$scratch/Remote.ini" <<INI
[general]
name=Remote
type=$kind
auto_close=false
[$kind]
$peer
[terminal]
encoding=CP932
width=60
height=20
INI
"$binary" -c "$scratch/Remote.ini" >"$scratch/Remote.terminal.log" 2>&1 & pids+=($!)
wait_for window Remote >/dev/null
remote=$(window Remote)
xdotool windowmove --sync "$remote" 930 100
if [[ $kind == local ]]; then
  wait_for test -s "$scratch/tty"
  exec 3>"$(cat "$scratch/tty")"
fi
# Leave a TTYPE SEND subnegotiation open across the handoff.
if [[ $kind == telnet ]]; then
  printf '\377\375\030\033]2;Ready\007\377\372\030\001' >&3
else
  printf '\033]2;Ready\007' >&3
fi
titled() { [[ $(xdotool getwindowname "$1") == "$2 | Remote" ]]; }
wait_for titled "$remote" Ready
if [[ $kind == serial ]]; then
  # The open device remains attached even if the configured path disappears.
  mv "$scratch/port" "$scratch/original-port"
fi
dock "$remote" "$alpha"
wait_for titled "$alpha" Ready
if [[ $kind == telnet ]]; then
  printf '\377\360' >&3
  wait_for grep -aq 'xterm-256color' "$scratch/output"
fi
printf '\033]2;\223\372\226\173\007' >&3
wait_for titled "$alpha" 日本
xdotool windowactivate --sync "$alpha" type 'docked'
wait_for grep -aq docked "$scratch/output"
