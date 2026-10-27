// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

import { spawn, execFile } from 'node:child_process';
import { closeSync, openSync } from 'node:fs';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { setTimeout as delay } from 'node:timers/promises';

const exec = promisify(execFile);
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const [
  destination,
  history = '0',
  mode = 'plain',
  load = 'idle',
  count = '20',
  stopAfter = '0',
  renderer = 'cairo',
  tracing = 'detail',
  idleMs = '0',
  links = 'on',
  capture = 'video',
] = process.argv.slice(2);
if (
  !destination ||
  !['plain', 'url', 'wrapped'].includes(mode) ||
  !['idle', 'append', 'overwrite'].includes(load) ||
  !['cairo', 'opengl'].includes(renderer) ||
  !['detail', 'light', 'off'].includes(tracing) ||
  !['on', 'off'].includes(links) ||
  !['none', 'video', 'full'].includes(capture)
) {
  throw new Error(
    'Usage: run-latency OUTPUT [HISTORY MODE LOAD COUNT STOP_AFTER RENDERER detail|light|off IDLE_MS on|off none|video|full]'
  );
}
for (const number of [history, count, stopAfter, idleMs]) {
  if (!/^\d+$/.test(number)) throw new Error(`Invalid integer: ${number}`);
}
const displayInteger = (name: string, fallback: number): number => {
  const value = process.env[name];
  if (value === undefined) return fallback;
  const parsed = Number(value);
  if (!/^\d+$/.test(value) || !Number.isSafeInteger(parsed) || parsed <= 0)
    throw new Error(`Invalid ${name}: ${value}`);
  return parsed;
};
const columns = displayInteger('ELDER_LATENCY_COLUMNS', 80);
const rows = displayInteger('ELDER_LATENCY_ROWS', 24);
const fontText = process.env.ELDER_LATENCY_FONT ?? '12';
const font = Number(fontText);
if (!Number.isFinite(font) || font <= 0)
  throw new Error(`Invalid ELDER_LATENCY_FONT: ${fontText}`);
const output = resolve(destination);
await mkdir(output, { recursive: false });
await mkdir(join(output, 'config'));
await mkdir(join(output, 'data'));
const binary = join(root, '.build/elder-terms-terminal/elder-terms-terminal');
const helper = join(root, '.build/latency-performance');
const quote = (value: string): string => `'${value.split("'").join("'\\''")}'`;
const peerArgs = [
  join(helper, 'latency-peer'),
  join(output, 'peer.tsv'),
  history,
  mode,
  load,
  '10',
  '8',
];
const config = `[general]\ntype=local\nauto_close=true\n[terminal]\nwidth=${columns}\nheight=${rows}\nfont_size_dip=${font}\nscrollback_lines=10000\n[local]\ncommand_line=${peerArgs.map(quote).join(' ')}\n${links === 'off' ? '[hyperlink]\nenabled=false\n' : ''}`;
await writeFile(join(output, 'connection.ini'), config);
const env: NodeJS.ProcessEnv = {
  ...process.env,
  XDG_CONFIG_HOME: join(output, 'config'),
  XDG_DATA_HOME: join(output, 'data'),
  NO_AT_BRIDGE: '1',
  GTK_IM_MODULE: 'gtk-im-context-simple',
  GSETTINGS_BACKEND: 'memory',
  GDK_BACKEND: 'x11',
  GDK_SCALE: '1',
  GDK_DPI_SCALE: '1',
  GIO_USE_VFS: 'local',
  LC_ALL: 'C.UTF-8',
};
if (renderer === 'cairo') env.GDK_GL = 'disable';
else delete env.GDK_GL;
env.LD_PRELOAD = join(root, '.build/jemalloc/lib/libjemalloc.so.2');
if (tracing !== 'off') {
  env.LD_PRELOAD += `:${join(root, '.build/latency/latency-probe.so')}`;
  env.ELDER_LATENCY_TRACE = join(output, 'trace.tsv');
  if (tracing === 'light') env.ELDER_LATENCY_LIGHT = '1';
  else delete env.ELDER_LATENCY_LIGHT;
} else {
  delete env.ELDER_LATENCY_TRACE;
  delete env.ELDER_LATENCY_LIGHT;
}
const revision = (
  await exec('git', ['rev-parse', 'HEAD'], { cwd: root })
).stdout.trim();
const boxrigRevision = (
  await exec('git', ['rev-parse', 'HEAD'], { cwd: join(root, 'deps/boxrig') })
).stdout.trim();
const hash = createHash('sha256')
  .update(await readFile(binary))
  .digest('hex');
await writeFile(
  join(output, 'configuration.json'),
  JSON.stringify(
    {
      revision,
      boxrigRevision,
      hash,
      history,
      mode,
      load,
      count,
      stopAfter,
      renderer,
      tracing,
      idleMs,
      links,
      capture,
      columns,
      rows,
      font,
      display: env.DISPLAY,
      started: new Date().toISOString(),
      command: peerArgs,
      system: (await exec('uname', ['-a'])).stdout.trim(),
    },
    null,
    2
  )
);
let previousWindow: string | null = null;
try {
  previousWindow =
    (
      await exec('xdotool', ['getactivewindow'], { timeout: 1000 })
    ).stdout.trim() || null;
} catch {
  /* A display without a window manager has no active window to restore. */
}
const terminalLog = openSync(join(output, 'terminal.log'), 'w');
const app = spawn(binary, ['-c', join(output, 'connection.ini')], {
  env,
  stdio: ['ignore', terminalLog, terminalLog],
});
closeSync(terminalLog);
const exited = new Promise<number | null>((resolveExit, reject) => {
  app.once('error', reject);
  app.once('close', resolveExit);
});
let completed = false;
try {
  const found = await exec(
    'xdotool',
    [
      'search',
      '--sync',
      '--onlyvisible',
      '--pid',
      String(app.pid),
      '--name',
      'elder-latency:ready',
    ],
    { timeout: 120_000 }
  );
  const window = found.stdout.trim().split('\n')[0];
  // Place the test window where root capture has a stable unobscured region.
  await exec('xdotool', ['windowmove', '--sync', window, '80', '80']);
  if (Number(idleMs) !== 0) await delay(Number(idleMs));
  const input = await exec(
    join(helper, 'latency-input'),
    [window, output, count, stopAfter, capture, 'window'],
    { timeout: 300_000 }
  );
  await writeFile(join(output, 'input.log'), input.stdout + input.stderr);
  const exitCode = await exited;
  if (exitCode !== 0) throw new Error(`Terminal exited ${exitCode}`);
  completed = true;
  if (capture !== 'none')
    await exec('ffmpeg', [
      '-nostdin',
      '-hide_banner',
      '-loglevel',
      'error',
      '-f',
      'concat',
      '-safe',
      '0',
      '-i',
      join(output, 'video.ffconcat'),
      '-c:v',
      'ffv1',
      '-pix_fmt',
      'bgr0',
      '-fps_mode',
      'vfr',
      join(output, 'video.mkv'),
    ]);
  console.log(output);
} finally {
  if (!completed && app.exitCode === null) {
    app.kill('SIGTERM');
    await exited;
  }
  if (previousWindow !== null) {
    try {
      await exec('xdotool', ['windowactivate', '--sync', previousWindow], {
        timeout: 3000,
      });
    } catch {
      /* The original window may have closed during the measurement. */
    }
  }
}
