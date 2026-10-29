// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn, type ChildProcess } from 'node:child_process';
import { once } from 'node:events';
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  runGtkTest,
  withTemporaryDirectory,
  readTerminalGridLayout,
} from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

const exec = promisify(execFile);

for (const backend of ['cairo', 'opengl']) {
  it(`blurs only the desktop behind the terminal and withdraws the effect (${backend})`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const config = join(directory, 'blur.ini');
      const peer = join(directory, 'peer.mjs');
      await writeFile(
        peer,
        "process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write('\\x1b[?25l\\x1b[48;2;0;0;0m\\x1b[38;2;255;255;255m\\x1b[2J\\x1b[HFOREGROUND\\r\\n\\x1b[38;2;0;0;255m████████');"
      );
      await writeFile(
        config,
        `[terminal]\ncursor_shape=bar\ncursor_blink=false\ncolor_filter=oklch\nbackground_mode=blur\nbackground_opacity=0\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await writeFile(
        join(directory, 'kwinrc'),
        '[Compositing]\nEnabled=true\n[Plugins]\nblurEnabled=true\n[Effect-blur]\nBlurStrength=10\nNoiseStrength=0\n'
      );
      await writeFile(
        join(directory, 'kdeglobals'),
        '[KDE]\nAnimationDurationFactor=0\n'
      );
      await runGtkTest(
        context,
        ['-c', config],
        async (app, evidence) => {
          const env = {
            ...(await app.environment()),
            XDG_CONFIG_HOME: directory,
            KWIN_COMPOSE: 'O2',
            LIBGL_ALWAYS_SOFTWARE: '1',
          };
          const processes: ChildProcess[] = [];
          const closed: Promise<unknown>[] = [];
          const logs: string[] = [];
          const launch = (command: string, args: string[]): ChildProcess => {
            const child = spawn(command, args, {
              env,
              stdio: ['ignore', 'pipe', 'pipe'],
            });
            processes.push(child);
            closed.push(once(child, 'close'));
            child.stderr.on('data', (bytes: Buffer) => {
              logs.push(command + ': ' + bytes.toString());
            });
            return child;
          };
          try {
            launch('kwin_x11', ['--replace']);
            await waitForResult(async () =>
              expect(
                (
                  await exec(
                    'xprop',
                    ['-root', '_KDE_NET_WM_BLUR_BEHIND_REGION'],
                    { env }
                  )
                ).stdout
              ).toContain(' = ')
            );
            launch(
              fileURLToPath(
                new URL(
                  '../../.build/elder-terms-terminal/appearance-backdrop',
                  import.meta.url
                )
              ),
              ['checker']
            );
            const layout = await readTerminalGridLayout(app);
            await layout.mainWindow.moveTo(50, 50);
            const terminal = await app.getById('terminal_view');
            const expectBlur = async (blurred: boolean): Promise<void> => {
              await waitForResult(async () => {
                const frame = PNG.sync.read((await terminal.capture()).image);
                let sum = 0,
                  square = 0,
                  count = 0,
                  white = 0,
                  correctedBlue = 0;
                for (let y = 60; y < frame.height - 60; y++)
                  for (let x = 60; x < frame.width - 60; x++) {
                    const value = frame.data[(y * frame.width + x) * 4];
                    sum += value;
                    square += value * value;
                    count++;
                  }
                const mean = sum / count;
                const deviation = Math.sqrt(square / count - mean * mean);
                expect(mean).toBeGreaterThan(95);
                // Compositors may blur in linear light (half white is sRGB 188)
                // or in sRGB (128). Both must suppress the checker contrast.
                expect(mean).toBeLessThan(205);
                if (blurred) expect(deviation).toBeLessThan(25);
                else expect(deviation).toBeGreaterThan(90);
                for (let y = 0; y < layout.cellHeight; y++)
                  for (let x = 0; x < 10 * layout.cellWidth; x++) {
                    const at = (y * frame.width + x) * 4;
                    if (
                      frame.data[at] > 240 &&
                      frame.data[at + 1] > 240 &&
                      frame.data[at + 2] > 240
                    )
                      white++;
                  }
                expect(white).toBeGreaterThan(20);
                for (let y = layout.cellHeight; y < 2 * layout.cellHeight; y++)
                  for (let x = 0; x < 10 * layout.cellWidth; x++) {
                    const at = (y * frame.width + x) * 4;
                    if (
                      frame.data[at] === 46 &&
                      frame.data[at + 1] === 105 &&
                      frame.data[at + 2] === 255
                    )
                      correctedBlue++;
                  }
                expect(correctedBlue).toBeGreaterThan(20);
              });
            };
            await expectBlur(true);
            await evidence.captureEvidence('frosted-background', async () =>
              app.capture()
            );
            const openSettings = async (): Promise<void> => {
              for (const id of [
                'application_menu_button',
                'settings_menu_item',
              ]) {
                const bounds = (await (await app.getById(id)).capture()).bounds;
                await app.input.moveMouseTo(
                  Math.round(bounds.x + bounds.width / 2),
                  Math.round(bounds.y + bounds.height / 2)
                );
                await app.input.setMouseButton('left', true);
                await app.input.setMouseButton('left', false);
              }
              const tabs = expectElementKind(
                await app.getById('settings_notebook'),
                'tabList'
              );
              for (let i = 0; i < (await tabs.getChildCount()); i++)
                if (
                  (await (await tabs.childAt(i))?.info())?.name ===
                  'Terminal appearance'
                ) {
                  await tabs.selectChildAt(i);
                  break;
                }
              const scrollbar = expectElementKind(
                await app.getById(
                  'settings_terminal_appearance_page_scrollbar'
                ),
                'scrollbar'
              );
              await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
            };
            const closeAfterApply = async (): Promise<void> => {
              await waitForResult(async () => {
                expect(
                  (await (await app.getById('settings_dialog')).info()).states
                ).toContain('showing');
              });
              await expectElementKind(
                await app.getById('settings_cancel_button'),
                'button'
              ).click();
            };
            const mode = async (index: number): Promise<void> => {
              const combo = expectElementKind(
                await app.getById('settings_terminal_background_mode_combo'),
                'comboBox'
              );
              const scrollbar = expectElementKind(
                await app.getById(
                  'settings_terminal_appearance_page_scrollbar'
                ),
                'scrollbar'
              );
              const page = (
                await (
                  await app.getById('settings_terminal_appearance_page')
                ).capture()
              ).bounds;
              const range = await scrollbar.valueInfo();
              const step = Math.max(1, Math.floor(page.height / 3));
              // More settings can follow background mode. Locate its row
              // instead of assuming that scrolling to the bottom reveals it.
              for (
                let value = range.minimum;
                value < range.maximum + step;
                value += step
              ) {
                await scrollbar.setValue(Math.min(value, range.maximum));
                if (!(await combo.info()).states.includes('showing')) continue;
                const position = (await combo.capture()).bounds;
                const current = (await scrollbar.valueInfo()).value;
                await scrollbar.setValue(
                  Math.max(
                    range.minimum,
                    Math.min(range.maximum, current + position.y - page.y - 40)
                  )
                );
                break;
              }
              await waitForResult(async () => {
                const position = (await combo.capture()).bounds;
                expect(position.y).toBeGreaterThanOrEqual(page.y);
                expect(position.y + position.height).toBeLessThanOrEqual(
                  page.y + page.height
                );
              });
              const bounds = (await combo.capture()).bounds;
              await app.input.moveMouseTo(
                Math.round(bounds.x + bounds.width / 2),
                Math.round(bounds.y + bounds.height / 2)
              );
              await app.input.setMouseButton('left', true);
              await app.input.setMouseButton('left', false);
              const item = await combo.childAt(index);
              expect(item).toBeDefined();
              await waitForResult(async () =>
                expect((await item!.info()).states).toContain('showing')
              );
              const target = (await item!.capture()).bounds;
              await app.input.moveMouseTo(
                Math.round(target.x + target.width / 2),
                Math.round(target.y + target.height / 2)
              );
              await app.input.setMouseButton('left', true);
              await app.input.setMouseButton('left', false);
              await waitForResult(async () =>
                expect(await combo.isChildSelected(index)).toBe(true)
              );
            };
            await openSettings();
            expect(
              await expectElementKind(
                await app.getById('settings_terminal_background_status'),
                'label'
              ).text()
            ).toBe('Frosted glass requested');
            await mode(2);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await closeAfterApply();
            await expectBlur(false);
            await evidence.captureEvidence('blur-withdrawn', async () =>
              app.capture()
            );
            await openSettings();
            await mode(3);
            await expectElementKind(
              await app.getById('settings_save_button'),
              'button'
            ).click();
            await expectBlur(true);
            await layout.mainWindow.resizeTo(
              layout.mainBounds.width + 100,
              layout.mainBounds.height + 80
            );
            await expectBlur(true);
            const native = await layout.mainWindow.x11Info();
            await exec('xdotool', ['windowunmap', '--sync', native.windowId], {
              env,
            });
            await exec('xdotool', ['windowmap', '--sync', native.windowId], {
              env,
            });
            await waitForResult(async () => {
              await layout.mainWindow.activate();
            });
            await expectBlur(true);
            const terminalBounds = (await terminal.capture()).bounds;
            const property = (
              await exec(
                'xprop',
                ['-id', native.windowId, '_KDE_NET_WM_BLUR_BEHIND_REGION'],
                { env }
              )
            ).stdout;
            const rectangle = property
              .split('=')[1]
              .trim()
              .split(',')
              .map(Number);
            expect(rectangle).toHaveLength(4);
            expect(rectangle[1]).toBeGreaterThan(0);
            expect(rectangle.slice(2)).toEqual([
              terminalBounds.width,
              terminalBounds.height,
            ]);

            // The compositor blurs the desktop underneath, never the app's image.
            const image = new PNG({
              width: terminalBounds.width,
              height: terminalBounds.height,
            });
            for (let y = 0; y < image.height; y++)
              for (let x = 0; x < image.width; x++) {
                const red = (Math.floor(x / 8) + Math.floor(y / 8)) % 2 === 0;
                image.data.set(
                  red ? [255, 0, 0, 255] : [0, 0, 255, 255],
                  (y * image.width + x) * 4
                );
              }
            const imagePath = join(directory, 'sharp.png');
            await writeFile(imagePath, PNG.sync.write(image));
            await openSettings();
            await expectElementKind(
              await app.getById('settings_terminal_background_image_entry'),
              'entry'
            ).setText(imagePath);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await closeAfterApply();
            await waitForResult(async () => {
              const frame = PNG.sync.read((await terminal.capture()).image);
              let red = 0,
                blue = 0,
                count = 0;
              for (let y = 60; y < frame.height - 60; y++)
                for (let x = 60; x < frame.width - 60; x++) {
                  const at = (y * frame.width + x) * 4;
                  if (
                    frame.data[at] === 255 &&
                    frame.data[at + 1] === 0 &&
                    frame.data[at + 2] === 0
                  )
                    red++;
                  if (
                    frame.data[at] === 0 &&
                    frame.data[at + 1] === 0 &&
                    frame.data[at + 2] === 255
                  )
                    blue++;
                  count++;
                }
              expect(red / count).toBeGreaterThan(0.25);
              expect(blue / count).toBeGreaterThan(0.25);
            });
            await evidence.captureEvidence('sharp-image-over-blur', async () =>
              app.capture()
            );
            await openSettings();
            await expectElementKind(
              await app.getById('settings_terminal_background_image_clear'),
              'button'
            ).click();
            await expectElementKind(
              await app.getById('settings_save_button'),
              'button'
            ).click();
            await expectBlur(true);
          } finally {
            for (const child of processes.reverse()) child.kill('SIGTERM');
            await Promise.all(closed);
            await evidence.log('compositor output', logs);
          }
        },
        {
          isolatedDisplay: true,
          env:
            backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' },
        }
      );
      expect(await readFile(config, 'utf8')).toContain('background_mode=blur');
      await runGtkTest(
        context,
        ['-c', config],
        async (app) => {
          await expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          ).click();
          await expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          ).click();
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_background_status'),
              'label'
            ).text()
          ).toBe('Opaque background: desktop transparency is unavailable');
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          const frame = PNG.sync.read(
            (await (await app.getById('terminal_view')).capture()).image
          );
          const at =
            (Math.floor(frame.height / 2) * frame.width +
              Math.floor(frame.width / 2)) *
            4;
          expect([...frame.data.subarray(at, at + 3)]).toEqual([0, 0, 0]);
        },
        {
          isolatedDisplay: true,
          env:
            backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' },
        }
      );
    });
  });
}
