// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { chmod, mkdir, readFile, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withWaylandHarness } from './docking-prototype/wayland-harness.mjs';

if (process.argv[2] !== '--inside') {
  const child = spawn(
    'xvfb-run',
    [
      '-a',
      '-s',
      '-screen 0 1600x1000x24',
      'dbus-run-session',
      '--',
      process.execPath,
      fileURLToPath(import.meta.url),
      '--inside',
      ...process.argv.slice(2),
    ],
    { stdio: 'inherit' }
  );
  process.exit((await once(child, 'close'))[0] ?? 1);
}
const binary = resolve(process.argv[3]);
const compact = process.argv[4] === 'compact';
await withWaylandHarness(null, async (h) => {
  const processes = [];
  const file = (name) => join(h.directory, name);
  const colors = { Alpha: [20, 109, 163], Beta: [179, 90, 25] };
  const locate = async (name) =>
    h.findColor(await h.capture('screen'), ...colors[name]);
  const ready = async (name) => {
    let area;
    await h.waitUntil(async () => Boolean((area = await locate(name))), name);
    return area;
  };
  const start = async (name) => {
    const rgb = colors[name]
      .map((channel) => channel.toString(16).padStart(2, '0'))
      .join('');
    await writeFile(
      file(name + '.sh'),
      `#!/bin/sh\necho $$ > "${file(name + '.pid')}"\nwhile IFS= read -r line; do echo "$$:$line" >> "${file(name + '.input')}"; done\n`
    );
    await chmod(file(name + '.sh'), 0o700);
    await writeFile(
      file(name + '.ini'),
      `[general]\nname=${name}\ntype=local\nbackground=#${rgb}\ncompact_mode=${compact}\n[local]\ncommand_line=${file(name + '.sh')}\n[terminal]\nwidth=44\nheight=12\n`
    );
    const child = spawn(binary, ['-c', file(name + '.ini')], {
      env: {
        ...h.env,
        XDG_CONFIG_HOME: file('config'),
        XDG_DATA_HOME: file('data'),
      },
      stdio: 'ignore',
    });
    processes.push(child);
    return await ready(name);
  };
  const input = async (name, value) => {
    await h.run('xdotool', ['type', value]);
    await h.run('xdotool', ['key', 'Return']);
    const pid = (await readFile(file(name + '.pid'), 'utf8')).trim();
    await h.waitUntil(
      async () => {
        try {
          return (await readFile(file(name + '.input'), 'utf8')).includes(
            `${pid}:${value}\n`
          );
        } catch {
          return false;
        }
      },
      name + ' shell received ' + value
    );
  };
  try {
    await mkdir(file('config'));
    await start('Alpha');
    let b = await start('Beta');
    const a = await ready('Alpha');
    const inside = { x: a.left + 12, y: a.bottom - 8 };
    const headerOffset = compact ? 16 : 24;
    const startingPosition = { left: b.left, top: b.top };
    await h.drag(
      { x: b.left + 110, y: b.top - headerOffset },
      { x: b.left + 290, y: b.top + 64 }
    );
    await h.waitUntil(async () => {
      const moved = await locate('Beta');
      return (
        moved &&
        (Math.abs(moved.left - startingPosition.left) > 40 ||
          Math.abs(moved.top - startingPosition.top) > 40)
      );
    }, 'title text moved the Wayland terminal window');
    b = await ready('Beta');
    const beforeWhitespace = { left: b.left, top: b.top };
    await h.drag(
      { x: b.left + 44, y: b.top - headerOffset },
      { x: b.left + 44, y: b.top - headerOffset - 90 }
    );
    await h.waitUntil(async () => {
      const moved = await locate('Beta');
      return (
        moved &&
        (Math.abs(moved.left - beforeWhitespace.left) > 40 ||
          Math.abs(moved.top - beforeWhitespace.top) > 40)
      );
    }, 'title whitespace moved the Wayland terminal window');
    b = await ready('Beta');
    await h.drag({ x: b.left + 14, y: b.top - headerOffset }, inside);
    await h.waitUntil(
      async () => !(await locate('Alpha')),
      'Alpha is in the background'
    );
    const docked = await ready('Beta');
    await h.point(docked.left + 20, docked.top + 20);
    await h.run('xdotool', ['click', '1']);
    await input('Beta', 'docked');
    // The two rows occupy the sidebar directly to the left of the terminal.
    const from = { x: docked.left - 100, y: docked.top + 56 };
    const wallpaper = h.findColor(await h.capture('desktop'), 224, 48, 32);
    if (!wallpaper) throw new Error('Missing Weston desktop');
    await h.drag(from, { x: wallpaper.right - 6, y: wallpaper.bottom - 6 });
    await ready('Alpha');
    const detached = await ready('Beta');
    await h.point(detached.left + 20, detached.top + 20);
    await h.run('xdotool', ['click', '1']);
    await input('Beta', 'detached');
  } finally {
    for (const child of processes)
      if (child.exitCode === null && child.signalCode === null) {
        child.kill();
        await once(child, 'close');
      }
  }
});
