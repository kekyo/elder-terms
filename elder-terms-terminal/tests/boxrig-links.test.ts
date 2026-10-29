// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const appPath =
  process.env.ELDER_TERMS_BOXRIG_DRIVER ??
  fileURLToPath(
    new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
  );

describe('boxrig link actions', () => {
  for (const backend of ['opengl', 'cairo'] as const) {
    it(`uses current Unicode and OSC 8 captures on Ctrl+click with ${backend}`, async () => {
      const fixture =
        'https://first.test/　https://second.test/\r\n' +
        '\x1b]8;;vscode://file/tmp/a%20b.cpp:12:3\x1b\\Open file\x1b]8;;\x1b\\\r\n' +
        '\x1b[?1000h\x1b[?1006h';
      const launcher = createGtkAppLauncher({
        appPath,
        env: {
          LIBGL_ALWAYS_SOFTWARE: '1',
          GTK_IM_MODULE: 'gtk-im-context-simple',
          ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
        },
      });
      try {
        const app = await launcher.launch([
          '--links',
          '--local',
          '--',
          '/bin/sh',
          '-c',
          `stty raw -echo; printf '%s' '${fixture}'; exec cat`,
        ]);
        const window = await app.getById('driver_window');
        if (window.kind !== 'window') throw new Error('Missing driver window');
        await window.activate();
        const terminal = await app.getById('driver_terminal');
        await toPass(async () => {
          const lines = (await app.output()).stdout.trimEnd().split('\n');
          const state =
            lines.reverse().find((line) => line.startsWith('backend=')) ?? '';
          expect(state).toContain('ready=1');
          expect(
            Buffer.from(/text=(.*)/.exec(state)?.[1] ?? '', 'base64').toString()
          ).toContain('Open file');
          const cells = /cell=(\d+)x(\d+)/.exec(state);
          expect(cells).not.toBeNull();
        });
        const state =
          (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend=')) ?? '';
        const cells = /cell=(\d+)x(\d+)/.exec(state)!;
        const { bounds } = await terminal.capture();
        const click = async (row: number, column: number) => {
          await app.input.moveMouseTo(
            Math.trunc(bounds.x + (column + 0.5) * Number(cells[1])),
            Math.trunc(bounds.y + (row + 0.5) * Number(cells[2]))
          );
          await app.input.setModifier('control', true);
          try {
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
          } finally {
            await app.input.setModifier('control', false);
          }
        };
        const actions = async () =>
          (await app.output()).stdout
            .split('\n')
            .filter((line) => line.startsWith('link='))
            .map((line) =>
              Buffer.from(line.slice(5), 'base64').toString().split('\0')
            );
        await click(0, 5);
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=1');
        });
        await toPass(async () =>
          expect(await actions()).toEqual([['xdg-open', 'https://first.test/']])
        );
        await click(0, 25);
        await toPass(async () => expect(await actions()).toHaveLength(2));
        expect((await actions())[1]).toEqual([
          'xdg-open',
          'https://second.test/',
        ]);
        await app.input.moveMouseTo(
          bounds.x + 3,
          bounds.y + Number(cells[2]) * 3
        );
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=0');
        });
        const screen = await app.capture();
        const screenPng = (await import('pngjs')).PNG.sync.read(screen.image);
        const frameSize = screenPng.width * screenPng.height * 4;
        const environment = await app.environment();
        const videoDirectory = fileURLToPath(
          new URL(
            '../../.build/test-results/boxrig-links-video/',
            import.meta.url
          )
        );
        await mkdir(videoDirectory, { recursive: true });
        const videoPath = join(videoDirectory, `${backend}.mkv`);
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
            '30',
            '-draw_mouse',
            '0',
            '-video_size',
            `${screenPng.width}x${screenPng.height}`,
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
            videoPath,
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
        let pending = Buffer.alloc(0);
        let frame = Buffer.alloc(0);
        let frameNumber = 0;
        let recorderError: Error | undefined;
        let recorderStderr = '';
        recorder.on('error', (error) => {
          recorderError = error;
        });
        recorder.stderr.on('data', (bytes: Buffer) => {
          recorderStderr += bytes.toString('utf8');
        });
        recorder.stdout.on('data', (bytes: Buffer) => {
          pending = Buffer.concat([pending, bytes]);
          while (pending.length >= frameSize) {
            frame = Buffer.from(pending.subarray(0, frameSize));
            pending = pending.subarray(frameSize);
            frameNumber += 1;
          }
        });
        const finished = new Promise<number | null>((resolve) =>
          recorder.once('close', resolve)
        );
        const region = (image: Buffer) => {
          const bytes: number[] = [];
          for (
            let y = bounds.y + Number(cells[2]);
            y < bounds.y + Number(cells[2]) * 2;
            y += 1
          )
            for (
              let x = bounds.x + Number(cells[1]) * 3;
              x < bounds.x + Number(cells[1]) * 8;
              x += 1
            ) {
              const offset = (y * screenPng.width + x) * 4;
              bytes.push(image[offset], image[offset + 1], image[offset + 2]);
            }
          return bytes;
        };
        try {
          await toPass(async () => {
            if (recorderError !== undefined) throw recorderError;
            expect(frameNumber).toBeGreaterThan(0);
          });
          const unhovered = region(frame);
          const unhoveredFrame = frameNumber;
          await app.input.moveMouseTo(
            Math.trunc(bounds.x + 3.5 * Number(cells[1])),
            Math.trunc(bounds.y + 1.5 * Number(cells[2]))
          );
          await toPass(async () => {
            const status = (await app.output()).stdout
              .split('\n')
              .reverse()
              .find((line) => line.startsWith('backend='));
            expect(status).toContain('link_hover=1');
            expect(frameNumber).toBeGreaterThan(unhoveredFrame);
            expect(region(frame)).not.toEqual(unhovered);
          });
          const hoveredFrame = frameNumber;
          await app.input.moveMouseTo(
            bounds.x + 3,
            bounds.y + Number(cells[2]) * 3
          );
          await toPass(async () => {
            const status = (await app.output()).stdout
              .split('\n')
              .reverse()
              .find((line) => line.startsWith('backend='));
            expect(status).toContain('link_hover=0');
            expect(frameNumber).toBeGreaterThan(hoveredFrame);
            expect(region(frame)).toEqual(unhovered);
          });
          await writeFile(
            join(videoDirectory, `${backend}-frames.json`),
            JSON.stringify({
              unhoveredFrame,
              hoveredFrame,
              clearedFrame: frameNumber,
            })
          );
        } finally {
          recorder.stdin.write('q\n');
          const exitCode = await finished;
          expect(exitCode, recorderStderr).toBe(0);
        }
        await click(1, 3);
        await toPass(async () => expect(await actions()).toHaveLength(3));
        expect((await actions())[2]).toEqual([
          'code',
          '--reuse-window',
          '--goto',
          '/tmp/a b.cpp:12:3',
        ]);
        const toggle = await app.getById('driver_links_toggle');
        if (toggle.kind !== 'button') throw new Error('Missing link toggle');
        await toggle.click();
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=0');
        });
        await toggle.click();
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=1');
          expect(status).toContain('ready=1');
        });
        await click(0, 5);
        await toPass(async () => expect(await actions()).toHaveLength(4));
        await app.input.moveMouseTo(
          bounds.x + 3,
          bounds.y + Number(cells[2]) * 3
        );
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=0');
        });
        await app.input.pressKey('q');
        await toPass(async () =>
          expect((await app.output()).stdout).toContain('cooked=cQ==')
        );
        const output = (await app.output()).stdout
          .split('\n')
          .filter((line) => line.startsWith('cooked='))
          .map((line) => Buffer.from(line.slice(7), 'base64').toString())
          .join('');
        expect(output).not.toContain('\x1b[<');
      } finally {
        await launcher.release();
      }
    });
  }
});
