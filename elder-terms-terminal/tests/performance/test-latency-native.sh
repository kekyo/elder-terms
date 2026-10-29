#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
directory=$(mktemp -d)
trap 'rm -rf -- "$directory"' EXIT
xvfb-run -a node "$root/elder-terms-terminal/tests/performance/run-latency.mts" \
  "$directory/run" 0 plain idle 3 0 cairo detail
node "$root/elder-terms-terminal/tests/performance/analyze-latency.mts" "$directory/run"
