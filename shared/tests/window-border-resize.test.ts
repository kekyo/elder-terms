// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn, type ChildProcess } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createGtkAppLauncher } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import {
  createTestEvidence,
  expectElementKind,
} from './connection-test-helpers.ts';

const exec = promisify(execFile);

for (const application of ['terminal', 'file-transfer']) {
  for (const scale of [1, 2]) {
    for (const { width, composited } of [
      { width: 3, composited: false },
      { width: 11, composited: false },
      ...(scale === 1 ? [{ width: 11, composited: true }] : []),
    ]) {
      it(`resizes only on the visible ${width}px borders (${application}, scale ${scale}${composited ? ', composited' : ''})`, async (context) => {
        const directory = await mkdtemp(join(tmpdir(), 'elder-resize-border-'));
        const evidence = createTestEvidence(context);
        const launcher = createGtkAppLauncher({
          appPath: fileURLToPath(
            new URL(
              `../../.build/elder-terms-${application}/elder-terms-${application}`,
              import.meta.url
            )
          ),
          env: {
            LANGUAGE: 'en',
            LC_ALL: 'C.UTF-8',
            GTK_THEME: 'Adwaita',
            GDK_SCALE: String(scale),
            XDG_CONFIG_HOME: join(directory, 'config'),
            XDG_DATA_HOME: join(directory, 'data'),
          },
          xvfbScreen: scale === 1 ? '1600x1000x24' : '3200x2000x24',
          xvfbTrayHost: false,
          onSystemOutput: evidence.recordSystemOutputEvent,
        });
        let compositor: ChildProcess | undefined;
        let compositorFinished: Promise<void> | undefined;
        const compositorOutput: string[] = [];
        try {
          if (composited) {
            const env = {
              ...(await launcher.environment()),
              XDG_CONFIG_HOME: join(directory, 'config'),
              KWIN_COMPOSE: 'O2',
              LIBGL_ALWAYS_SOFTWARE: '1',
            };
            await mkdir(env.XDG_CONFIG_HOME, { recursive: true });
            await writeFile(
              join(env.XDG_CONFIG_HOME, 'kwinrc'),
              '[Compositing]\nEnabled=true\n[Plugins]\nblurEnabled=false\n'
            );
            await writeFile(
              join(env.XDG_CONFIG_HOME, 'kdeglobals'),
              '[KDE]\nAnimationDurationFactor=0\n'
            );
            compositor = spawn('kwin_x11', ['--replace'], {
              env,
              stdio: ['ignore', 'ignore', 'pipe'],
            });
            compositorFinished = new Promise((resolve, reject) => {
              compositor!.once('error', reject);
              compositor!.once('close', () => resolve());
            });
            compositor.stderr!.on('data', (bytes: Buffer) => {
              compositorOutput.push(bytes.toString());
            });
            await waitForResult(async () => {
              const result = await exec(
                'xprop',
                ['-root', '_NET_SUPPORTING_WM_CHECK'],
                { env }
              );
              expect(result.stdout).toContain('window id #');
            });
            await waitForResult(async () => {
              const result = await exec(
                'gdbus',
                [
                  'call',
                  '--session',
                  '--dest',
                  'org.kde.KWin',
                  '--object-path',
                  '/Compositor',
                  '--method',
                  'org.freedesktop.DBus.Properties.Get',
                  'org.kde.kwin.Compositing',
                  'active',
                ],
                { env }
              );
              expect(result.stdout).toContain('<true>');
            });
          }
          const config = join(directory, 'connection.ini');
          await writeFile(
            config,
            `[general]\ncompact_mode=true\nshow_border=true\nborder_width=${width}\nexterior_background=#800000\n${application === 'file-transfer' ? 'type=sftp\n[ssh]\naddress=fixture.example\n[sftp]\nremote_directory=/remote\nlocal_directory=' + directory : '[hyperlink]\nenabled=false'}\n`
          );
          const app = await launcher.launch(['--test-fixture', '-c', config], {
            onOutput: evidence.recordAppOutputEvent,
          });
          const window = expectElementKind(
            await app.getById(
              application === 'terminal'
                ? 'main_window'
                : 'file_transfer_window'
            ),
            'window'
          );
          await window.moveTo(180, 100);
          await window.activate();
          const prefix = application === 'terminal' ? '' : 'file_transfer_';
          if (composited) {
            const header = await app.getById(`${prefix}header_bar`);
            const firstFrame = await window.bounds();
            const firstHeader = (await header.capture()).bounds;
            for (const [horizontal, vertical] of [
              [-1, 0],
              [1, 0],
              [0, -1],
              [-1, -1],
              [1, -1],
            ]) {
              const { bounds, before } = await waitForResult(async () => {
                const before = await window.bounds();
                const bounds = (await header.capture()).bounds;
                expect(bounds.x - before.x).toBe(firstHeader.x - firstFrame.x);
                expect(bounds.y - before.y).toBe(firstHeader.y - firstFrame.y);
                expect(before.width - bounds.width).toBe(
                  firstFrame.width - firstHeader.width
                );
                return { bounds, before };
              });
              const inset = Math.floor(width / 2);
              const x =
                bounds.x +
                (horizontal < 0
                  ? inset
                  : horizontal > 0
                    ? bounds.width - 1 - inset
                    : Math.floor(bounds.width / 2));
              const y =
                bounds.y + (vertical ? inset : Math.floor(bounds.height / 2));
              await evidence.log('Header drag', {
                horizontal,
                vertical,
                x,
                y,
                before,
              });
              await app.input.moveMouseTo(x, y);
              await app.input.setMouseButton('left', true);
              try {
                let motion = 0;
                await waitForResult(async () => {
                  const delta = 20 + (motion++ % 2) * 4;
                  await app.input.moveMouseTo(
                    x + horizontal * delta,
                    y + vertical * delta
                  );
                  const after = await window.bounds();
                  if (horizontal)
                    expect(
                      after.width,
                      `header edge ${horizontal},${vertical}`
                    ).toBeGreaterThan(before.width);
                  if (vertical)
                    expect(
                      after.height,
                      `header edge ${horizontal},${vertical}`
                    ).toBeGreaterThan(before.height);
                });
              } finally {
                await app.input.setMouseButton('left', false);
              }
            }
          }
          const original = await window.bounds();
          await evidence.captureEvidence('visible-borders', async () =>
            app.capture()
          );
          const borders = [];
          for (const side of ['start', 'end', 'bottom']) {
            const border = await app.getById(`${prefix}frame_${side}_border`);
            borders.push({
              side,
              border,
              initial: (await border.capture()).bounds,
            });
          }
          for (const { side, border, initial } of borders) {
            const vertical = side !== 'bottom';
            // Check every physical pixel in our border, its inner neighbour,
            // and the exterior of the window (including the theme's outline).
            for (const offset of [
              ...Array(width * scale).keys(),
              -1,
              width * scale,
            ]) {
              const { before, bounds } = await waitForResult(async () => {
                const before = await window.bounds();
                const current = (await border.capture()).bounds;
                // Native window bounds are physical pixels, while AT-SPI
                // widget bounds use GTK logical coordinates.
                expect(current).toEqual({
                  x:
                    initial.x +
                    (before.x - original.x) / scale +
                    (side === 'end'
                      ? (before.width - original.width) / scale
                      : 0),
                  y:
                    initial.y +
                    (before.y - original.y) / scale +
                    (vertical ? 0 : (before.height - original.height) / scale),
                  width:
                    initial.width +
                    (vertical ? 0 : (before.width - original.width) / scale),
                  height:
                    initial.height +
                    (vertical ? (before.height - original.height) / scale : 0),
                });
                return { before, bounds: current };
              });
              expect(vertical ? bounds.width : bounds.height).toBe(width);
              let x = Math.round(
                bounds.x * scale +
                  (vertical ? offset : (bounds.width * scale) / 2)
              );
              let y = Math.round(
                bounds.y * scale +
                  (vertical ? (bounds.height * scale) / 2 : offset)
              );
              if (side === 'start' && offset === -1) x = before.x - 1;
              if (side === 'end' && offset === width * scale)
                x = before.x + before.width;
              if (side === 'bottom' && offset === width * scale)
                y = before.y + before.height;
              if (composited) {
                // The theme's transparent shadow used to contain an unrelated
                // ten-pixel resize grip outside the visible colored border.
                if (side === 'start' && offset === -1)
                  x = (bounds.x - 8) * scale;
                if (side === 'end' && offset === width * scale)
                  x = (bounds.x + bounds.width + 8) * scale;
                if (side === 'bottom' && offset === width * scale)
                  y = (bounds.y + bounds.height + 8) * scale;
              }
              await evidence.log('Border drag', { side, offset, bounds, x, y });
              await app.input.moveMouseTo(x, y);
              await app.input.setMouseButton('left', true);
              const distance = (offset % 2 === 0 ? 24 : -24) * scale;
              await app.input.moveMouseTo(
                x + (vertical ? distance : 0),
                y + (vertical ? 0 : distance)
              );
              const inside = offset >= 0 && offset < width * scale;
              try {
                if (inside) {
                  let motion = 0;
                  await waitForResult(async () => {
                    // Keep moving while the window manager acquires the drag
                    // grab; a single motion can precede that asynchronous grab.
                    if (composited) {
                      const step = distance + (motion++ % 2) * scale;
                      await app.input.moveMouseTo(
                        x + (vertical ? step : 0),
                        y + (vertical ? 0 : step)
                      );
                    }
                    const resized = await window.bounds();
                    expect(
                      vertical ? resized.width : resized.height,
                      `${side} border at pixel ${offset}`
                    ).not.toBe(vertical ? before.width : before.height);
                  });
                }
              } finally {
                await app.input.setMouseButton('left', false);
              }
              if (composited) await app.capture();
              if (!inside) {
                await app.capture();
                expect(
                  await window.bounds(),
                  `${side} outside pixel ${offset}`
                ).toEqual(before);
              }
            }
          }
          if (composited) {
            for (const side of ['start', 'end']) {
              const border = await app.getById(`${prefix}frame_${side}_border`);
              const bounds = (await border.capture()).bounds;
              const before = await window.bounds();
              const x = Math.round(bounds.x + bounds.width / 2);
              const y = bounds.y + bounds.height - 2;
              await app.input.moveMouseTo(x, y);
              await app.input.setMouseButton('left', true);
              try {
                let motion = 0;
                await waitForResult(async () => {
                  const distance = 24 + (motion++ % 2);
                  await app.input.moveMouseTo(
                    x + (side === 'start' ? -distance : distance),
                    y + distance
                  );
                  const resized = await window.bounds();
                  expect(resized.width).toBeGreaterThan(before.width);
                  expect(resized.height).toBeGreaterThan(before.height);
                });
              } finally {
                await app.input.setMouseButton('left', false);
              }
            }
          }
          await evidence.captureEvidence('resized-borders', async () =>
            app.capture()
          );
          expect((await app.output()).stderr).not.toMatch(/CRITICAL|WARNING/);
          await evidence.flushOutputs([app], launcher);
        } finally {
          if (compositor) {
            compositor.kill('SIGTERM');
            await compositorFinished;
            await evidence.log('Compositor output', compositorOutput.join(''));
          }
          await launcher.release();
          await evidence.release();
          await rm(directory, { recursive: true, force: true });
        }
      }, 90_000);
    }
  }
}
