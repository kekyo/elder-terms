// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  stat,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { PNG } from 'pngjs';

const pause = (milliseconds) =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

const run = async (program, args, env = process.env) => {
  const child = spawn(program, args, {
    env,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (bytes) => {
    stdout += bytes.toString();
  });
  child.stderr.on('data', (bytes) => {
    stderr += bytes.toString();
  });
  const [code] = await once(child, 'close');
  if (code !== 0)
    throw new Error(`${program} exited ${code}: ${stderr || stdout}`);
  return stdout;
};

const waitUntil = async (predicate, description, timeout = 10000) => {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await pause(50);
  }
  throw new Error(`Timed out waiting for ${description}`);
};

const findColor = (image, red, green, blue, tolerance = 2) => {
  let left = image.width;
  let top = image.height;
  let right = -1;
  let bottom = -1;
  let count = 0;
  for (let y = 0; y < image.height; y++) {
    for (let x = 0; x < image.width; x++) {
      const offset = (y * image.width + x) * 4;
      if (
        Math.abs(image.data[offset] - red) > tolerance ||
        Math.abs(image.data[offset + 1] - green) > tolerance ||
        Math.abs(image.data[offset + 2] - blue) > tolerance
      )
        continue;
      left = Math.min(left, x);
      top = Math.min(top, y);
      right = Math.max(right, x);
      bottom = Math.max(bottom, y);
      count++;
    }
  }
  return count > 20 ? { left, top, right, bottom, count } : undefined;
};

/**
 * Runs a native Wayland pilot under a Weston window on the outer X display.
 *
 * @param {string|null} binary Pilot executable path, or null when the caller launches product terminals.
 * @param {(harness: object) => Promise<void>} callback Receives capture and XTest input helpers.
 * @returns {Promise<void>} Resolves after the pilot and compositor have stopped.
 */
export const withWaylandHarness = async (binary, callback) => {
  if (!process.env.DISPLAY) throw new Error('Run this harness inside xvfb-run');
  const directory = await mkdtemp(join(tmpdir(), 'elder-dock-wayland-'));
  const runtime = join(directory, 'runtime');
  await mkdir(runtime, { mode: 0o700 });
  const config = join(directory, 'weston.ini');
  await writeFile(
    config,
    '[shell]\nbackground-image=\nbackground-color=0xffe03020\npanel-position=none\nlocking=false\nanimation=none\nclose-animation=none\n'
  );
  const socket = `dock-${process.pid}`;
  const env = {
    ...process.env,
    XDG_RUNTIME_DIR: runtime,
    WAYLAND_DISPLAY: socket,
    GDK_BACKEND: 'wayland',
    GDK_GL: 'disable',
    NO_AT_BRIDGE: '1',
    LIBGL_ALWAYS_SOFTWARE: '1',
  };
  const weston = spawn(
    'weston',
    [
      '--backend=x11-backend.so',
      '--renderer=pixman',
      '--width=1280',
      '--height=720',
      `--socket=${socket}`,
      '--idle-time=0',
      `--config=${config}`,
      `--log=${join(directory, 'weston.log')}`,
    ],
    { env, stdio: ['ignore', 'ignore', 'pipe'] }
  );
  let westonError = '';
  weston.stderr.on('data', (bytes) => {
    westonError += bytes.toString();
  });
  let app;
  let appOutput = '';
  let appError = '';
  try {
    await waitUntil(async () => {
      try {
        return (await stat(join(runtime, socket))).isSocket();
      } catch {
        if (weston.exitCode !== null)
          throw new Error(`Weston exited ${weston.exitCode}: ${westonError}`);
        return false;
      }
    }, 'Weston socket');
    if (binary) {
      app = spawn(binary, ['--fixture'], {
        env,
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      app.stdout.on('data', (bytes) => {
        appOutput += bytes.toString();
      });
      app.stderr.on('data', (bytes) => {
        appError += bytes.toString();
      });
    }
    const capture = async (name) => {
      const path = join(directory, `${name}.png`);
      await run('import', ['-window', 'root', path]);
      return PNG.sync.read(await readFile(path));
    };
    const point = async (x, y) => {
      await run('xdotool', ['mousemove', '--sync', String(x), String(y)]);
    };
    const drag = async (from, to, steps = 12, cancel = false) => {
      await point(from.x, from.y);
      await run('xdotool', ['mousedown', '1']);
      for (let step = 1; step <= steps; step++) {
        const x = Math.round(from.x + ((to.x - from.x) * step) / steps);
        const y = Math.round(from.y + ((to.y - from.y) * step) / steps);
        await point(x, y);
        await pause(35);
      }
      if (cancel) await run('xdotool', ['key', 'Escape']);
      await run('xdotool', ['mouseup', '1']);
    };
    await callback({
      directory,
      env,
      run,
      pause,
      waitUntil,
      capture,
      findColor,
      point,
      drag,
      app,
      get output() {
        return appOutput;
      },
      get errors() {
        return appError;
      },
    });
  } finally {
    if (app && app.exitCode === null) {
      app.kill('SIGTERM');
      await once(app, 'close');
    }
    if (weston.exitCode === null) {
      weston.kill('SIGTERM');
      await once(weston, 'close');
    }
    await rm(directory, { recursive: true, force: true });
  }
};
