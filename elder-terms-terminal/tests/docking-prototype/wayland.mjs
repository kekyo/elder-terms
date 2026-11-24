// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { copyFile, mkdir, stat, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withWaylandHarness } from './wayland-harness.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const evidence = resolve(
  process.env.DOCK_RESULTS ?? join(here, '../../../test-results/dock-pilot/wayland')
);
if (process.argv[2] !== '--inside') {
  const child = spawn(
    'xvfb-run',
    [
      '-a',
      '-s',
      '-screen 0 1600x1000x24',
      process.execPath,
      fileURLToPath(import.meta.url),
      '--inside',
    ],
    { stdio: 'inherit', env: process.env }
  );
  const [code] = await once(child, 'close');
  process.exit(code ?? 1);
}

await mkdir(evidence, { recursive: true });
const colors = {
  1: [0x14, 0x6d, 0xa3],
  2: [0xb3, 0x5a, 0x19],
  3: [0x39, 0x83, 0x49],
  4: [0x8b, 0x47, 0x89],
};
const binary = join(here, 'build/dock-pilot');
try {
  await stat(binary);
} catch {
  throw new Error(`Build the pilot first: make -C ${here}`);
}

await withWaylandHarness(binary, async (h) => {
  const recorder = spawn(
    'ffmpeg',
    [
      '-nostdin',
      '-loglevel',
      'error',
      '-y',
      '-f',
      'x11grab',
      '-video_size',
      '1280x720',
      '-framerate',
      '15',
      '-i',
      process.env.DISPLAY,
      '-c:v',
      'libx264',
      '-threads',
      '2',
      '-preset',
      'ultrafast',
      '-pix_fmt',
      'yuv420p',
      join(evidence, 'gestures.mp4'),
    ],
    { stdio: ['ignore', 'ignore', 'pipe'] }
  );
  let recorderError = '';
  recorder.stderr.on('data', (bytes) => {
    recorderError += bytes.toString();
  });
  let checkedAt = 0;
  const expectLine = async (expression) => {
    await h.waitUntil(
      async () => expression.test(h.output.slice(checkedAt)),
      `pilot output ${expression}`
    );
  };
  const mark = () => {
    checkedAt = h.output.length;
  };
  const rect = (id, name) => {
    const records = [...h.output.matchAll(/^rect (\d+) (\w+) (\d+) (\d+) (\d+) (\d+)$/gm)];
    const record = records.reverse().find((row) => row[1] === String(id) && row[2] === name);
    if (!record) throw new Error(`Missing rectangle ${id}/${name}`);
    return {
      x: Number(record[3]),
      y: Number(record[4]),
      width: Number(record[5]),
      height: Number(record[6]),
    };
  };
  const locate = async (id, frameName) => {
    const color = colors[id];
    if (!color) throw new Error(`No marker color for window ${id}`);
    let visible;
    for (let attempt = 0; attempt < 12; attempt++) {
      const frame = await h.capture(frameName);
      // GTK darkens the button background while the window is inactive.
      const active = h.findColor(frame, ...color, 6);
      const dimmed = h.findColor(
        frame,
        ...color.map((channel) => Math.round(channel * 0.916)),
        6
      );
      visible =
        (active?.count ?? 0) >= (dimmed?.count ?? 0) ? active : dimmed;
      if (visible) break;
      await h.pause(50);
    }
    if (!visible)
      throw new Error(`Window ${id} title marker is not visible in ${frameName}`);
    const title = rect(id, 'title');
    return {
      x: visible.left - title.x,
      y: visible.top - title.y,
      visible,
    };
  };
  const point = async (id, name) => {
    const origin = await locate(id, `locate-${id}-${name}`);
    const area = rect(id, name);
    return {
      x:
        origin.x +
        area.x +
        (name === 'content' ? Math.min(40, Math.floor(area.width / 2)) : Math.floor(area.width / 2)),
      y: origin.y + area.y + Math.floor(area.height / 2),
    };
  };
  const click = async (id, name) => {
    const at = await point(id, name);
    await h.point(at.x, at.y);
    await h.run('xdotool', ['click', '1']);
  };
  const drag = async (id, name, to, cancel = false) => {
    await h.drag(await point(id, name), to, 12, cancel);
  };
  try {
    await expectLine(/^backend GdkWaylandDisplay$/m);
    await expectLine(/^state 1 tabs=AB active=A sidebar=1$/m);
    await expectLine(/^state 2 tabs=C active=C sidebar=0$/m);
    await expectLine(/^rect 2 native /m);
    await h.pause(200);
    const before = await locate(2, 'native-before');
    const native = await point(2, 'native');
    await drag(2, 'native', { x: native.x + 260, y: native.y + 65 });
    await h.pause(200);
    const after = await locate(2, 'native-after');
    if (
      Math.abs(after.x - before.x) < 30 &&
      Math.abs(after.y - before.y) < 30
    )
      throw new Error('Native title drag did not move the Wayland toplevel');
    await click(1, 'tabB');
    await expectLine(/^state 1 tabs=AB active=B sidebar=1$/m);
    mark();
    await h.run('xdotool', ['type', 'before']);
    await h.run('xdotool', ['key', 'Return']);
    await expectLine(/^pty B ack-\d+-before$/m);
    const pid = /^pty B ack-(\d+)-before$/m.exec(h.output)?.[1];
    if (!pid) throw new Error('Missing shell PID');
    mark();
    await drag(1, 'tabA', { x: 1200, y: 680 }, true);
    await expectLine(/^drag-failed 1 A (error|user-cancelled)$/m);
    if (/^state 3 /m.test(h.output.slice(checkedAt)))
      throw new Error('Escape unexpectedly undocked the tab');
    mark();
    await drag(1, 'tabB', await point(2, 'content'));
    await expectLine(/^state 1 tabs=A active=A sidebar=0$/m);
    await expectLine(/^state 2 tabs=CB active=B sidebar=1$/m);
    await click(2, 'content');
    await h.run('xdotool', ['type', 'after']);
    await h.run('xdotool', ['key', 'Return']);
    await expectLine(new RegExp(`^pty B ack-${pid}-after$`, 'm'));
    mark();
    await drag(2, 'tabB', { x: 800, y: 100 });
    await expectLine(/^drag-failed 2 B \S+$/m);
    if (/^state 3 /m.test(h.output.slice(checkedAt)))
      throw new Error('Desktop drop unexpectedly undocked the tab');
    await click(2, 'tabB');
    mark();
    await h.run('xdotool', ['key', 'ctrl+shift+u']);
    await expectLine(/^state 3 tabs=B active=B sidebar=0$/m);
    await expectLine(/^state 2 tabs=C active=C sidebar=0$/m);
    mark();
    await drag(3, 'title', await point(1, 'content'));
    await expectLine(/^state 1 tabs=AB active=B sidebar=1$/m);
    await expectLine(/^closed 3$/m);
    mark();
    await drag(1, 'title', await point(2, 'content'));
    await expectLine(/^state 2 tabs=CAB active=B sidebar=1$/m);
    await expectLine(/^closed 1$/m);
    const finalNative = await point(2, 'native');
    await drag(2, 'native', {
      x: finalNative.x - 500,
      y: finalNative.y - 50,
    });
    await h.pause(150);
    await h.capture('final-docked');
    await click(2, 'closeB');
    await expectLine(/^state 2 tabs=CA active=A sidebar=1$/m);
    await click(2, 'closeA');
    await expectLine(/^state 2 tabs=C active=C sidebar=0$/m);
    await h.capture('last-terminal');
  } finally {
    if (recorder.exitCode === null) recorder.kill('SIGINT');
    if (recorder.exitCode === null) await once(recorder, 'close');
    await writeFile(join(evidence, 'ffmpeg.log'), recorderError);
    await writeFile(join(evidence, 'pilot.log'), h.output + h.errors);
    await copyFile(join(h.directory, 'weston.log'), join(evidence, 'weston.log'));
    for (const name of ['native-before', 'native-after', 'final-docked', 'last-terminal']) {
      try {
        await copyFile(join(h.directory, `${name}.png`), join(evidence, `${name}.png`));
      } catch {
        // Failed steps may not have produced every frame.
      }
    }
  }
});
const frames = Number(
  execFileSync('ffprobe', [
    '-v',
    'error',
    '-select_streams',
    'v:0',
    '-count_frames',
    '-show_entries',
    'stream=nb_read_frames',
    '-of',
    'default=nw=1:nk=1',
    join(evidence, 'gestures.mp4'),
  ]).toString().trim()
);
if (frames < 30) throw new Error(`Gesture video recorded only ${frames} frames`);
console.log(
  `PASS Wayland native move, Escape, desktop drop, tab and title docking, key undock, PTY continuity and close; ${frames} video frames; evidence: ${evidence}`
);
