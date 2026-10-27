#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
probe="$root/.build/latency/latency-probe.so"
runtime="$root/.build/elder-terms-terminal/terminal-render-work-test"
report=$(mktemp)
trap 'rm -f "$report"' EXIT
xvfb-run -a env GDK_GL=disable GTK_IM_MODULE=gtk-im-context-simple NO_AT_BRIDGE=1 \
  ELDER_LATENCY_TRACE="$report" LD_PRELOAD="$probe" "$runtime"
node --input-type=module - "$report" <<'NODE'
import { readFile } from 'node:fs/promises';
const lines = (await readFile(process.argv[2], 'utf8')).trim().split('\n');
if (!lines[0].startsWith('kind\tstart_ns\tend_ns\t')) throw new Error('No latency trace');
const rows = lines.slice(1).map(line => line.split('\t'));
for (const kind of ['feed', 'frame', 'recognize', 'model', 'composite']) {
  const matching = rows.filter(row => row[0] === kind);
  if (matching.length === 0) throw new Error(`No ${kind} observations`);
  for (const row of matching) {
    if (!(BigInt(row[2]) >= BigInt(row[1]))) throw new Error(`Reversed ${kind} interval`);
    if (BigInt(row[3]) < 0n) throw new Error(`Negative CPU time in ${kind}`);
  }
}
console.log('Observed real input, rendering, and recognition without changing runtime behavior.');
NODE
