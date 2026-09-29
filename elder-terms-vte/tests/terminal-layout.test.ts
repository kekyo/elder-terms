import { setTimeout as delay } from 'node:timers/promises';
import { writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import type { PNG as PngImage } from 'pngjs';
import {
  assertTerminalTextGridMatches,
  constrainedFontZoomSteps,
  defaultColumns,
  defaultRows,
  expectFixtureVteGridSize,
  expectWindowCellSize,
  moveMouseToTerminalCenter,
  pressKeyWithModifiers,
  rapidFontZoomBurstSteps,
  readTerminalGridLayout,
  readWindowCellLayout,
  runGtkTest,
  saveTerminalGridLayoutEvidence,
  scrollWheelBurstWithControl,
  scrollWheelWithControl,
  terminalTextGrid80x24FontScale11Path,
  terminalTextGrid80x24Path,
  terminalTextGrid81x25Path,
  withTemporaryDirectory,
} from './gtk-test-helpers';
import { capturePixel } from './test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

describe.concurrent('elder-terms-vte terminal layout', () => {
  it('centers the title text in the title bar', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'centered-title.ini');
      await writeFile(configPath, '[general]\nname=Centered title\n', 'utf8');
      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          const capture = await (
            await app.getById('header_title_label')
          ).capture();
          const png = PNG.sync.read(capture.image) as PngImage;
          const inkColumns: number[] = [];
          for (let x = 0; x < png.width; ++x) {
            for (let y = 1; y < png.height - 1; ++y) {
              const pixel = (y * png.width + x) * 4;
              const background = (y * png.width + png.width - 1) * 4;
              const difference =
                Math.abs(png.data[pixel]! - png.data[background]!) +
                Math.abs(png.data[pixel + 1]! - png.data[background + 1]!) +
                Math.abs(png.data[pixel + 2]! - png.data[background + 2]!);
              if (difference > 120) {
                inkColumns.push(x);
                break;
              }
            }
          }
          expect(inkColumns.length).toBeGreaterThan(10);
          expect(
            Math.abs(
              (inkColumns[0]! + inkColumns[inkColumns.length - 1]!) / 2 -
                png.width / 2
            )
          ).toBeLessThan(12);
        }
      );
    });
  });

  it('expands a long title without wrapping in regular mode', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'long-title.ini');
      await writeFile(
        configPath,
        `[general]\nname=${'A long connection title '.repeat(20)}\n`,
        'utf8'
      );
      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          const initial = await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, defaultColumns, defaultRows);
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            return layout;
          });
          expect(
            (await (await app.getById('status_bar')).info()).states
          ).toContain('showing');
          const title = await (
            await app.getById('header_title_label')
          ).capture();
          await initial.mainWindow.resizeTo(
            initial.mainBounds.width + initial.hints.widthIncrement,
            initial.mainBounds.height
          );
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows);
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows
            );
          });
          const expanded = await (
            await app.getById('header_title_label')
          ).capture();
          expect(expanded.bounds.width).toBeGreaterThan(title.bounds.width);
          expect(expanded.bounds.height).toBe(title.bounds.height);
        }
      );
    });
  });

  it('uses the title bar indicators and a bottom border in compact mode while retaining cell geometry', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'compact.ini');
      await writeFile(
        configPath,
        [
          '[general]',
          `name=${'A long connection title '.repeat(20)}`,
          'compact_mode=true',
          'exterior_background=#800000',
          '',
          '[terminal]',
          'show_border=true',
          'border_width=7',
          '',
        ].join('\n'),
        'utf8'
      );
      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          const layout = await waitForResult(async () => {
            const current = await readWindowCellLayout(app);
            expectWindowCellSize(current, defaultColumns, defaultRows);
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            return current;
          });
          expect(
            (await (await app.getById('status_bar')).info()).states
          ).not.toContain('showing');
          const bottom = await (
            await app.getById('frame_bottom_border')
          ).capture();
          expect(bottom.bounds.height).toBe(7);
          expect(capturePixel(bottom, 0.5, 0.5)).toStrictEqual([128, 0, 0]);
          const side = await (await app.getById('frame_end_border')).capture();
          expect(bottom.bounds.x + bottom.bounds.width).toBe(side.bounds.x);
          expect(bottom.bounds.y + bottom.bounds.height).toBe(
            side.bounds.y + side.bounds.height
          );
          expect(
            (await (await app.getById('activity_indicator_bar')).info()).states
          ).toContain('showing');
          const [title, indicators, menu] = await Promise.all([
            (await app.getById('header_title_label')).capture(),
            (await app.getById('activity_indicator_bar')).capture(),
            (await app.getById('application_menu_button')).capture(),
          ]);
          expect(title.bounds.x + title.bounds.width).toBeLessThanOrEqual(
            indicators.bounds.x
          );
          expect(
            indicators.bounds.x + indicators.bounds.width
          ).toBeLessThanOrEqual(menu.bounds.x);
          await layout.mainWindow.resizeTo(
            layout.mainBounds.width + layout.hints.widthIncrement,
            layout.mainBounds.height + layout.hints.heightIncrement
          );
          await waitForResult(async () => {
            const current = await readWindowCellLayout(app);
            expectWindowCellSize(current, defaultColumns + 1, defaultRows + 1);
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
          });
          const expandedTitle = await (
            await app.getById('header_title_label')
          ).capture();
          expect(expandedTitle.bounds.width).toBeGreaterThan(
            title.bounds.width
          );
          expect(expandedTitle.bounds.height).toBe(title.bounds.height);
          await pressKeyWithModifiers(app, ['control'], 'equal');
          const zoomed = await waitForResult(async () => {
            const current = await readWindowCellLayout(app);
            expect(current.hints.widthIncrement).not.toBe(
              layout.hints.widthIncrement
            );
            expectWindowCellSize(current, defaultColumns + 1, defaultRows + 1);
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
            return current;
          });
          await zoomed.mainWindow.resizeTo(
            zoomed.mainBounds.width + zoomed.hints.widthIncrement - 1,
            zoomed.mainBounds.height + zoomed.hints.heightIncrement - 1
          );
          await waitForResult(async () => {
            const current = await readWindowCellLayout(app);
            expectWindowCellSize(current, defaultColumns + 1, defaultRows + 1);
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
          });
        }
      );
    });
  });

  it('fills the whole terminal image up to the right and bottom edges', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const layout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });
      await saveTerminalGridLayoutEvidence(evidence, layout, 'initial-layout');
      await assertTerminalTextGridMatches(
        layout.terminal,
        'fixture-terminal-initial',
        terminalTextGrid80x24Path,
        evidence
      );
    });
  });

  it('resizes to the requested whole-cell window size', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.hints.widthIncrement,
        initialLayout.mainBounds.height + initialLayout.hints.heightIncrement
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
        await expectFixtureVteGridSize(
          app,
          defaultColumns + 1,
          defaultRows + 1
        );
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'whole-cell-resize-layout'
      );
      await assertTerminalTextGridMatches(
        resizedLayout.terminal,
        'fixture-terminal-whole-cell-resize',
        terminalTextGrid81x25Path,
        evidence
      );
    });
  });

  it('keeps whole-cell geometry while showing colored window side borders', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'window-side-borders.ini');
      await writeFile(
        configPath,
        '[general]\nexterior_background=#800000\n\n[terminal]\nshow_border=true\nborder_width=7\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app, evidence) => {
          const startBorder = await app.getById('frame_start_border');
          const endBorder = await app.getById('frame_end_border');
          const root = await app.getById('root_box');

          const assertBorders = async (): Promise<void> => {
            const [startCapture, endCapture, rootCapture, scrollerCapture] =
              await Promise.all([
                startBorder.capture(),
                endBorder.capture(),
                root.capture(),
                (await app.getById('terminal_scroller')).capture(),
              ]);

            expect(startCapture.bounds.width).toBe(7);
            expect(endCapture.bounds.width).toBe(7);
            expect(startCapture.bounds.x).toBe(rootCapture.bounds.x);
            expect(startCapture.bounds.y).toBe(rootCapture.bounds.y);
            expect(startCapture.bounds.height).toBe(rootCapture.bounds.height);
            expect(startCapture.bounds.x + startCapture.bounds.width).toBe(
              scrollerCapture.bounds.x
            );
            expect(endCapture.bounds.x).toBe(
              scrollerCapture.bounds.x + scrollerCapture.bounds.width
            );
            expect(endCapture.bounds.y).toBe(rootCapture.bounds.y);
            expect(endCapture.bounds.height).toBe(rootCapture.bounds.height);
            expect(endCapture.bounds.x + endCapture.bounds.width).toBe(
              rootCapture.bounds.x + rootCapture.bounds.width
            );
            expect(capturePixel(startCapture, 0.5, 0.5)).toStrictEqual([
              128, 0, 0,
            ]);
            expect(capturePixel(endCapture, 0.5, 0.5)).toStrictEqual([
              128, 0, 0,
            ]);

            await Promise.all([
              evidence.captureEvidence(
                'window-start-border',
                async () => startCapture
              ),
              evidence.captureEvidence(
                'window-end-border',
                async () => endCapture
              ),
            ]);
          };

          const initialLayout = await waitForResult(async () => {
            const layout = await readTerminalGridLayout(app);
            expectWindowCellSize(layout, defaultColumns, defaultRows);
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            return layout;
          });
          await assertBorders();
          await assertTerminalTextGridMatches(
            initialLayout.terminal,
            'fixture-terminal-bordered-initial',
            terminalTextGrid80x24Path,
            evidence
          );

          await initialLayout.mainWindow.resizeTo(
            initialLayout.mainBounds.width + initialLayout.hints.widthIncrement,
            initialLayout.mainBounds.height +
              initialLayout.hints.heightIncrement
          );
          const resizedLayout = await waitForResult(async () => {
            const layout = await readTerminalGridLayout(app);
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
            return layout;
          });
          await assertTerminalTextGridMatches(
            resizedLayout.terminal,
            'fixture-terminal-bordered-resized',
            terminalTextGrid81x25Path,
            evidence
          );

          await moveMouseToTerminalCenter(app, resizedLayout);
          await scrollWheelWithControl(app, -1);
          await waitForResult(async () => {
            const layout = await readTerminalGridLayout(app);
            expect(layout.hints.widthIncrement).not.toBe(
              resizedLayout.hints.widthIncrement
            );
            expect(layout.hints.heightIncrement).not.toBe(
              resizedLayout.hints.heightIncrement
            );
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
          });
          await assertBorders();
        }
      );
    });
  });

  it('lets repeated border-like resize steps accumulate before snapping to cells', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      for (
        let index = 0;
        index <= initialLayout.hints.widthIncrement;
        ++index
      ) {
        const bounds = await initialLayout.mainWindow.bounds();
        await initialLayout.mainWindow.resizeTo(
          bounds.width + 1,
          bounds.height
        );
        await delay(10);
      }

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns + 1, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'incremental-border-resize-layout'
      );
    });
  });

  it('keeps the resized VTE grid size after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      const resizedColumns = defaultColumns + 1;
      const resizedRows = defaultRows + 1;
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.hints.widthIncrement,
        initialLayout.mainBounds.height + initialLayout.hints.heightIncrement
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
        return layout;
      });

      await moveMouseToTerminalCenter(app, resizedLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          resizedLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).not.toBe(
          resizedLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        zoomedLayout,
        'resized-font-zoom-layout'
      );
    });
  });

  it('rounds a fractional-cell window resize request to a whole-cell size', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      expect(initialLayout.hints.widthIncrement).toBeGreaterThan(1);
      expect(initialLayout.hints.heightIncrement).toBeGreaterThan(1);
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.hints.widthIncrement - 1,
        initialLayout.mainBounds.height +
          initialLayout.hints.heightIncrement -
          1
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.mainBounds.width).not.toBe(
          initialLayout.mainBounds.width +
            initialLayout.hints.widthIncrement -
            1
        );
        expect(layout.mainBounds.height).not.toBe(
          initialLayout.mainBounds.height +
            initialLayout.hints.heightIncrement -
            1
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'fractional-cell-resize-layout'
      );
      await assertTerminalTextGridMatches(
        resizedLayout.terminal,
        'fixture-terminal-fractional-cell-resize',
        terminalTextGrid80x24Path,
        evidence
      );
    });
  });

  it('keeps the window constrained to whole VTE cells after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).not.toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        zoomedLayout,
        'font-zoom-layout'
      );
      await assertTerminalTextGridMatches(
        zoomedLayout.terminal,
        'fixture-terminal-font-zoom',
        terminalTextGrid80x24FontScale11Path,
        evidence
      );
    });
  });

  it('lets repeated border-like resize steps accumulate after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          initialLayout.hints.widthIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await delay(250);

      for (let index = 0; index <= zoomedLayout.hints.widthIncrement; ++index) {
        const bounds = await zoomedLayout.mainWindow.bounds();
        await zoomedLayout.mainWindow.resizeTo(bounds.width + 1, bounds.height);
        await delay(10);
      }

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns + 1, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'zoomed-incremental-border-resize-layout'
      );
    });
  });

  it('keeps the VTE grid size during repeated Ctrl+wheel font zoom steps', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      let currentLayout = initialLayout;
      for (const ySteps of [-1, -1, -1, 1, 1, 1]) {
        await scrollWheelWithControl(app, ySteps);
        await delay(25);
        currentLayout = await waitForResult(async () => {
          const layout = await readTerminalGridLayout(app);
          expectWindowCellSize(layout, defaultColumns, defaultRows);
          await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
          return layout;
        });
      }

      await saveTerminalGridLayoutEvidence(
        evidence,
        currentLayout,
        'repeated-font-zoom-steps-layout'
      );
    });
  });

  it('zooms with the default Ctrl+equal and Ctrl+minus bindings while keeping the VTE grid size', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });

      await pressKeyWithModifiers(app, ['control'], 'equal');
      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          initialLayout.hints.widthIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });

      await pressKeyWithModifiers(app, ['control'], 'minus');
      const restoredLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          zoomedLayout.hints.widthIncrement
        );
        expect(layout.hints.widthIncrement).toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        restoredLayout,
        'keyboard-font-zoom-restored-layout'
      );
    });
  });

  it('requires configured keyboard modifiers to match exactly', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'strict-key-bindings.ini');
      await writeFile(
        configPath,
        '[terminal]\nzoom_in_key=shift+plus\nzoom_out_key=ctrl+shift+underscore\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          const initialLayout = await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, defaultColumns, defaultRows);
            return layout;
          });

          await pressKeyWithModifiers(app, ['control', 'shift'], 'plus');
          await delay(100);
          const unmatchedLayout = await readWindowCellLayout(app);
          expect(unmatchedLayout.hints.widthIncrement).toBe(
            initialLayout.hints.widthIncrement
          );
          expect(unmatchedLayout.hints.heightIncrement).toBe(
            initialLayout.hints.heightIncrement
          );

          await pressKeyWithModifiers(app, ['shift'], 'plus');
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expect(layout.hints.widthIncrement).not.toBe(
              initialLayout.hints.widthIncrement
            );
          });

          await pressKeyWithModifiers(app, ['control', 'shift'], 'underscore');
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expect(layout.hints.widthIncrement).toBe(
              initialLayout.hints.widthIncrement
            );
            expect(layout.hints.heightIncrement).toBe(
              initialLayout.hints.heightIncrement
            );
          });
        }
      );
    });
  });

  it('keeps terminal text stable after rapid Ctrl+wheel zoom in and out burst', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelBurstWithControl(app, [
        -rapidFontZoomBurstSteps,
        rapidFontZoomBurstSteps,
        -rapidFontZoomBurstSteps,
        rapidFontZoomBurstSteps,
      ]);
      await delay(100);

      const restoredLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        restoredLayout,
        'rapid-font-zoom-burst-restored-layout'
      );
      await assertTerminalTextGridMatches(
        restoredLayout.terminal,
        'fixture-terminal-rapid-font-zoom-burst-restored',
        terminalTextGrid80x24Path,
        evidence
      );
    });
  });

  it('keeps the user resized VTE grid size after returning from font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -constrainedFontZoomSteps);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.hints.widthIncrement).toBeGreaterThan(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBeGreaterThan(
          initialLayout.hints.heightIncrement
        );
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await evidence.log('rapid font zoom in layout verified', {
        hints: zoomedLayout.hints,
        mainBounds: zoomedLayout.mainBounds,
      });
      await delay(250);

      await zoomedLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width,
        initialLayout.mainBounds.height
      );
      const constrainedLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.hints.widthIncrement).toBe(
          zoomedLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          zoomedLayout.hints.heightIncrement
        );
        return layout;
      });
      const resizedColumns =
        (constrainedLayout.mainBounds.width -
          constrainedLayout.hints.baseWidth) /
        constrainedLayout.hints.widthIncrement;
      const resizedRows =
        (constrainedLayout.mainBounds.height -
          constrainedLayout.hints.baseHeight) /
        constrainedLayout.hints.heightIncrement;
      expect(resizedColumns < defaultColumns || resizedRows < defaultRows).toBe(
        true
      );
      await toPass(async () => {
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
      });
      await evidence.log('font zoom constrained layout verified', {
        hints: constrainedLayout.hints,
        mainBounds: constrainedLayout.mainBounds,
      });

      await scrollWheelWithControl(app, constrainedFontZoomSteps);

      const restoredLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
        return layout;
      });
      await evidence.log('rapid font zoom restored layout verified', {
        hints: restoredLayout.hints,
        mainBounds: restoredLayout.mainBounds,
      });
    });
  });
});
