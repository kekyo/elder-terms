// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  readWindowCellLayout,
  runGtkTest,
  scrollWheelWithControl,
} from './gtk-test-helpers';
import {
  withTemporaryDirectory,
  capturePixel,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

describe('terminal zoom limits', () => {
  for (const backend of ['cairo', 'opengl']) {
    for (const compact of [false, true]) {
      for (const mode of ['preserve-grid', 'preserve-window']) {
        it(`keeps a large colored local terminal usable at minimum zoom (${backend}, compact=${compact}, ${mode})`, async (context) => {
          await withTemporaryDirectory(async (directory) => {
            const config = join(directory, 'terminal.ini');
            const peer = join(directory, 'peer.mjs');
            const input = join(directory, 'input.txt');
            await writeFile(
              peer,
              `import { appendFileSync } from 'node:fs';
process.stdin.setRawMode(true);
process.stdin.resume();
process.stdin.on('data', bytes => {
  appendFileSync(process.argv[2], bytes);
  process.stdout.write(bytes);
});
process.stdout.write('Local terminal\\r\\n日本語とEnglish\\r\\n');
`
            );
            await writeFile(
              config,
              `[general]\ncompact_mode=${compact}\nbackground=#2B1212\nshow_border=true\n[terminal]\nfont_families=Inconsolata;Noto Sans Mono CJK JP;\nfont_zoom_mode=${mode}\ncursor_shape=bar\nwidth=111\nheight=31\n[local]\ncommand_line=${process.execPath} ${peer} ${input}\n`
            );
            await runGtkTest(
              context,
              [
                ...(mode === 'preserve-grid' ? ['--test-maximize-window'] : []),
                '-c',
                config,
              ],
              async (app, evidence) => {
                let current = await waitForResult(() =>
                  readWindowCellLayout(app)
                );
                // Both a maximized window and the normal preserve-window mode
                // must grow the grid without shrinking the current allocation.
                await current.mainWindow.resizeTo(3840, 1600);
                current = await waitForResult(async () => {
                  const layout = await readWindowCellLayout(app);
                  expect(layout.columns).toBeGreaterThan(500);
                  expect(layout.rows).toBeGreaterThan(80);
                  return layout;
                });
                const initial = current;
                const state = expectElementKind(
                  await app.getById('fixture_terminal_state_label'),
                  'label'
                );
                expect(await state.text()).toContain(`backend=${backend}`);
                const terminal = await app.getById('terminal_view');
                let sent = '';
                for (let step = 0; step < 10; ++step) {
                  const bounds = (await terminal.capture()).bounds;
                  await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
                  await scrollWheelWithControl(app, 1);
                  await app.input.pressKey('x');
                  sent += 'x';
                  current = await waitForResult(async () => {
                    // The PTY acknowledgement proves input still works even after
                    // additional wheel events beyond the minimum font size.
                    expect(await readFile(input, 'utf8')).toBe(sent);
                    return await readWindowCellLayout(app);
                  });
                  await evidence.log('zoom out', {
                    step,
                    font: current.fontSizeDip,
                    columns: current.columns,
                    rows: current.rows,
                  });
                }
                expect(current.fontSizeDip).toBeLessThanOrEqual(8);
                expect(current.columns).toBeGreaterThan(initial.columns);
                expect(current.rows).toBeGreaterThan(initial.rows);
                expect([
                  current.mainBounds.width,
                  current.mainBounds.height,
                ]).toEqual([
                  initial.mainBounds.width,
                  initial.mainBounds.height,
                ]);
                expect(
                  capturePixel(await terminal.capture(), 0.9, 0.9)
                ).toEqual([43, 18, 18]);
                await evidence.captureEvidence('minimum-font', async () =>
                  app.capture()
                );
                await scrollWheelWithControl(app, -1);
                await waitForResult(async () => {
                  expect(
                    (await readWindowCellLayout(app)).fontSizeDip
                  ).toBeGreaterThan(current.fontSizeDip);
                });
                expect((await app.output()).stderr).not.toMatch(
                  /boxrig .* failed/
                );
              },
              {
                env: {
                  GDK_GL: backend === 'cairo' ? 'disable' : 'always',
                  LIBGL_ALWAYS_SOFTWARE: '1',
                },
              }
            );
          });
        });
      }
    }
    for (const limit of [6, 72]) {
      it(`accepts input at the font size limit and zooms back (${backend}, ${limit})`, async (context) => {
        await withTemporaryDirectory(async (directory) => {
          const config = join(directory, 'terminal.ini');
          const peer = join(directory, 'peer.mjs');
          const input = join(directory, 'input.txt');
          await writeFile(
            peer,
            `import { appendFileSync } from 'node:fs';
process.stdin.setRawMode(true);
process.stdin.resume();
process.stdin.on('data', bytes => appendFileSync(process.argv[2], bytes));
process.stdout.write('LIMIT');
`
          );
          await writeFile(
            config,
            `[terminal]\nfont_zoom_mode=preserve-window\nfont_size_dip=${limit}\nwidth=30\nheight=4\n[local]\ncommand_line=${process.execPath} ${peer} ${input}\n`
          );
          await runGtkTest(
            context,
            ['-c', config],
            async (app) => {
              const initial = await waitForResult(() =>
                readWindowCellLayout(app)
              );
              expect(initial.fontSizeDip).toBe(limit);
              const bounds = (
                await (await app.getById('terminal_view')).capture()
              ).bounds;
              await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
              let sent = '';
              for (let step = 0; step < 3; step++) {
                await scrollWheelWithControl(app, limit === 6 ? 1 : -1);
                await app.input.pressKey('x');
                sent += 'x';
                await waitForResult(async () => {
                  expect(await readFile(input, 'utf8')).toBe(sent);
                  const layout = await readWindowCellLayout(app);
                  expect(layout.fontSizeDip).toBe(limit);
                  expect(layout.mainBounds).toEqual(initial.mainBounds);
                  expect([layout.columns, layout.rows]).toEqual([
                    initial.columns,
                    initial.rows,
                  ]);
                });
              }
              await scrollWheelWithControl(app, limit === 6 ? -1 : 1);
              await waitForResult(async () => {
                const layout = await readWindowCellLayout(app);
                if (limit === 6)
                  expect(layout.fontSizeDip).toBeGreaterThan(limit);
                else expect(layout.fontSizeDip).toBeLessThan(limit);
                expect(layout.mainBounds).toEqual(initial.mainBounds);
              });
            },
            {
              env: {
                GDK_GL: backend === 'cairo' ? 'disable' : 'always',
                LIBGL_ALWAYS_SOFTWARE: '1',
              },
            }
          );
        });
      });
    }
  }
});
