// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { setTimeout as delay } from 'node:timers/promises';
import { writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import type { GtkWidgetElement } from 'gestament';
import { waitForResult, toPass } from 'gestament/testing';
import type { PNG as PngImage } from 'pngjs';
import {
  assertTerminalTextGridMatches,
  constrainedFontZoomSteps,
  defaultColumns,
  defaultRows,
  expectFixtureTerminalGridSize,
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
  terminalTextGrid80x24VisualStepPath,
  terminalTextGrid80x24Path,
  terminalTextGrid81x25Path,
  terminalPlainTextGridConfigPath,
} from './gtk-test-helpers';
import {
  withTemporaryDirectory,
  capturePixel,
} from '../../shared/tests/connection-test-helpers.ts';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

describe.concurrent('elder-terms-terminal terminal layout', () => {
  for (const { theme, scale } of [
    { theme: 'Adwaita', scale: 1 },
    { theme: 'Adwaita:dark', scale: 1 },
    { theme: 'Adwaita', scale: 2 },
    { theme: 'Yaru', scale: 1 },
    { theme: 'Yaru-dark', scale: 1 },
    { theme: 'Yaru-dark', scale: 2 },
  ]) {
    it(`shrinks the compact title bar to 75% with intact serial indicators and controls (${theme}, scale ${scale})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const configPath = join(directory, 'compact-header.ini');
        const settings = '[general]\ntype=serial\nname=Compact header\n';
        const terminalSettings = '\n[terminal]\nwidth=72\nheight=12\n';
        const env = { GTK_THEME: theme, GDK_SCALE: String(scale) };
        await writeFile(configPath, settings + terminalSettings, 'utf8');
        let regularHeight = 0;
        let regularConnectionIconSize = 0;
        await runGtkTest(
          context,
          ['--test-fixture', '-c', configPath],
          async (app, evidence) => {
            const header = await (await app.getById('header_bar')).capture();
            regularHeight = header.bounds.height;
            regularConnectionIconSize = (
              await (await app.getById('header_connection_icon')).capture()
            ).bounds.width;
            await evidence.captureEvidence('regular-serial-screen', async () =>
              app.capture()
            );
          },
          { env }
        );

        await writeFile(
          configPath,
          settings + 'compact_mode=true\n' + terminalSettings,
          'utf8'
        );
        await runGtkTest(
          context,
          ['--test-fixture', '-c', configPath],
          async (app, evidence) => {
            const layout = await waitForResult(async () => {
              const current = await readWindowCellLayout(app);
              expectWindowCellSize(current, 72, 12);
              await expectFixtureTerminalGridSize(app, 72, 12);
              return current;
            });
            const header = await (await app.getById('header_bar')).capture();
            const screen = await evidence.captureEvidence(
              'compact-serial-screen',
              async () => app.capture()
            );
            expect(header.clipped).toBe(false);
            // GTK3 AT-SPI bounds use logical pixels; X11 input and screen
            // captures use physical pixels, including on HiDPI displays.
            expect(header.bounds.height).toBe(Math.round(regularHeight * 0.75));
            expect(
              (await (await app.getById('header_connection_icon')).capture())
                .bounds.width
            ).toBeLessThan(regularConnectionIconSize);
            expect(
              layout.mainBounds.x + layout.mainBounds.width
            ).toBeLessThanOrEqual(screen.bounds.width);
            expect(
              layout.mainBounds.y + layout.mainBounds.height
            ).toBeLessThanOrEqual(screen.bounds.height);
            const controls = [
              'header_title_label',
              'activity_indicator_bar',
              'transfer_button',
              'application_menu_button',
            ];
            let previousEnd = header.bounds.x;
            for (const id of controls) {
              const capture = await (await app.getById(id)).capture();
              expect(capture.clipped, id).toBe(false);
              expect(capture.bounds.x, id).toBeGreaterThanOrEqual(previousEnd);
              expect(capture.bounds.y, id).toBeGreaterThanOrEqual(
                header.bounds.y
              );
              expect(
                capture.bounds.y + capture.bounds.height,
                id
              ).toBeLessThanOrEqual(header.bounds.y + header.bounds.height);
              previousEnd = capture.bounds.x + capture.bounds.width;
            }
            for (const indicator of [
              'conn',
              'log',
              'sd',
              'rd',
              'rts',
              'cts',
              'dtr',
              'dsr',
              'cd',
              'ri',
            ]) {
              const image = await (
                await app.getById(`${indicator}_indicator_image`)
              ).capture();
              const label = await (
                await app.getById(`${indicator}_indicator_label`)
              ).capture();
              expect(image.bounds.width).toBe(18);
              expect(image.bounds.height).toBe(18);
              expect(image.bounds.y).toBeGreaterThanOrEqual(header.bounds.y);
              expect(image.bounds.y + image.bounds.height).toBeLessThanOrEqual(
                label.bounds.y
              );
              expect(label.bounds.height).toBeGreaterThan(0);
              expect(label.bounds.y + label.bounds.height).toBeLessThanOrEqual(
                header.bounds.y + header.bounds.height
              );
            }
            const pending: GtkWidgetElement[] = [
              await app.getById('header_bar'),
            ];
            const buttons = [
              await app.getById('transfer_button'),
              await app.getById('application_menu_button'),
            ];
            let closeButton;
            while (pending.length !== 0) {
              const widget = pending.shift()!;
              if (widget.kind === 'button') {
                buttons.push(widget);
                const capture = await widget.capture();
                expect(capture.bounds.x).toBeGreaterThanOrEqual(previousEnd);
                expect(capture.bounds.y).toBeGreaterThanOrEqual(
                  header.bounds.y
                );
                expect(
                  capture.bounds.x + capture.bounds.width
                ).toBeLessThanOrEqual(header.bounds.x + header.bounds.width);
                expect(
                  capture.bounds.y + capture.bounds.height
                ).toBeLessThanOrEqual(header.bounds.y + header.bounds.height);
                if ((await widget.info()).name === 'Close') {
                  closeButton = widget;
                }
              }
              if ('getChildCount' in widget) {
                const count = await widget.getChildCount();
                for (let index = 0; index < count; ++index) {
                  const child = await widget.childAt(index);
                  if (child !== undefined) pending.push(child);
                }
              }
            }
            expect(closeButton).toBeDefined();
            if (closeButton === undefined)
              throw new Error('Missing title-bar close button');
            const close = await closeButton.capture();

            const menu = await (
              await app.getById('application_menu_button')
            ).capture();
            for (const button of buttons) {
              const { bounds } = await button.capture();
              const name = (await button.info()).name;
              // Both application and window controls should stay square,
              // similarly sized, and clear of the title bar's edges.
              expect(bounds.width, name).toBe(bounds.height);
              expect(bounds.height, name).toBe(menu.bounds.height);
              expect(bounds.height, name).toBeGreaterThanOrEqual(24);
              expect(bounds.y - header.bounds.y, name).toBeGreaterThanOrEqual(
                2
              );
              expect(
                header.bounds.y +
                  header.bounds.height -
                  bounds.y -
                  bounds.height,
                name
              ).toBeGreaterThanOrEqual(2);
            }

            for (const [buttonId, itemId] of [
              ['application_menu_button', 'settings_menu_item'],
              ['transfer_button', 'transfer_log_enabled_item'],
            ] as const) {
              const button = await (await app.getById(buttonId)).capture();
              await app.input.moveMouseTo(
                Math.trunc((button.bounds.x + button.bounds.width / 2) * scale),
                Math.trunc((button.bounds.y + button.bounds.height / 2) * scale)
              );
              await app.input.setMouseButton('left', true);
              await app.input.setMouseButton('left', false);
              await waitForResult(async () => {
                expect(
                  (await (await app.getById(itemId)).info()).states
                ).toContain('showing');
              });
              await app.input.pressKey('Escape');
            }
            await expectFixtureTerminalGridSize(app, 72, 12);
            expectWindowCellSize(await readWindowCellLayout(app), 72, 12);
            await app.input.moveMouseTo(
              Math.trunc((close.bounds.x + close.bounds.width / 2) * scale),
              Math.trunc((close.bounds.y + close.bounds.height / 2) * scale)
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await waitForResult(async () =>
              expect((await app.output()).exitCode).toBe(0)
            );
          },
          { env }
        );
      });
    });
  }

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
            await expectFixtureTerminalGridSize(
              app,
              defaultColumns,
              defaultRows
            );
            return layout;
          });
          expect(
            (await (await app.getById('status_bar')).info()).states
          ).toContain('showing');
          const title = await (
            await app.getById('header_title_label')
          ).capture();
          await initial.mainWindow.resizeTo(
            initial.mainBounds.width + initial.cellWidth,
            initial.mainBounds.height
          );
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows);
            await expectFixtureTerminalGridSize(
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
          'show_border=true',
          'border_width=7',
          'exterior_background=#800000',
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
            await expectFixtureTerminalGridSize(
              app,
              defaultColumns,
              defaultRows
            );
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
            layout.mainBounds.width + layout.cellWidth,
            layout.mainBounds.height + layout.cellHeight
          );
          await waitForResult(async () => {
            const current = await readWindowCellLayout(app);
            expectWindowCellSize(current, defaultColumns + 1, defaultRows + 1);
            await expectFixtureTerminalGridSize(
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
            expect(current.fontSizeDip).not.toBe(layout.fontSizeDip);
            expect([current.cellWidth, current.cellHeight]).not.toEqual([
              layout.cellWidth,
              layout.cellHeight,
            ]);
            expectWindowCellSize(current, defaultColumns + 1, defaultRows + 1);
            await expectFixtureTerminalGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
            return current;
          });
          await zoomed.mainWindow.resizeTo(
            zoomed.mainBounds.width + zoomed.cellWidth - 1,
            zoomed.mainBounds.height + zoomed.cellHeight - 1
          );
          await waitForResult(async () => {
            const current = await readWindowCellLayout(app);
            expectWindowCellSize(current, defaultColumns + 1, defaultRows + 1);
            await expectFixtureTerminalGridSize(
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
    await runGtkTest(
      context,
      ['--test-fixture', '-c', terminalPlainTextGridConfigPath],
      async (app, evidence) => {
        const layout = await waitForResult(async () =>
          readTerminalGridLayout(app)
        );
        await toPass(async () => {
          await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
        });
        await saveTerminalGridLayoutEvidence(
          evidence,
          layout,
          'initial-layout'
        );
        await assertTerminalTextGridMatches(
          layout.terminal,
          'fixture-terminal-initial',
          terminalTextGrid80x24Path,
          evidence
        );
      }
    );
  });

  it('resizes to the requested whole-cell window size', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '-c', terminalPlainTextGridConfigPath],
      async (app, evidence) => {
        const initialLayout = await waitForResult(async () =>
          readTerminalGridLayout(app)
        );
        expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
        await toPass(async () => {
          await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
        });
        await initialLayout.mainWindow.resizeTo(
          initialLayout.mainBounds.width + initialLayout.cellWidth,
          initialLayout.mainBounds.height + initialLayout.cellHeight
        );

        const resizedLayout = await waitForResult(async () => {
          const layout = await readTerminalGridLayout(app);
          expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
          await expectFixtureTerminalGridSize(
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
      }
    );
  });

  it('keeps whole-cell geometry while showing colored window side borders', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'window-side-borders.ini');
      await writeFile(
        configPath,
        '[general]\nexterior_background=#800000\nshow_border=true\nborder_width=7\n\n[hyperlink]\nenabled=false\n',
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
            await expectFixtureTerminalGridSize(
              app,
              defaultColumns,
              defaultRows
            );
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
            initialLayout.mainBounds.width + initialLayout.cellWidth,
            initialLayout.mainBounds.height + initialLayout.cellHeight
          );
          const resizedLayout = await waitForResult(async () => {
            const layout = await readTerminalGridLayout(app);
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
            await expectFixtureTerminalGridSize(
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
            expect(layout.fontSizeDip).not.toBe(resizedLayout.fontSizeDip);
            expect([layout.cellWidth, layout.cellHeight]).not.toEqual([
              resizedLayout.cellWidth,
              resizedLayout.cellHeight,
            ]);
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
            await expectFixtureTerminalGridSize(
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
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
      });

      for (let index = 0; index <= initialLayout.cellWidth; ++index) {
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
        await expectFixtureTerminalGridSize(
          app,
          defaultColumns + 1,
          defaultRows
        );
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'incremental-border-resize-layout'
      );
    });
  });

  it('keeps the resized terminal grid size after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
      });

      const resizedColumns = defaultColumns + 1;
      const resizedRows = defaultRows + 1;
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.cellWidth,
        initialLayout.mainBounds.height + initialLayout.cellHeight
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureTerminalGridSize(app, resizedColumns, resizedRows);
        return layout;
      });

      await moveMouseToTerminalCenter(app, resizedLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.fontSizeDip).not.toBe(resizedLayout.fontSizeDip);
        expect([layout.cellWidth, layout.cellHeight]).not.toEqual([
          resizedLayout.cellWidth,
          resizedLayout.cellHeight,
        ]);
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureTerminalGridSize(app, resizedColumns, resizedRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        zoomedLayout,
        'resized-font-zoom-layout'
      );
    });
  });

  it('retains a fractional-cell allocation while committing the fitted grid', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      expect(initialLayout.cellWidth).toBeGreaterThan(1);
      expect(initialLayout.cellHeight).toBeGreaterThan(1);
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.cellWidth - 1,
        initialLayout.mainBounds.height + initialLayout.cellHeight - 1
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.mainBounds.width).toBe(
          initialLayout.mainBounds.width + initialLayout.cellWidth - 1
        );
        expect(layout.mainBounds.height).toBe(
          initialLayout.mainBounds.height + initialLayout.cellHeight - 1
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'fractional-cell-resize-layout'
      );
      expect(resizedLayout.terminalCapture.bounds.width).toBe(
        defaultColumns * resizedLayout.cellWidth + resizedLayout.cellWidth - 1
      );
    });
  });

  it('keeps the window constrained to whole terminal cells after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '-c', terminalPlainTextGridConfigPath],
      async (app, evidence) => {
        const initialLayout = await waitForResult(async () =>
          readTerminalGridLayout(app)
        );
        expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
        await toPass(async () => {
          await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
        });

        await moveMouseToTerminalCenter(app, initialLayout);
        await scrollWheelWithControl(app, -1);

        const zoomedLayout = await waitForResult(async () => {
          const layout = await readTerminalGridLayout(app);
          expect(layout.fontSizeDip).not.toBe(initialLayout.fontSizeDip);
          expect([layout.cellWidth, layout.cellHeight]).not.toEqual([
            initialLayout.cellWidth,
            initialLayout.cellHeight,
          ]);
          expectWindowCellSize(layout, defaultColumns, defaultRows);
          await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
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
          terminalTextGrid80x24VisualStepPath,
          evidence
        );
      }
    );
  });

  it('lets repeated border-like resize steps accumulate after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.fontSizeDip).not.toBe(initialLayout.fontSizeDip);
        expect([layout.cellWidth, layout.cellHeight]).not.toEqual([
          initialLayout.cellWidth,
          initialLayout.cellHeight,
        ]);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await delay(250);

      for (let index = 0; index <= zoomedLayout.cellWidth; ++index) {
        const bounds = await zoomedLayout.mainWindow.bounds();
        await zoomedLayout.mainWindow.resizeTo(bounds.width + 1, bounds.height);
        await delay(10);
      }

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows);
        await expectFixtureTerminalGridSize(
          app,
          defaultColumns + 1,
          defaultRows
        );
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'zoomed-incremental-border-resize-layout'
      );
    });
  });

  it('keeps the terminal grid size during repeated Ctrl+wheel font zoom steps', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      let currentLayout = initialLayout;
      for (const ySteps of [-1, -1, -1, 1, 1, 1]) {
        await scrollWheelWithControl(app, ySteps);
        await delay(25);
        currentLayout = await waitForResult(async () => {
          const layout = await readTerminalGridLayout(app);
          expectWindowCellSize(layout, defaultColumns, defaultRows);
          await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
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

  it('zooms with the default Ctrl+equal and Ctrl+minus bindings while keeping the terminal grid size', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
        return layout;
      });

      await pressKeyWithModifiers(app, ['control'], 'equal');
      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.fontSizeDip).not.toBe(initialLayout.fontSizeDip);
        expect([layout.cellWidth, layout.cellHeight]).not.toEqual([
          initialLayout.cellWidth,
          initialLayout.cellHeight,
        ]);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
        return layout;
      });

      await pressKeyWithModifiers(app, ['control'], 'minus');
      const restoredLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.fontSizeDip).not.toBe(zoomedLayout.fontSizeDip);
        expect([layout.cellWidth, layout.cellHeight]).not.toEqual([
          zoomedLayout.cellWidth,
          zoomedLayout.cellHeight,
        ]);
        expect(layout.cellWidth).toBe(initialLayout.cellWidth);
        expect(layout.cellHeight).toBe(initialLayout.cellHeight);
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
          expect(unmatchedLayout.cellWidth).toBe(initialLayout.cellWidth);
          expect(unmatchedLayout.cellHeight).toBe(initialLayout.cellHeight);

          await pressKeyWithModifiers(app, ['shift'], 'plus');
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expect(layout.fontSizeDip).not.toBe(initialLayout.fontSizeDip);
            expect([layout.cellWidth, layout.cellHeight]).not.toEqual([
              initialLayout.cellWidth,
              initialLayout.cellHeight,
            ]);
          });

          await pressKeyWithModifiers(app, ['control', 'shift'], 'underscore');
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expect(layout.cellWidth).toBe(initialLayout.cellWidth);
            expect(layout.cellHeight).toBe(initialLayout.cellHeight);
          });
        }
      );
    });
  });

  it('keeps terminal text stable after rapid Ctrl+wheel zoom in and out burst', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '-c', terminalPlainTextGridConfigPath],
      async (app, evidence) => {
        const initialLayout = await waitForResult(async () =>
          readTerminalGridLayout(app)
        );
        expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
        await toPass(async () => {
          await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
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
          expect(layout.cellWidth).toBe(initialLayout.cellWidth);
          expect(layout.cellHeight).toBe(initialLayout.cellHeight);
          expectWindowCellSize(layout, defaultColumns, defaultRows);
          await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
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
      }
    );
  });

  it('keeps the user resized terminal grid size after returning from font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -constrainedFontZoomSteps);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.cellWidth).toBeGreaterThan(initialLayout.cellWidth);
        expect(layout.cellHeight).toBeGreaterThan(initialLayout.cellHeight);
        await expectFixtureTerminalGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await evidence.log('rapid font zoom in layout verified', {
        cell: [zoomedLayout.cellWidth, zoomedLayout.cellHeight],
        mainBounds: zoomedLayout.mainBounds,
      });
      await delay(250);

      // Keep the reduced grid wide enough for the header controls after the
      // font returns to its original size.
      await zoomedLayout.mainWindow.resizeTo(
        Math.round(
          (initialLayout.mainBounds.width + zoomedLayout.mainBounds.width) / 2
        ),
        initialLayout.mainBounds.height
      );
      const constrainedLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.cellWidth).toBe(zoomedLayout.cellWidth);
        expect(layout.cellHeight).toBe(zoomedLayout.cellHeight);
        return layout;
      });
      const resizedColumns = constrainedLayout.columns;
      const resizedRows = constrainedLayout.rows;
      expect(resizedColumns < defaultColumns || resizedRows < defaultRows).toBe(
        true
      );
      await toPass(async () => {
        await expectFixtureTerminalGridSize(app, resizedColumns, resizedRows);
      });
      await evidence.log('font zoom constrained layout verified', {
        cell: [constrainedLayout.cellWidth, constrainedLayout.cellHeight],
        mainBounds: constrainedLayout.mainBounds,
      });

      await scrollWheelWithControl(app, constrainedFontZoomSteps);

      const restoredLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.cellWidth).toBe(initialLayout.cellWidth);
        expect(layout.cellHeight).toBe(initialLayout.cellHeight);
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureTerminalGridSize(app, resizedColumns, resizedRows);
        return layout;
      });
      await evidence.log('rapid font zoom restored layout verified', {
        cell: [restoredLayout.cellWidth, restoredLayout.cellHeight],
        mainBounds: restoredLayout.mainBounds,
      });
    });
  });
});
