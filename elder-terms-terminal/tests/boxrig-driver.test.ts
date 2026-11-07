// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { mkdir, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { createGtkAppLauncher } from 'gestament';
import { toPass, waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const execFileAsync = promisify(execFile);
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('boxrig GTK3 driver', () => {
  for (const backend of ['cairo', 'opengl']) {
    it(`preserves the normal window during zoom and adopts its effective grid on mode return (${backend})`, async () => {
      const launcher = createGtkAppLauncher({
        appPath: driverPath,
        args: ['--zoom-modes'],
        env: {
          LIBGL_ALWAYS_SOFTWARE: '1',
          GTK_IM_MODULE: 'gtk-im-context-simple',
          ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
        },
      });
      try {
        const app = await launcher.launch();
        const window = await app.getById('driver_window');
        const zoomIn = await app.getById('driver_zoom_in');
        const zoomOut = await app.getById('driver_zoom_out');
        const mode = await app.getById('driver_preserve_window');
        if (
          window.kind !== 'window' ||
          zoomIn.kind !== 'button' ||
          zoomOut.kind !== 'button' ||
          mode.kind !== 'checkbox'
        )
          throw new Error('Missing zoom controls');
        const read = async () => {
          const report =
            (await app.output()).stdout
              .trimEnd()
              .split('\n')
              .filter((line) => line.startsWith('backend='))
              .pop() ?? '';
          expect(report).toContain(`backend=${backend}`);
          expect(report).toContain('ready=1');
          const grid = /grid=(\d+)x(\d+)/.exec(report)!;
          return {
            columns: Number(grid[1]),
            rows: Number(grid[2]),
            font: Number(/font=([\d.]+)/.exec(report)?.[1]),
            bounds: await window.bounds(),
          };
        };
        const initial = await waitForResult(read);
        const initialOutput = (await app.output()).stdout;
        const allocation = [
          ...initialOutput.matchAll(/window_allocation=(\d+x\d+)/g),
        ].pop()?.[1];
        expect(allocation).toBeDefined();
        await mode.toggle();
        const terminal = await app.getById('driver_terminal');
        const area = (await terminal.capture()).bounds;
        const screen = PNG.sync.read((await app.capture()).image);
        const directory = fileURLToPath(
          new URL(`../../test-results/boxrig/zoom-${backend}/`, import.meta.url)
        );
        await mkdir(directory, { recursive: true });
        const frameBytes = screen.width * screen.height * 4;
        let pending = Buffer.alloc(0);
        const ink: number[] = [];
        const greenBottom: number[] = [];
        let lowestFrame: Buffer | undefined;
        let lowestBottom = 0;
        let recordingError = '';
        const environment = await app.environment();
        const recorder = spawn(
          'ffmpeg',
          [
            '-y',
            '-hide_banner',
            '-loglevel',
            'error',
            '-f',
            'x11grab',
            '-framerate',
            '60',
            '-draw_mouse',
            '0',
            '-video_size',
            `${screen.width}x${screen.height}`,
            '-i',
            environment.DISPLAY ?? '',
            '-map',
            '0:v',
            '-c:v',
            'ffv1',
            '-threads',
            '1',
            '-pix_fmt',
            'bgr0',
            '-fps_mode',
            'passthrough',
            `${directory}zoom.mkv`,
            '-map',
            '0:v',
            '-c:v',
            'rawvideo',
            '-threads',
            '1',
            '-pix_fmt',
            'bgr0',
            '-fps_mode',
            'passthrough',
            '-f',
            'rawvideo',
            'pipe:1',
          ],
          { env: environment, stdio: ['pipe', 'pipe', 'pipe'] }
        );
        const finished = new Promise<number | null>((resolve) => {
          recorder.once('error', (error) => {
            recordingError = error.message;
            resolve(null);
          });
          recorder.once('close', resolve);
        });
        recorder.stderr.on('data', (bytes: Buffer) => {
          recordingError += bytes.toString();
        });
        recorder.stdout.on('data', (bytes: Buffer) => {
          pending = Buffer.concat([pending, bytes]);
          while (pending.length >= frameBytes) {
            const frame = pending.subarray(0, frameBytes);
            pending = pending.subarray(frameBytes);
            let green = 0;
            let bottom = 0;
            for (let y = area.y; y < area.y + area.height; ++y) {
              for (let x = area.x; x < area.x + 350; ++x) {
                const offset = (y * screen.width + x) * 4;
                if (
                  frame[offset + 1] > 100 &&
                  frame[offset + 1] > frame[offset] * 1.5 &&
                  frame[offset + 1] > frame[offset + 2] * 1.3
                ) {
                  ++green;
                  bottom = y;
                }
              }
            }
            ink.push(green);
            greenBottom.push(bottom);
            if (bottom > lowestBottom) {
              lowestBottom = bottom;
              lowestFrame = Buffer.from(frame);
            }
          }
        });
        let enlarged = initial;
        try {
          await waitForResult(async () =>
            expect(ink.length).toBeGreaterThan(0)
          );
          await zoomIn.click();
          enlarged = await waitForResult(async () => {
            const state = await read();
            expect(state.font).toBeGreaterThan(initial.font);
            expect(state.bounds).toEqual(initial.bounds);
            expect(state.columns).toBeLessThan(initial.columns);
            expect(state.rows).toBeLessThan(initial.rows);
            return state;
          });
          await zoomOut.click();
          await waitForResult(async () => {
            const state = await read();
            expect(state).toEqual(initial);
          });
          await zoomIn.click();
          await waitForResult(async () =>
            expect(await read()).toEqual(enlarged)
          );
          for (let index = 0; index < 12; ++index) {
            await zoomOut.click();
            await waitForResult(async () =>
              expect(await read()).toEqual(initial)
            );
            await zoomIn.click();
            await waitForResult(async () =>
              expect(await read()).toEqual(enlarged)
            );
          }
          await writeFile(
            `${directory}enlarged.png`,
            (await window.capture()).image
          );
          const hints = await window.resizeHints();
          expect(hints.widthIncrement).toBeLessThanOrEqual(1);
          expect(hints.heightIncrement).toBeLessThanOrEqual(1);
        } finally {
          recorder.stdin.write('q\n');
          expect(await finished, recordingError).toBe(0);
          await writeFile(
            `${directory}frames.json`,
            JSON.stringify({ ink, greenBottom })
          );
          if (lowestFrame) {
            const sample = new PNG({
              width: screen.width,
              height: screen.height,
            });
            for (let offset = 0; offset < lowestFrame.length; offset += 4) {
              sample.data[offset] = lowestFrame[offset + 2];
              sample.data[offset + 1] = lowestFrame[offset + 1];
              sample.data[offset + 2] = lowestFrame[offset];
              sample.data[offset + 3] = 255;
            }
            await writeFile(
              `${directory}maximum-text-bottom.png`,
              PNG.sync.write(sample)
            );
          }
        }
        expect(ink.length).toBeGreaterThan(3);
        expect(
          ink.every((count) => count > 20),
          JSON.stringify(ink)
        ).toBe(true);
        const zoomOutput = (await app.output()).stdout.slice(
          initialOutput.length
        );
        expect(
          [...zoomOutput.matchAll(/window_allocation=(\d+x\d+)/g)].every(
            (match) => match[1] === allocation
          ),
          zoomOutput
        ).toBe(true);
        const report =
          (await app.output()).stdout
            .trimEnd()
            .split('\n')
            .filter((line) => line.startsWith('backend='))
            .pop() ?? '';
        const metrics = /cell=(\d+)x(\d+)/.exec(report)!;
        const cellWidth = Number(metrics[1]);
        const cellHeight = Number(metrics[2]);
        await window.activate();
        await app.input.moveMouseTo(
          Math.trunc(area.x + cellWidth / 2),
          Math.trunc(area.y + cellHeight * (enlarged.rows - 1.5))
        );
        await app.input.setMouseButton('left', true);
        await app.input.moveMouseTo(
          Math.trunc(area.x + cellWidth * 25.5),
          Math.trunc(area.y + cellHeight * (enlarged.rows - 1.5))
        );
        await app.input.setMouseButton('left', false);
        const copy = await app.getById('driver_copy');
        if (copy.kind !== 'button') throw new Error('Missing copy button');
        await copy.click();
        await toPass(async () => {
          const { stdout } = await execFileAsync(
            fileURLToPath(
              new URL(
                '../../.build/elder-terms-terminal/clipboard-read-helper',
                import.meta.url
              )
            ),
            [],
            { env: await app.environment() }
          );
          expect(stdout).toBe('Select and copy this text.');
        });
        await mode.toggle();
        await waitForResult(async () => expect(await read()).toEqual(enlarged));
        await zoomIn.click();
        await waitForResult(async () => {
          const state = await read();
          expect(state.font).toBeGreaterThan(enlarged.font);
          expect(state.columns).toBe(enlarged.columns);
          expect(state.rows).toBe(enlarged.rows);
          expect(state.bounds.width).toBeGreaterThan(initial.bounds.width);
        });
        // The marked line is near the cursor at the bottom. It must move
        // directly between complete layouts, never through a clipped image
        // drawn with the new cell metrics and the old core row count.
        expect(
          Math.max(...greenBottom),
          JSON.stringify(greenBottom)
        ).toBeLessThanOrEqual(
          Math.max(greenBottom[0], greenBottom[greenBottom.length - 1])
        );
      } finally {
        await launcher.release();
      }
    });
  }

  it('loads the shared core, UI and GTK3 libraries through the normal build', async () => {
    const { stdout } = await execFileAsync('ldd', [driverPath]);
    for (const name of ['boxrig', 'boxrig-ui', 'boxrig-gtk3']) {
      expect(stdout).toContain(`lib${name}.so.0 =>`);
    }
    expect(stdout).not.toContain('not found');
    expect(stdout).not.toContain('libvte');
  });

  for (const backend of ['opengl', 'cairo', 'context-loss'] as const) {
    it(`displays Japanese and preserves its grid through visual zoom with ${backend}`, async () => {
      const launcher = createGtkAppLauncher({
        appPath: driverPath,
        args: backend === 'context-loss' ? ['--context-loss'] : [],
        env: {
          LIBGL_ALWAYS_SOFTWARE: '1',
          GTK_IM_MODULE: 'gtk-im-context-simple',
          ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
        },
      });
      try {
        const app = await launcher.launch();
        const status = await app.getById('driver_status');
        const terminal = await app.getById('driver_terminal');
        const window = await app.getById('driver_window');
        const zoom = await app.getById('driver_zoom_in');
        if (
          status.kind !== 'label' ||
          window.kind !== 'window' ||
          zoom.kind !== 'button'
        ) {
          throw new Error(
            'The driver does not expose its terminal and controls'
          );
        }
        const visibleText = async (): Promise<string> => {
          const output = await app.output();
          const line = output.stdout.trimEnd().split('\n').pop() ?? '';
          return Buffer.from(
            /text=(.*)/.exec(line)?.[1] ?? '',
            'base64'
          ).toString('utf8');
        };
        expect((await terminal.info()).interfaces).toContain('Text');
        await toPass(async () => {
          expect(await status.text()).toContain(
            `backend=${backend === 'cairo' ? 'cairo' : 'opengl'}`
          );
          expect(await status.text()).toContain(
            backend === 'cairo' ? 'reason=3' : 'reason=1'
          );
          expect(await status.text()).toContain('grid=80x24');
          expect(await visibleText()).toContain('日本語の端末');
          const report =
            (await app.output()).stdout.trimEnd().split('\n').pop() ?? '';
          expect(report).toContain('ime_policy=0 ime_route=1');
          expect(Number(/caret=(\d+)/.exec(report)?.[1])).toBeGreaterThan(0);
          expect(Number(/text_events=(\d+)/.exec(report)?.[1])).toBeGreaterThan(
            0
          );
          expect(
            Number(/caret_events=(\d+)/.exec(report)?.[1])
          ).toBeGreaterThan(0);
        });
        if (backend === 'context-loss') {
          const loseContext = await app.getById('driver_lose_gl_context');
          if (loseContext.kind !== 'button')
            throw new Error('Context-loss control is missing');
          await loseContext.click();
          await toPass(async () => {
            expect(await status.text()).toContain('backend=cairo');
            expect(await status.text()).toContain('reason=3');
            expect(await visibleText()).toContain('日本語の端末');
          });
        }
        const terminalCapture = await terminal.capture();
        const metrics = /cell=(\d+)x(\d+)/.exec((await app.output()).stdout);
        if (!metrics)
          throw new Error('No effective cell metrics were published');
        const cellWidth = Number(metrics[1]);
        const cellHeight = Number(metrics[2]);
        const origin = terminalCapture.bounds;
        await window.activate();
        await app.input.moveMouseTo(
          Math.trunc(origin.x + cellWidth / 2),
          Math.trunc(origin.y + cellHeight * 2.5)
        );
        await app.input.setMouseButton('left', true);
        await app.input.moveMouseTo(
          Math.trunc(origin.x + cellWidth * 25.5),
          Math.trunc(origin.y + cellHeight * 2.5)
        );
        await app.input.setMouseButton('left', false);
        await toPass(async () => {
          const report =
            (await app.output()).stdout.trimEnd().split('\n').pop() ?? '';
          expect(report).toContain('selected=1');
          expect(
            Number(/selection_events=(\d+)/.exec(report)?.[1])
          ).toBeGreaterThan(0);
        });
        const copy = await app.getById('driver_copy');
        if (copy.kind !== 'button') throw new Error('Copy control is missing');
        await copy.click();
        await toPass(async () => {
          const { stdout } = await execFileAsync(
            fileURLToPath(
              new URL(
                '../../.build/elder-terms-terminal/clipboard-read-helper',
                import.meta.url
              )
            ),
            [],
            { env: await app.environment() }
          );
          expect(stdout).toBe('Select and copy this text.');
        });
        await toPass(async () => {
          const frame = PNG.sync.read((await terminal.capture()).image);
          let gray = 0;
          let darkInk = 0;
          for (let y = cellHeight * 2; y < cellHeight * 3; y++) {
            for (let x = 0; x < cellWidth * 25; x++) {
              const pixel = (y * frame.width + x) * 4;
              if (
                frame.data[pixel] === 179 &&
                frame.data[pixel + 1] === 179 &&
                frame.data[pixel + 2] === 179
              )
                gray++;
              if (
                frame.data[pixel] < 80 &&
                frame.data[pixel + 1] < 80 &&
                frame.data[pixel + 2] < 80
              )
                darkInk++;
            }
          }
          expect(gray).toBeGreaterThan(cellWidth * cellHeight * 10);
          // Small text can have antialiased edges without a fully black pixel.
          expect(darkInk).toBeGreaterThan(10);
        });
        await app.input.moveMouseTo(
          origin.x + cellWidth * 40,
          origin.y + cellHeight * 12
        );
        await app.input.scrollWheel(0, -80);
        await toPass(async () => {
          expect(await visibleText()).toContain('History line 1\n');
        });
        await app.input.scrollWheel(0, 80);
        await toPass(async () => {
          expect(await visibleText()).toContain('日本語の端末');
        });
        // Wait for native painting too: AtkText can advance before GTK draws.
        await toPass(async () => {
          const pixels = PNG.sync.read((await terminal.capture()).image);
          let green = 0;
          for (
            let offset = 0;
            offset < pixels.width * cellHeight * 3 * 4;
            offset += 4
          ) {
            if (
              pixels.data[offset + 1] > 100 &&
              pixels.data[offset + 1] > pixels.data[offset] * 1.5 &&
              pixels.data[offset + 1] > pixels.data[offset + 2] * 1.3
            )
              green++;
          }
          expect(green).toBeGreaterThan(20);
        });
        await mkdir(
          fileURLToPath(new URL('../../test-results/boxrig', import.meta.url)),
          { recursive: true }
        );
        await writeFile(
          fileURLToPath(
            new URL(`../../test-results/boxrig/${backend}.png`, import.meta.url)
          ),
          (await window.capture()).image
        );
        const before = await status.text();
        const fontBefore = Number(/font=([\d.]+)/.exec(before)?.[1]);
        await zoom.click();
        try {
          await toPass(async () => {
            const current = await status.text();
            expect(current).toContain('grid=80x24');
            expect(Number(/font=([\d.]+)/.exec(current)?.[1])).toBeGreaterThan(
              fontBefore
            );
            expect(await visibleText()).toContain('日本語の端末');
            expect(
              (await app.output()).stdout.trimEnd().split('\n').pop()
            ).toContain('ready=1');
          });
        } finally {
          const output = await app.output();
          await writeFile(
            fileURLToPath(
              new URL(
                `../../test-results/boxrig/driver-${backend}.log`,
                import.meta.url
              )
            ),
            output.stdout + '\n' + output.stderr
          );
        }
        const bounds = await window.bounds();
        await window.resizeTo(bounds.width + 100, bounds.height + 60);
        await toPass(async () => {
          expect(await status.text()).not.toContain('grid=80x24');
          expect(await visibleText()).toContain('日本語の端末');
        });
      } finally {
        await launcher.release();
      }
    });
  }
});
