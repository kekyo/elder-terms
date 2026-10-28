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
import type { SshLatencyFixture } from './ssh-latency-fixture.mjs';

const fixtureModule: typeof import('./ssh-latency-fixture.mjs') = await import(
  new URL('./ssh-latency-fixture.mts', import.meta.url).href
);

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
  transport = 'local',
] = process.argv.slice(2);
if (
  !destination ||
  !['plain', 'url', 'sparse-url', 'wrapped', 'osc8', 'mixed'].includes(mode) ||
  !['idle', 'append', 'overwrite', 'transcript'].includes(load) ||
  !['cairo', 'opengl'].includes(renderer) ||
  !['detail', 'light', 'off'].includes(tracing) ||
  !['on', 'off'].includes(links) ||
  !['none', 'video', 'full'].includes(capture) ||
  !['local', 'ssh'].includes(transport)
) {
  throw new Error(
    'Usage: run-latency OUTPUT [HISTORY MODE LOAD COUNT STOP_AFTER RENDERER detail|light|off IDLE_MS on|off none|video|full local|ssh]'
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
const binary = resolve(
  process.env.ELDER_LATENCY_BINARY ??
    join(root, '.build/elder-terms-terminal/elder-terms-terminal')
);
const boxrigSource = resolve(
  process.env.ELDER_LATENCY_BOXRIG_SOURCE ?? join(root, 'deps/boxrig')
);
const probe = resolve(
  process.env.ELDER_LATENCY_PROBE ??
    join(root, '.build/latency/latency-probe.so')
);
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
const cancellation = new AbortController();
let app: ReturnType<typeof spawn> | null = null;
const interrupt = (): void => {
  cancellation.abort();
  app?.kill('SIGTERM');
};
process.on('SIGINT', interrupt);
process.on('SIGTERM', interrupt);
let fixture: SshLatencyFixture | null = null;
try {
  fixture =
    transport === 'ssh'
      ? await fixtureModule.startSshLatencyFixture(output, peerArgs)
      : null;
  const connection =
    fixture === null
      ? `[local]\ncommand_line=${peerArgs.map(quote).join(' ')}\n`
      : `[ssh]\naddress=127.0.0.1\nport=${fixture.port}\nusername=${fixture.username}\nidentity_file=${fixture.clientKey}\n`;
  const config = `[general]\ntype=${transport}\nauto_close=true\n[terminal]\nwidth=${columns}\nheight=${rows}\nfont_size_dip=${font}\nscrollback_lines=10000\n${connection}${links === 'off' ? '[hyperlink]\nenabled=false\n' : ''}`;
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
    env.LD_PRELOAD += `:${probe}`;
    env.ELDER_LATENCY_TRACE = join(output, 'trace.tsv');
    if (tracing === 'light') env.ELDER_LATENCY_LIGHT = '1';
    else delete env.ELDER_LATENCY_LIGHT;
  } else {
    delete env.ELDER_LATENCY_TRACE;
    delete env.ELDER_LATENCY_LIGHT;
  }
  const revision = (
    await exec('git', ['rev-parse', 'HEAD'], {
      cwd: root,
      signal: cancellation.signal,
    })
  ).stdout.trim();
  const boxrigRevision =
    process.env.ELDER_LATENCY_BOXRIG_REVISION ??
    (
      await exec('git', ['rev-parse', 'HEAD'], {
        cwd: boxrigSource,
        signal: cancellation.signal,
      })
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
        boxrigSource,
        binary,
        probe: tracing === 'off' ? null : probe,
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
        transport,
        sshPort: fixture?.port ?? null,
        columns,
        rows,
        font,
        display: env.DISPLAY,
        started: new Date().toISOString(),
        command: peerArgs,
        system: (
          await exec('uname', ['-a'], { signal: cancellation.signal })
        ).stdout.trim(),
      },
      null,
      2
    )
  );
  let previousWindow: string | null = null;
  try {
    previousWindow =
      (
        await exec('xdotool', ['getactivewindow'], {
          timeout: 1000,
          signal: cancellation.signal,
        })
      ).stdout.trim() || null;
  } catch {
    /* A display without a window manager has no active window to restore. */
  }
  cancellation.signal.throwIfAborted();
  const terminalLog = openSync(join(output, 'terminal.log'), 'w');
  const appArgs = ['-c', join(output, 'connection.ini')];
  if (fixture !== null)
    appArgs.push(`--test-ssh-known-hosts-file=${fixture.knownHosts}`);
  try {
    app = spawn(binary, appArgs, {
      env,
      stdio: ['ignore', terminalLog, terminalLog],
    });
  } finally {
    closeSync(terminalLog);
  }
  const exited = new Promise<number | null>((resolveExit, reject) => {
    app!.once('error', reject);
    app!.once('close', resolveExit);
  });
  if (cancellation.signal.aborted) app.kill('SIGTERM');
  let completed = false;
  try {
    let window: string;
    if (fixture === null) {
      const found = await exec(
        'xdotool',
        [
          'search',
          '--sync',
          '--onlyvisible',
          '--all',
          '--pid',
          String(app.pid),
          '--name',
          'elder-latency:ready',
        ],
        { timeout: 120_000, signal: cancellation.signal }
      );
      window = found.stdout.trim().split('\n')[0];
    } else {
      const found = await exec(
        'xdotool',
        ['search', '--sync', '--onlyvisible', '--pid', String(app.pid)],
        { timeout: 120_000, signal: cancellation.signal }
      );
      window = found.stdout.trim().split('\n')[0];
      const deadline = Date.now() + 120_000;
      let ready = false;
      while (!ready && Date.now() < deadline) {
        try {
          const titled = await exec(
            'xdotool',
            [
              'search',
              '--onlyvisible',
              '--all',
              '--pid',
              String(app.pid),
              '--name',
              'elder-latency:ready',
            ],
            { timeout: 2000, signal: cancellation.signal }
          );
          ready = titled.stdout.trim().split('\n').includes(window);
        } catch {
          /* SSH authentication has not completed yet. */
        }
        if (!ready) {
          await exec('xdotool', ['windowfocus', window], {
            signal: cancellation.signal,
          });
          await exec('xdotool', ['key', 'Return'], {
            signal: cancellation.signal,
          });
          await delay(100, undefined, { signal: cancellation.signal });
        }
      }
      if (!ready) throw new Error('SSH peer did not report readiness');
    }
    // Place the test window where root capture has a stable unobscured region.
    await exec('xdotool', ['windowmove', '--sync', window, '80', '80'], {
      signal: cancellation.signal,
    });
    if (Number(idleMs) !== 0)
      await delay(Number(idleMs), undefined, { signal: cancellation.signal });
    const input = await exec(
      join(helper, 'latency-input'),
      [window, output, count, stopAfter, capture, 'window'],
      { timeout: 300_000, signal: cancellation.signal }
    );
    await writeFile(join(output, 'input.log'), input.stdout + input.stderr);
    const exitCode = await exited;
    if (exitCode !== 0) throw new Error(`Terminal exited ${exitCode}`);
    completed = true;
    if (capture !== 'none')
      await exec(
        'ffmpeg',
        [
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
        ],
        { signal: cancellation.signal }
      );
    cancellation.signal.throwIfAborted();
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
} finally {
  try {
    if (fixture !== null) await fixture.stop();
  } finally {
    process.off('SIGINT', interrupt);
    process.off('SIGTERM', interrupt);
  }
}
