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
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';

const appPath =
  process.env.ELDER_TERMS_BOXRIG_DRIVER ??
  fileURLToPath(
    new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
  );

describe('local link validation appearance', () => {
  for (const backend of ['opengl', 'cairo']) {
    it(`keeps pending and invalid paths dim throughout the recording with ${backend}`, async () => {
      const missing = '/proc/self/elder-terms-missing-link';
      const fixture = `\x1b[2J\x1b[H/tmp\r\n${missing}\r\n`;
      const peer = `process.stdin.setRawMode(true);
        process.stdout.write('\x1b[?25l\x1b]10;#ffffff\x07\x1b]11;#000000\x07\x1b[4;1HREADY');
        process.stdin.once('data', () => process.stdout.write(${JSON.stringify(fixture)}));`;
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
          '--defer-link-validation',
          '--link-decoration=validated',
          '--local',
          '--',
          process.execPath,
          '-e',
          peer,
        ]);
        const window = await app.getById('driver_window');
        if (window.kind !== 'window') throw new Error('Missing driver window');
        await window.activate();
        let cellWidth = 0;
        let cellHeight = 0;
        await toPass(async () => {
          const status =
            (await app.output()).stdout
              .split('\n')
              .reverse()
              .find((line) => line.startsWith('backend=')) ?? '';
          expect(status).toContain('ready=1');
          expect(
            Buffer.from(
              /text=(.*)/.exec(status)?.[1] ?? '',
              'base64'
            ).toString()
          ).toContain('READY');
          const cells = /cell=(\d+)x(\d+)/.exec(status)!;
          cellWidth = Number(cells[1]);
          cellHeight = Number(cells[2]);
        });
        const terminal = await app.getById('driver_terminal');
        const { bounds } = await terminal.capture();
        const screen = PNG.sync.read((await app.capture()).image);
        const frameSize = screen.width * screen.height * 4;
        const environment = await app.environment();
        const directory = fileURLToPath(
          new URL(
            '../../.build/test-results/boxrig-local-links-video/',
            import.meta.url
          )
        );
        await mkdir(directory, { recursive: true });
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
            join(directory, `${backend}.mkv`),
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
        const frames: number[][] = [];
        let pending = Buffer.alloc(0);
        let recorderError: Error | undefined;
        let stderr = '';
        recorder.on('error', (error) => {
          recorderError = error;
        });
        recorder.stderr.on('data', (bytes: Buffer) => {
          stderr += bytes.toString();
        });
        recorder.stdout.on('data', (bytes: Buffer) => {
          pending = Buffer.concat([pending, bytes]);
          while (pending.length >= frameSize) {
            const frame = pending.subarray(0, frameSize);
            pending = pending.subarray(frameSize);
            frames.push(
              [4, missing.length].map((columns, row) => {
                let maximum = 0;
                for (
                  let y = bounds.y + row * cellHeight;
                  y < bounds.y + (row + 1) * cellHeight;
                  y++
                ) {
                  for (
                    let x = bounds.x;
                    x < bounds.x + columns * cellWidth;
                    x++
                  ) {
                    const offset = (y * screen.width + x) * 4;
                    maximum = Math.max(
                      maximum,
                      frame[offset],
                      frame[offset + 1],
                      frame[offset + 2]
                    );
                  }
                }
                return maximum;
              })
            );
          }
        });
        const finished = new Promise<number | null>((resolve) =>
          recorder.once('close', resolve)
        );
        try {
          await toPass(async () => {
            if (recorderError) throw recorderError;
            expect(frames.length).toBeGreaterThan(1);
            expect(frames[frames.length - 1]).toEqual([0, 0]);
          });
          await app.input.pressKey('p');
          await toPass(async () => {
            expect(
              frames.filter((frame) => frame[0] === 127 && frame[1] === 127)
                .length
            ).toBeGreaterThanOrEqual(5);
          });
          const pendingFrames = frames.length;
          expect(
            frames.every((frame) => frame.every((value) => value <= 127))
          ).toBe(true);
          const validate = await app.getById('driver_validate_paths');
          if (validate.kind !== 'button')
            throw new Error('Missing validation control');
          await validate.click();
          await toPass(async () => {
            expect(
              frames.filter((frame) => frame[0] === 255 && frame[1] === 127)
                .length
            ).toBeGreaterThanOrEqual(5);
          });
          expect(frames.every((frame) => frame[1] <= 127)).toBe(true);
          const validFrame = frames.findIndex((frame) => frame[0] === 255);
          expect(validFrame).toBeGreaterThanOrEqual(pendingFrames);
          expect(
            frames.slice(validFrame).every((frame) => frame[0] === 255)
          ).toBe(true);
          await writeFile(
            join(directory, `${backend}-frames.json`),
            JSON.stringify({ pendingFrames, validFrame, frames })
          );
        } finally {
          recorder.stdin.write('q\n');
          expect(await finished, stderr).toBe(0);
        }
      } finally {
        await launcher.release();
      }
    });
  }
});
