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

const samples = [
  ['000080', 'NAVY'],
  ['0000ff', 'BLUE'],
  ['5080ff', 'LIGHT BLUE'],
  ['8000ff', 'VIOLET'],
  ['00ffff', 'CYAN'],
  ['808080', 'GRAY'],
  ['ff0000', 'RED'],
  ['800000', 'DARK RED'],
  ['008000', 'GREEN'],
  ['808000', 'OLIVE'],
  ['800080', 'MAGENTA'],
  ['505050', 'DARK GRAY'],
  ['000000', 'BLACK'],
  ['ffffff', 'WHITE'],
] as const;
const filters = [
  'reverse-lightness',
  'reverse-contrast',
  'reverse-roles',
  'reverse-cam16',
  'reverse-palette',
];

for (const backend of ['cairo', 'opengl'])
  for (const [filterIndex, filter] of filters.entries()) {
    const background = '000000';
    it(`applies, cancels and saves ${filter} with unchanged terminal bytes (${backend})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const config = join(directory, 'filter.ini');
        const peer = join(directory, 'colors.mjs');
        let output = '\x1b[?25l\x1b[2J\x1b[H';
        for (const [hex, label] of samples) {
          const rgb = hex
            .match(/../g)!
            .map((value) => Number.parseInt(value, 16));
          output += `\x1b[38;2;${rgb.join(';')}m${label.padEnd(12)} ████████\r\n`;
        }
        output += '\x1b[0mDEFAULT TEXT\r\n';
        // The PTY retains ONLCR, adding CR before each LF in the received bytes.
        const receivedOutput = output.replace(/\n/g, '\r\n');
        await writeFile(
          peer,
          `process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write(${JSON.stringify(output)});`
        );
        const logPath = join(directory, 'raw.log');
        const original = `[general]\nbackground=#${background}\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=raw.log\nmode=raw\n[local]\ncommand_line=${process.execPath} ${peer}\n`;
        await writeFile(config, original);
        const env =
          backend === 'cairo'
            ? { GDK_GL: 'disable' }
            : { LIBGL_ALWAYS_SOFTWARE: '1' };
        const expectColors = async (
          image: Buffer,
          enabled: boolean
        ): Promise<void> => {
          const frame = PNG.sync.read(image);
          const counts = new Map<string, number>();
          for (let i = 0; i < frame.data.length; i += 4) {
            const hex = frame.data.subarray(i, i + 3).toString('hex');
            counts.set(hex, (counts.get(hex) ?? 0) + 1);
          }
          expect(
            counts.get(enabled ? 'ffffff' : '000000') ?? 0
          ).toBeGreaterThan((frame.width * frame.height) / 2);
          expect(
            counts.get(enabled ? '000000' : 'ffffff') ?? 0
          ).toBeGreaterThan(150);
          let chromatic = 0;
          for (let i = 0; i < frame.data.length; i += 4)
            if (
              Math.max(frame.data[i], frame.data[i + 1], frame.data[i + 2]) -
                Math.min(frame.data[i], frame.data[i + 1], frame.data[i + 2]) >
              20
            )
              chromatic++;
          expect(chromatic).toBeGreaterThan(200);
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
              expectColors((await terminal.capture()).image, false)
            );
            await evidence.captureEvidence('filter-off', async () =>
              app.capture()
            );
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
              for (let index = 0; index < (await tabs.getChildCount()); index++)
                if (
                  (await (await tabs.childAt(index))?.info())?.name ===
                  'Terminal appearance'
                ) {
                  await tabs.selectChildAt(index);
                  break;
                }
              const scroll = expectElementKind(
                await app.getById(
                  'settings_terminal_appearance_page_scrollbar'
                ),
                'scrollbar'
              );
              await scroll.setValue((await scroll.valueInfo()).maximum);
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
            const select = async (index: number): Promise<void> => {
              const combo = expectElementKind(
                await app.getById('settings_terminal_color_filter_combo'),
                'comboBox'
              );
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
            await openSettings();
            await select(filterIndex + 3);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await closeAfterApply();
            await waitForResult(async () =>
              expectColors((await terminal.capture()).image, true)
            );
            expect(await readFile(config, 'utf8')).toBe(savedAtStartup);
            await evidence.captureEvidence(filter, async () => app.capture());
            await openSettings();
            await select(1);
            await expectElementKind(
              await app.getById('settings_cancel_button'),
              'button'
            ).click();
            await waitForResult(async () =>
              expectColors((await terminal.capture()).image, true)
            );
            await openSettings();
            await select(1);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await closeAfterApply();
            await waitForResult(async () =>
              expectColors((await terminal.capture()).image, false)
            );
            await openSettings();
            await select(filterIndex + 3);
            await expectElementKind(
              await app.getById('settings_save_button'),
              'button'
            ).click();
            await waitForResult(async () =>
              expectColors((await terminal.capture()).image, true)
            );
          },
          { env }
        );
        expect(await readFile(config, 'utf8')).toContain(
          `color_filter=${filter}`
        );
        expect(await readFile(logPath, 'utf8')).toBe(receivedOutput);
        await runGtkTest(
          context,
          ['-c', config],
          async (app) => {
            const terminal = await app.getById('terminal_view');
            await waitForResult(async () =>
              expectColors((await terminal.capture()).image, true)
            );
            await waitForResult(async () =>
              expect(await readFile(logPath, 'utf8')).toBe(
                receivedOutput + receivedOutput
              )
            );
          },
          { env }
        );
      });
    });
  }
