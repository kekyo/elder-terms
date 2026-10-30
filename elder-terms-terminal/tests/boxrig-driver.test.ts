// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { mkdir, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const execFileAsync = promisify(execFile);
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('boxrig GTK3 driver', () => {
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
