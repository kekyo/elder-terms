// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

import { spawn } from 'node:child_process';
import { chmod, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startSshLatencyFixture } from '../../shared/tests/ssh-latency-fixture.mts';

interface Measurements {
  phase: string;
  pid: number;
  dsrMicroseconds: number[];
  bulkMicroseconds: number[];
}

const binary = fileURLToPath(
  new URL(
    '../../.build/elder-terms-terminal/elder-terms-terminal',
    import.meta.url
  )
);
const script = fileURLToPath(
  new URL('./terminal-docking-performance.sh', import.meta.url)
);
const mode = process.argv[2] === '--local' ? 'local' : 'ssh';
const directory = await mkdtemp(join(tmpdir(), 'elder-terms-dock-perf-'));
const peer = join(directory, 'peer.sh');
const peerJs = join(directory, 'peer.mjs');
const metrics = join(directory, 'measurements.ndjson');
const quote = (value: string): string => `'${value.split("'").join("'\\''")}'`;
let fixture: Awaited<ReturnType<typeof startSshLatencyFixture>> | undefined;
let failed = false;
try {
  await writeFile(
    peer,
    `#!/bin/sh\nstty raw -echo\nexec node ${quote(peerJs)}\n`
  );
  await chmod(peer, 0o700);
  await writeFile(
    peerJs,
    `import { appendFileSync, readSync, writeFileSync, writeSync } from 'node:fs';
const output = ${JSON.stringify(metrics)};
const input = Buffer.alloc(1);
const payload = Buffer.alloc(65536, 0x58);
const readByte = () => {
  if (readSync(0, input, 0, 1, null) !== 1) process.exit(0);
  return input[0];
};
const writeAll = (bytes) => {
  for (let offset = 0; offset < bytes.length;)
    offset += writeSync(1, bytes, offset, bytes.length - offset);
};
const dsr = () => {
  writeSync(1, '\\x1b[H\\x1b[6n');
  let response = '';
  while (!response.endsWith('R')) response += String.fromCharCode(readByte());
  if (!/^\\x1b\\[\\d+;\\d+R$/.test(response)) throw new Error('Invalid DSR: ' + JSON.stringify(response));
};
writeFileSync(${JSON.stringify(join(directory, 'ready'))}, String(process.pid));
for (;;) {
  let command = '';
  for (let byte = readByte(); byte !== 13 && byte !== 10; byte = readByte())
    command += String.fromCharCode(byte);
  if (!command.startsWith('bench-')) continue;
  const measure = (action) => {
    const start = process.hrtime.bigint();
    action();
    return Number(process.hrtime.bigint() - start) / 1000;
  };
  const dsrMicroseconds = [];
  const bulkMicroseconds = [];
  // Keep the rendered area stable so the bulk sample measures transport and parsing.
  writeSync(1, '\\x1b[?7l');
  dsr();
  for (let round = 0; round < 28; round++) dsrMicroseconds.push(measure(dsr));
  for (let round = 0; round < 12; round++)
    bulkMicroseconds.push(measure(() => { writeAll(payload); dsr(); }));
  appendFileSync(output, JSON.stringify({ phase: command.slice(6), pid: process.pid,
    dsrMicroseconds, bulkMicroseconds }) + '\\n');
}
`
  );
  if (mode === 'ssh') fixture = await startSshLatencyFixture(directory, [peer]);
  const child = spawn(
    'bash',
    [
      script,
      binary,
      mode,
      String(fixture?.port ?? 0),
      fixture?.username ?? '',
      fixture?.clientKey ?? '',
      fixture?.knownHosts ?? '',
      directory,
      peer,
    ],
    { stdio: 'inherit' }
  );
  const status = await new Promise<number>((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', (code, signal) => resolve(code ?? (signal ? 1 : 0)));
  });
  if (status !== 0) {
    failed = true;
    process.exitCode = status;
  } else {
    const rows = (await readFile(metrics, 'utf8'))
      .trim()
      .split('\n')
      .map((line) => JSON.parse(line) as Measurements);
    if (
      rows.length !== 3 ||
      rows.some((row) => row.pid !== rows[0].pid) ||
      rows.map((row) => row.phase).join(',') !== 'before,docked,undocked'
    ) {
      throw new Error(
        'Terminal peer did not survive the three measurement phases'
      );
    }
    const median = (values: number[]): number => {
      const sorted = [...values].sort((left, right) => left - right);
      return (
        (sorted[(sorted.length - 1) >> 1] + sorted[sorted.length >> 1]) / 2
      );
    };
    const baselineDsr = median(rows[0].dsrMicroseconds.slice(4));
    const baselineBulk = median(rows[0].bulkMicroseconds.slice(1));
    console.log(`Same ${mode} shell PID:`, rows[0].pid);
    console.log(
      '64 KiB samples disable terminal line wrapping. Warm-up samples are excluded.'
    );
    console.log(
      'Phase        DSR median (ms)   Relative   64 KiB + DSR median (ms)   Relative'
    );
    for (const row of rows) {
      const dsr = median(row.dsrMicroseconds.slice(4));
      const bulk = median(row.bulkMicroseconds.slice(1));
      console.log(
        `${row.phase.padEnd(12)} ${(dsr / 1000).toFixed(2).padStart(9)}          ${(dsr / baselineDsr).toFixed(2).padStart(5)}x` +
          `                 ${(bulk / 1000).toFixed(2).padStart(9)}           ${(bulk / baselineBulk).toFixed(2).padStart(5)}x`
      );
    }
  }
} catch (error) {
  failed = true;
  throw error;
} finally {
  if (fixture) await fixture.stop();
  if (failed || process.env.ELDER_DOCK_KEEP_ARTIFACTS)
    console.error(`Docking performance artifacts: ${directory}`);
  else await rm(directory, { recursive: true, force: true });
}
