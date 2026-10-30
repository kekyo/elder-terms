// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn, execFile, type ChildProcess } from 'node:child_process';
import { chmod, readFile, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
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
const backdropPath = fileURLToPath(
  new URL(
    '../../.build/elder-terms-terminal/appearance-backdrop',
    import.meta.url
  )
);

for (const backend of ['cairo', 'opengl']) {
  // Composition, repeated settings edits, and a compositor-free restart can
  // exceed the global one-minute budget when using software rendering.
  it(`composites only the terminal background over the desktop (${backend})`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const config = join(directory, 'background.ini');
      const peer = join(directory, 'peer.mjs');
      const backgroundImage = join(directory, 'green.png');
      const image = new PNG({ width: 1, height: 1 });
      image.data.set([0, 255, 0, 128]);
      await writeFile(backgroundImage, PNG.sync.write(image));
      await writeFile(
        peer,
        `process.stdin.setRawMode(true); process.stdin.resume();
process.stdout.write('\\x1b[?25l\\x1b[48;2;0;0;0m\\x1b[38;2;255;255;255m\\x1b[2J\\x1b[HWHITE');
`
      );
      await writeFile(
        config,
        `[terminal]\nbackground_mode=transparent\nbackground_opacity=50\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await writeFile(
        join(directory, 'kwinrc'),
        '[Compositing]\nEnabled=true\n[Plugins]\nblurEnabled=false\n'
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
          const finished: Promise<void>[] = [];
          const logs: string[] = [];
          const launch = (command: string, args: string[]): void => {
            const child = spawn(command, args, {
              env,
              stdio: ['ignore', 'pipe', 'pipe'],
            });
            processes.push(child);
            finished.push(
              new Promise((resolve, reject) => {
                child.once('error', reject);
                child.once('close', () => resolve());
              })
            );
            child.stderr.on('data', (bytes: Buffer) => {
              logs.push(command + ': ' + bytes.toString());
            });
          };
          try {
            launch('kwin_x11', ['--replace']);
            await waitForResult(async () => {
              const result = await exec(
                'xprop',
                ['-root', '_NET_SUPPORTING_WM_CHECK'],
                { env }
              );
              expect(result.stdout).toContain('window id #');
            });
            launch(backdropPath, []);
            const layout = await readTerminalGridLayout(app);
            await layout.mainWindow.moveTo(50, 50);
            const terminal = await app.getById('terminal_view');
            const expectBackground = async (rgb: number[]): Promise<void> => {
              await waitForResult(async () => {
                const png = PNG.sync.read((await terminal.capture()).image);
                const at =
                  (Math.floor(png.height / 2) * png.width +
                    Math.floor(png.width / 2)) *
                  4;
                for (const [channel, value] of rgb.entries())
                  expect(
                    Math.abs(png.data[at + channel] - value)
                  ).toBeLessThanOrEqual(2);
                let white = 0;
                for (let y = 0; y < layout.cellHeight; y++)
                  for (let x = 0; x < 5 * layout.cellWidth; x++) {
                    const offset = (y * png.width + x) * 4;
                    if (
                      png.data[offset] > 240 &&
                      png.data[offset + 1] > 240 &&
                      png.data[offset + 2] > 240
                    )
                      white++;
                  }
                expect(white).toBeGreaterThan(10);
              });
            };
            await expectBackground([112, 24, 16]);
            await evidence.captureEvidence('transparent-background', async () =>
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
              await expectElementKind(
                await app.getById('settings_dialog'),
                'window'
              ).moveTo(620, 80);
              const tabs = expectElementKind(
                await app.getById('settings_notebook'),
                'tabList'
              );
              for (let i = 0; i < (await tabs.getChildCount()); i++) {
                if (
                  (await (await tabs.childAt(i))?.info())?.name ===
                  'Terminal appearance'
                ) {
                  await tabs.selectChildAt(i);
                  break;
                }
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
            const setEntry = async (
              name: string,
              value: string
            ): Promise<void> => {
              await expectElementKind(
                await app.getById(
                  'settings_terminal_background_' + name + '_entry'
                ),
                'entry'
              ).setText(value);
            };
            const selectMode = async (index: number): Promise<void> => {
              // Wait for the popup item before sending native selection input.
              // Key events sent while GTK creates the popup can be lost.
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
              const item = (await combo.childAt(index))!;
              await waitForResult(async () =>
                expect((await item.info()).states).toContain('showing')
              );
              const target = (await item.capture()).bounds;
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
            for (const step of [
              { opacity: '0', tint: '0', rgb: [224, 48, 32] },
              { opacity: '0', tint: '50', rgb: [112, 24, 144] },
              { opacity: '0', tint: '100', rgb: [0, 0, 255] },
              { opacity: '100', tint: '100', rgb: [0, 0, 0] },
              { opacity: '50', tint: '0', rgb: [112, 24, 16] },
            ]) {
              await openSettings();
              expect(
                await expectElementKind(
                  await app.getById('settings_terminal_background_status'),
                  'label'
                ).text()
              ).toBe('Transparent background');
              await setEntry('opacity', step.opacity);
              await setEntry('tint_color', '#0000ff');
              await setEntry('tint_opacity', step.tint);
              await expectElementKind(
                await app.getById('settings_apply_button'),
                'button'
              ).click();
              await closeAfterApply();
              await expectBackground(step.rgb);
              expect(await readFile(config, 'utf8')).not.toContain(
                'background_tint_color'
              );
            }
            // Failed persistence must leave the dialog/draft available and restore
            // the previous presentation, even after native settings were accepted.
            await openSettings();
            await setEntry('opacity', '0');
            const stored = await readFile(config, 'utf8');
            await chmod(directory, 0o555);
            try {
              await expectElementKind(
                await app.getById('settings_save_button'),
                'button'
              ).click();
              await waitForResult(async () =>
                expect(
                  await expectElementKind(
                    await app.getById('settings_action_message'),
                    'label'
                  ).text()
                ).toBe('Failed to save connection')
              );
              expect(
                await expectElementKind(
                  await app.getById(
                    'settings_terminal_background_opacity_entry'
                  ),
                  'entry'
                ).text()
              ).toBe('0');
              expect(await readFile(config, 'utf8')).toBe(stored);
            } finally {
              await chmod(directory, 0o755);
            }
            await expectElementKind(
              await app.getById('settings_cancel_button'),
              'button'
            ).click();
            await expectBackground([112, 24, 16]);
            await openSettings();
            await selectMode(1);
            await setEntry('opacity', '0');
            await setEntry('tint_opacity', '100');
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await closeAfterApply();
            await expectBackground([0, 0, 0]);
            await openSettings();
            expect(
              await expectElementKind(
                await app.getById('settings_terminal_background_status'),
                'label'
              ).text()
            ).toBe('Opaque background');
            await selectMode(2);
            await expectElementKind(
              await app.getById('settings_save_button'),
              'button'
            ).click();
            await expectBackground([0, 0, 255]);
            await evidence.captureEvidence('tinted-background', async () =>
              app.capture()
            );
            expect(await readFile(config, 'utf8')).toContain(
              'background_tint_color=#0000ff'
            );
            await openSettings();
            await setEntry('tint_opacity', '0');
            await setEntry('image', backgroundImage);
            await setEntry('image_opacity', '50');
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await closeAfterApply();
            await expectBackground([168, 100, 24]);
            await evidence.captureEvidence(
              'transparent-image-alpha',
              async () => app.capture()
            );
            await openSettings();
            await setEntry('tint_opacity', '50');
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await closeAfterApply();
            await expectBackground([84, 50, 140]);
            await openSettings();
            await expectElementKind(
              await app.getById('settings_terminal_background_image_clear'),
              'button'
            ).click();
            await setEntry('tint_opacity', '100');
            await expectElementKind(
              await app.getById('settings_save_button'),
              'button'
            ).click();
            await expectBackground([0, 0, 255]);
          } finally {
            for (const child of processes.reverse()) child.kill('SIGTERM');
            await Promise.all(finished);
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
      await runGtkTest(
        context,
        ['-c', config],
        async (app, evidence) => {
          const terminal = await app.getById('terminal_view');
          await waitForResult(async () => {
            const png = PNG.sync.read((await terminal.capture()).image);
            const offset =
              (Math.floor(png.height / 2) * png.width +
                Math.floor(png.width / 2)) *
              4;
            expect([...png.data.subarray(offset, offset + 3)]).toEqual([
              0, 0, 255,
            ]);
          });
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
          expect(await readFile(config, 'utf8')).toContain(
            'background_mode=transparent'
          );
          await evidence.captureEvidence(
            'restored-tint-without-compositor',
            async () => app.capture()
          );
        },
        {
          env:
            backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' },
        }
      );
    });
  }, 180_000);
}
