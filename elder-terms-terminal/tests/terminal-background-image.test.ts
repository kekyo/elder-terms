// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { chmod, readFile, unlink, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

for (const backend of ['cairo', 'opengl']) {
  it(`keeps alpha images independent of desktop transparency and failed edits (${backend})`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const imagePath = join(directory, 'green.png');
      const png = new PNG({ width: 40, height: 20 });
      for (let i = 0; i < png.data.length; i += 4) {
        const x = (i / 4) % png.width;
        png.data.set(
          x < 10
            ? [255, 0, 0, 128]
            : x >= 30
              ? [0, 0, 255, 128]
              : [0, 255, 0, 128],
          i
        );
      }
      await writeFile(imagePath, PNG.sync.write(png));
      const replacementPath = join(directory, 'blue.png');
      const blue = new PNG({ width: 1, height: 1 });
      blue.data.set([0, 0, 255, 255]);
      await writeFile(replacementPath, PNG.sync.write(blue));
      const config = join(directory, 'image.ini');
      const peer = join(directory, 'peer.mjs');
      await writeFile(
        peer,
        "process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write('\\x1b[?25l\\x1b[48;2;0;0;0m\\x1b[38;2;255;255;255m\\x1b[2J\\x1b[HIMAGE'); process.stdin.on('data', () => process.stdout.write('\\r\\n'.repeat(50) + 'SCROLLED'));"
      );
      await writeFile(
        config,
        `[terminal]\nbackground_mode=opaque\nbackground_opacity=0\nbackground_image=${imagePath}\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await runGtkTest(
        context,
        ['-c', config],
        async (app, evidence) => {
          const terminal = await app.getById('terminal_view');
          const expectImage = async (
            fit: string,
            opacity: number
          ): Promise<void> => {
            await waitForResult(async () => {
              const frame = PNG.sync.read((await terminal.capture()).image);
              for (const [x, y, rgb] of [
                [0.5, 0.5, [0, opacity, 0]],
                [0.2, 0.5, fit === 'cover' ? [0, opacity, 0] : [opacity, 0, 0]],
                [0.5, 0.03, fit === 'contain' ? [0, 0, 0] : [0, opacity, 0]],
              ] as const) {
                const at =
                  (Math.floor(frame.height * y) * frame.width +
                    Math.floor(frame.width * x)) *
                  4;
                for (const [channel, expected] of rgb.entries())
                  expect(
                    Math.abs(frame.data[at + channel] - expected),
                    `${fit} at ${x},${y}: ${[...frame.data.subarray(at, at + 3)]}`
                  ).toBeLessThanOrEqual(2);
              }
            });
          };
          const openSettings = async (): Promise<void> => {
            await expectElementKind(
              await app.getById('application_menu_button'),
              'toggleButton'
            ).click();
            await expectElementKind(
              await app.getById('settings_menu_item'),
              'menuItem'
            ).click();
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
              await app.getById('settings_terminal_appearance_page_scrollbar'),
              'scrollbar'
            );
            await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
          };
          await expectImage('cover', 128);
          await evidence.captureEvidence('opaque-image-alpha', async () =>
            app.capture()
          );
          await app.input.pressKey('Return');
          await waitForResult(async () =>
            expect(
              (
                await expectElementKind(
                  await app.getById('terminal_scrollbar'),
                  'scrollbar'
                ).valueInfo()
              ).maximum
            ).toBeGreaterThan(24)
          );
          await expectImage('cover', 128);
          // Placement and opacity must work with retained pixels after the source disappears.
          await unlink(imagePath);
          for (const [index, fit] of [
            'cover',
            'contain',
            'stretch',
          ].entries()) {
            await openSettings();
            const combo = expectElementKind(
              await app.getById('settings_terminal_background_image_fit_combo'),
              'comboBox'
            );
            const bounds = (await combo.capture()).bounds;
            await app.input.moveMouseTo(
              Math.round(bounds.x + bounds.width / 2),
              Math.round(bounds.y + bounds.height / 2)
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            // GTK must expose the popup before native input selects its item.
            const item = (await combo.childAt(index + 1))!;
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
              expect(await combo.isChildSelected(index + 1)).toBe(true)
            );
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect(
                (await (await app.getById('settings_dialog')).info()).states
              ).toContain('showing');
            });
            await expectElementKind(
              await app.getById('settings_cancel_button'),
              'button'
            ).click();
            await expectImage(fit, 128);
            await evidence.captureEvidence(`image-${fit}`, async () =>
              app.capture()
            );
          }
          await openSettings();
          await expectElementKind(
            await app.getById(
              'settings_terminal_background_image_opacity_entry'
            ),
            'entry'
          ).setText('50');
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await expectImage('stretch', 64);
          const saved = await readFile(config, 'utf8');
          expect(saved).toContain('background_image_opacity=50');
          expect(saved).toContain('background_image_fit=stretch');
          await openSettings();
          await expectElementKind(
            await app.getById('settings_terminal_background_image_entry'),
            'entry'
          ).setText(replacementPath);
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
            expect(await readFile(config, 'utf8')).toBe(saved);
          } finally {
            await chmod(directory, 0o755);
          }
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectImage('stretch', 64);
          await openSettings();
          await expectElementKind(
            await app.getById('settings_terminal_background_image_entry'),
            'entry'
          ).setText(imagePath + '.missing');
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(
              await expectElementKind(
                await app.getById('settings_action_message'),
                'label'
              ).text()
            ).toContain('.missing')
          );
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectImage('stretch', 64);
          expect(await readFile(config, 'utf8')).toContain(
            `background_image=${imagePath}\n`
          );
        },
        {
          env:
            backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' },
        }
      );
      await writeFile(imagePath, PNG.sync.write(png));
      await runGtkTest(
        context,
        ['-c', config],
        async (app) => {
          await waitForResult(async () => {
            const frame = PNG.sync.read(
              (await (await app.getById('terminal_view')).capture()).image
            );
            const at =
              (Math.floor(frame.height / 2) * frame.width +
                Math.floor(frame.width / 2)) *
              4;
            expect([...frame.data.subarray(at, at + 3)]).toEqual([0, 64, 0]);
          });
          await expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          ).click();
          await expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          ).click();
          await expectElementKind(
            await app.getById('settings_terminal_background_image_clear'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            const frame = PNG.sync.read(
              (await (await app.getById('terminal_view')).capture()).image
            );
            const at =
              (Math.floor(frame.height / 2) * frame.width +
                Math.floor(frame.width / 2)) *
              4;
            expect([...frame.data.subarray(at, at + 3)]).toEqual([0, 0, 0]);
          });
          expect(await readFile(config, 'utf8')).toContain(
            'background_image=\n'
          );
        },
        {
          env:
            backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' },
        }
      );
      await writeFile(
        config,
        (await readFile(config, 'utf8')).replace(
          'background_image=\n',
          `background_image=${imagePath}\n`
        )
      );
      await unlink(imagePath);
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
          await waitForResult(async () =>
            expect(
              await expectElementKind(
                await app.getById('settings_terminal_background_status'),
                'label'
              ).text()
            ).toContain('Background image:')
          );
          expect(await readFile(config, 'utf8')).toContain(
            `background_image=${imagePath}\n`
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
  });
}
