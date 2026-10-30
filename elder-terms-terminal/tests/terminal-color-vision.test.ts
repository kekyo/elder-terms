// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

// Accepted stage-one reference colors: ANSI red/green, indexed 166/37,
// and TrueColor log samples. Native tests separately check discrimination.
const originals = ['800000', '008000', 'd75f00', '00afaf', 'c85050', '508050'];
const references = {
  protan: {
    off: ['800070', '008600', 'd74da7', '00b55b', 'c83ab3', '508400'],
    on: ['d05cba', '2c9d27', 'dd53ad', '00b55b', 'dc4ec5', '619623'],
  },
  deutan: {
    off: ['801c00', '007b71', 'd76a00', '00abd5', 'c85c00', '507d77'],
    on: ['d36a50', '33978c', 'd76a00', '00abd5', 'd7691d', '63918a'],
  },
  tritan: {
    off: ['840000', '7e6d00', 'e74f00', '68a7af', 'cd4b50', '7f7550'],
    on: ['dd6152', '99882d', 'ed5510', '68a7af', 'e15d60', '928862'],
  },
};

for (const backend of ['cairo', 'opengl']) {
  for (const [profile, colors] of Object.entries(references)) {
    it(`applies, cancels and saves inherited ${profile} recoloring (${backend})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const config = join(directory, 'vision.ini');
        const peer = join(directory, 'colors.mjs');
        const log = join(directory, 'raw.log');
        let output = '\x1b[?25l\x1b[2J\x1b[H\x1b]4;1;#800000;2;#008000\x07';
        const foregrounds = [
          '31',
          '32',
          '38;5;166',
          '38;5;37',
          '38;2;200;80;80',
          '38;2;80;128;80',
        ];
        const backgrounds = [
          '41',
          '42',
          '48;5;166',
          '48;5;37',
          '48;2;200;80;80',
          '48;2;80;128;80',
        ];
        for (let i = 0; i < originals.length; i++)
          output += `\x1b[${foregrounds[i]}mSAMPLE ${i} ████████\x1b[0m \x1b[${backgrounds[i]}m BG ${i}       \x1b[0m\r\n`;
        output += 'DEFAULT TEXT\r\n';
        await writeFile(
          peer,
          `process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write(${JSON.stringify(output)});`
        );
        const original = `[general]\nbackground=#202020\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=raw.log\nmode=raw\n[local]\ncommand_line=${process.execPath} ${peer}\n`;
        await writeFile(config, original);
        const options = {
          env:
            backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' },
          globalSettings: `[terminal]\ncolor_vision=${profile}\ncolor_vision_strength=100\n`,
        };
        const expectColors = (
          image: Buffer,
          foreground: string[],
          background: string[]
        ): void => {
          const frame = PNG.sync.read(image);
          const counts = new Map<string, number>();
          for (let i = 0; i < frame.data.length; i += 4) {
            const hex = frame.data.subarray(i, i + 3).toString('hex');
            counts.set(hex, (counts.get(hex) ?? 0) + 1);
          }
          for (const color of foreground)
            expect(counts.get(color) ?? 0, color).toBeGreaterThan(60);
          for (const color of background)
            expect(counts.get(color) ?? 0, color).toBeGreaterThan(500);
          expect(counts.get('202020') ?? 0).toBeGreaterThan(20000);
        };
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const savedAtStartup = await waitForResult(async () => {
              const saved = await readFile(config, 'utf8');
              expect(saved).toMatch(/^connection_id=[0-9a-f-]{36}$/mu);
              expect(
                saved.replace(/^connection_id=[0-9a-f-]{36}\n/mu, '')
              ).toBe(original);
              return saved;
            });
            const terminal = await app.getById('terminal_view');
            await waitForResult(async () =>
              expectColors(
                (await terminal.capture()).image,
                colors.off,
                colors.off
              )
            );
            const open = async (): Promise<void> => {
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
              for (let i = 0; i < (await tabs.getChildCount()); i++)
                if (
                  (await (await tabs.childAt(i))?.info())?.name ===
                  'Terminal appearance'
                )
                  await tabs.selectChildAt(i);
              const scroll = expectElementKind(
                await app.getById(
                  'settings_terminal_appearance_page_scrollbar'
                ),
                'scrollbar'
              );
              await scroll.setValue((await scroll.valueInfo()).maximum);
            };
            const select = async (index: number): Promise<void> => {
              await expectElementKind(
                await app.getById('settings_terminal_color_vision_combo'),
                'comboBox'
              ).selectChildAt(index);
            };
            const finish = async (action: string): Promise<void> => {
              await expectElementKind(
                await app.getById(`settings_${action}_button`),
                'button'
              ).click();
              if (action === 'apply') {
                await waitForResult(async () => {
                  expect(
                    (await (await app.getById('settings_dialog')).info()).states
                  ).toContain('showing');
                });
                await expectElementKind(
                  await app.getById('settings_cancel_button'),
                  'button'
                ).click();
              }
            };
            await open();
            await select(1);
            await finish('cancel');
            await waitForResult(async () =>
              expectColors(
                (await terminal.capture()).image,
                colors.off,
                colors.off
              )
            );
            await open();
            await select(1);
            await finish('apply');
            await waitForResult(async () =>
              expectColors(
                (await terminal.capture()).image,
                originals,
                originals
              )
            );
            expect(await readFile(config, 'utf8')).toBe(savedAtStartup);
            await open();
            await select(Object.keys(references).indexOf(profile) + 2);
            await expectElementKind(
              await app.getById(
                'settings_terminal_color_vision_strength_entry'
              ),
              'entry'
            ).setText('0');
            await finish('apply');
            await waitForResult(async () =>
              expectColors(
                (await terminal.capture()).image,
                originals,
                originals
              )
            );
            await open();
            await expectElementKind(
              await app.getById(
                'settings_terminal_color_vision_strength_entry'
              ),
              'entry'
            ).setText('100');
            await expectElementKind(
              await app.getById('settings_terminal_color_filter_combo'),
              'comboBox'
            ).selectChildAt(2);
            await finish('save');
            await waitForResult(async () =>
              expectColors(
                (await terminal.capture()).image,
                colors.on,
                colors.off
              )
            );
            await evidence.captureEvidence('color-vision-and-oklch', async () =>
              app.capture()
            );
          },
          options
        );
        const saved = await readFile(config, 'utf8');
        expect(saved).toContain(`color_vision=${profile}`);
        expect(saved).toContain('color_vision_strength=100');
        expect(saved).toContain('color_filter=oklch');
        const received = output.replace(/\n/g, '\r\n');
        expect(await readFile(log, 'utf8')).toBe(received);
        await runGtkTest(
          context,
          ['-c', config],
          async (app) => {
            const terminal = await app.getById('terminal_view');
            await waitForResult(async () =>
              expectColors(
                (await terminal.capture()).image,
                colors.on,
                colors.off
              )
            );
            await waitForResult(async () =>
              expect(await readFile(log, 'utf8')).toBe(received + received)
            );
          },
          options
        );
      });
    });
  }
}
