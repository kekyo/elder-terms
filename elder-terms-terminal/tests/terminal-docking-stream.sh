#!/bin/bash
# Drop a connected terminal onto two local tabs and exercise the original peer.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 1800x1000x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
kind=$3
encoding=${4:-CP932}
scratch=$(mktemp -d)
pids=()
terminals=()
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
launch_local() {
  local tab=$1
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
  terminals+=($!)
  wait_for window "$tab" >/dev/null
}
launch_local Alpha
launch_local Beta
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
encoding=$encoding
width=60
height=20
INI
"$binary" -c "$scratch/Remote.ini" >"$scratch/Remote.terminal.log" 2>&1 & pids+=($!)
terminals+=($!)
wait_for window Remote >/dev/null
remote=$(window Remote)
xdotool windowmove --sync "$remote" 930 100
if [[ $kind == local ]]; then
  wait_for test -s "$scratch/tty"
  exec 3>"$(cat "$scratch/tty")"
fi
if [[ $encoding == ISO-2022-JP ]]; then
  # The second OSC title starts in JIS X 0208 and finishes after the drop.
  printf '\033]2;Ready\007\033]2;\033$B\106\174' >&3
elif [[ $kind == telnet ]]; then
  # Leave a TTYPE SEND subnegotiation open across the handoff.
  printf '\377\375\030\033]2;Ready\007\377\372\030\001' >&3
elif [[ $kind == local ]]; then
  printf '\033]2;Ready\007\033]2;\223' >&3
else
  printf '\033]2;Ready\007' >&3
fi
titled() { [[ $(xdotool getwindowname "$1") == "$2 | Remote" ]]; }
wait_for titled "$remote" Ready
mkfifo "$scratch/clipboard"
exec 4<>"$scratch/clipboard"
"$(dirname "$binary")/clipboard-write-helper" 日本 <"$scratch/clipboard" >"$scratch/clipboard.log" & pids+=($!)
wait_for grep -q READY "$scratch/clipboard.log"
if [[ $encoding == ISO-2022-JP ]]; then
  xdotool windowactivate --sync "$remote" key ctrl+shift+v
  wait_for env LC_ALL=C grep -aFq $'\033$BF|K\\' "$scratch/output"
fi
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
if [[ $encoding == ISO-2022-JP ]]; then
  printf '\113\134\033(B\007' >&3
elif [[ $kind == local ]]; then
  printf '\372\226\173\007' >&3
else
  printf '\033]2;\223\372\226\173\007' >&3
fi
wait_for titled "$alpha" 日本
xdotool windowactivate --sync "$alpha" key ctrl+shift+v
if [[ $encoding == ISO-2022-JP ]]; then
  # Each paste includes the text sender's final shift back to ASCII.
  wait_for env LC_ALL=C grep -aFq $'\033$BF|K\\\033(B\033$BF|K\\\033(B' "$scratch/output"
else
  wait_for env LC_ALL=C grep -aFq $'\223\372\226\173' "$scratch/output"
fi
xdotool windowactivate --sync "$alpha" type 'docked'
wait_for grep -aq docked "$scratch/output"

# Move the group again. The active tab still receives from and writes to the same peer.
launch_local Delta
delta=$(window Delta)
xdotool windowmove --sync "$delta" 930 100
dock "$alpha" "$delta"
wait_for titled "$delta" 日本
printf '\033]2;Moved twice\007' >&3
wait_for titled "$delta" 'Moved twice'
xdotool windowactivate --sync "$delta" type 'twice'
wait_for grep -aq dockedtwice "$scratch/output"

width=$(xwininfo -id "$delta" | awk '/Width:/{print $2}')
xdotool mousemove --window "$delta" "$((width-25))" 27 click 1
wait_for gone "$delta"
for pid in "${terminals[@]}"; do wait "$pid"; done
