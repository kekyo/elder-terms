#!/bin/bash
set -eu
mkdir -p /run/dbus
if [ "$1" = peer ]; then
  /test-node /workspace/shared/tests/network/telnet-peer.ts &
fi
exec dbus-daemon --config-file=/workspace/shared/tests/network/dbus.conf --nofork
