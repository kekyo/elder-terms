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
      it(`keeps a large colored local terminal usable at minimum zoom (${backend}, compact=${compact})`, async (context) => {
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
            `[general]\ncompact_mode=${compact}\nbackground=#2B1212\nshow_border=true\n[terminal]\nfont_families=Inconsolata;Noto Sans Mono CJK JP;\ncursor_shape=bar\nwidth=111\nheight=31\n[local]\ncommand_line=${process.execPath} ${peer} ${input}\n`
          );
          await runGtkTest(
            context,
            ['--test-maximize-window', '-c', config],
            async (app, evidence) => {
              let current = await waitForResult(() =>
                readWindowCellLayout(app)
              );
              // Keep the actual allocation fixed, as a maximized window does:
              // zooming out must increase the grid instead of shrinking the window.
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
              ]).toEqual([initial.mainBounds.width, initial.mainBounds.height]);
              expect(capturePixel(await terminal.capture(), 0.9, 0.9)).toEqual([
                43, 18, 18,
              ]);
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
});
