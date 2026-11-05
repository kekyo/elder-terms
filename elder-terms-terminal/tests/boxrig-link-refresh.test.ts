// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';

const appPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('links in an updating process table', () => {
  for (const backend of ['cairo', 'opengl']) {
    it.each(['underline', 'validated'])(
      `keeps unchanged command paths stable in every video frame with ${backend}/%s`,
      async (decoration) => {
        const paths = Array.from({ length: 20 }, (_, row) =>
          row % 2 ? '/elder-link-refresh-missing' : '/usr/bin/env'
        );
        const fixture =
          '\x1b[?1049h\x1b[?25l\x1b]10;#ffffff\x07\x1b]11;#000000\x07' +
          paths.map((path) => `00000     ${path}\r\n`).join('');
        const peer = `process.stdin.setRawMode(true); process.stdin.resume();
          process.stdout.write(${JSON.stringify(fixture)});
          process.stdin.once('data', () => {
            let counter = 0;
            const timer = setInterval(() => {
              ++counter;
              for (let row = 0; row < 20; ++row)
                process.stdout.write('\\x1b[' + (row + 1) + ';1H' + String(counter).padStart(5, '0'));
              if (counter === 30) {
                clearInterval(timer);
                process.stdout.write('\\x1b[22;1HDONE');
              }
            }, 30);
          });`;
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
            `--regex-link-decoration=${decoration}`,
            `--osc8-link-decoration=${decoration}`,
            '--local',
            '--',
            process.execPath,
            '-e',
            peer,
          ]);
          const window = await app.getById('driver_window');
          if (window.kind !== 'window')
            throw new Error('Missing driver window');
          await window.activate();
          const terminal = await app.getById('driver_terminal');
          let width = 0,
            height = 0;
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
            ).toContain(paths[1]);
            const cell = /cell=(\d+)x(\d+)/.exec(status)!;
            width = Number(cell[1]);
            height = Number(cell[2]);
            const frame = PNG.sync.read((await terminal.capture()).image);
            for (let row = 0; row < paths.length; ++row) {
              const value = decoration === 'validated' && row % 2 ? 99 : 255;
              const underline = Array.from(
                { length: height },
                (_, y) => y
              ).some((y) => {
                for (
                  let x = 10 * width + 1;
                  x < (10 + paths[row].length) * width - 1;
                  ++x
                ) {
                  if (
                    frame.data[((row * height + y) * frame.width + x) * 4] !==
                    value
                  )
                    return false;
                }
                return true;
              });
              expect(underline, `initial underline on row ${row}`).toBe(true);
            }
          });
          const baselineCapture = await terminal.capture();
          const baseline = PNG.sync.read(baselineCapture.image);
          const captureWidth =
            Math.max(...paths.map((path) => path.length)) * width;
          const captureHeight = paths.length * height;
          const environment = await app.environment();
          const directory = fileURLToPath(
            new URL('../../.build/test-results/link-refresh/', import.meta.url)
          );
          await mkdir(directory, { recursive: true });
          const tag = `${backend}-${decoration}`;
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
              `${captureWidth}x${captureHeight}`,
              '-i',
              `${environment.DISPLAY}+${baselineCapture.bounds.x + 10 * width},${baselineCapture.bounds.y}`,
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
              `${directory}${tag}.mkv`,
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
          const differences: number[] = [];
          let pending = Buffer.alloc(0),
            stderr = '';
          recorder.stderr.on('data', (bytes: Buffer) => {
            stderr += bytes.toString();
          });
          recorder.stdout.on('data', (bytes: Buffer) => {
            pending = Buffer.concat([pending, bytes]);
            const size = captureWidth * captureHeight * 4;
            while (pending.length >= size) {
              const frame = pending.subarray(0, size);
              pending = pending.subarray(size);
              let different = 0;
              for (let row = 0; row < paths.length; ++row) {
                for (let y = row * height; y < (row + 1) * height; ++y) {
                  for (let x = 0; x < paths[row].length * width; ++x) {
                    const source = (y * baseline.width + 10 * width + x) * 4;
                    const target = (y * captureWidth + x) * 4;
                    if (
                      frame[target + 2] !== baseline.data[source] ||
                      frame[target + 1] !== baseline.data[source + 1] ||
                      frame[target] !== baseline.data[source + 2]
                    )
                      different++;
                  }
                }
              }
              differences.push(different);
            }
          });
          const finished = new Promise<number | null>((resolve, reject) => {
            recorder.once('error', reject);
            recorder.once('close', resolve);
          });
          try {
            await toPass(async () => {
              expect(differences.length).toBeGreaterThanOrEqual(2);
            });
            await app.input.pressKey('r');
            await toPass(async () => {
              const status =
                (await app.output()).stdout
                  .split('\n')
                  .reverse()
                  .find((line) => line.startsWith('backend=')) ?? '';
              expect(
                Buffer.from(
                  /text=(.*)/.exec(status)?.[1] ?? '',
                  'base64'
                ).toString()
              ).toContain('DONE');
              expect(status).toContain('ready=1');
              expect(differences.length).toBeGreaterThan(20);
            });
          } finally {
            recorder.stdin.write('q\n');
            expect(await finished, stderr).toBe(0);
            await writeFile(
              `${directory}${tag}-frames.json`,
              JSON.stringify(differences)
            );
          }
          expect(
            differences.every((different) => different === 0),
            'unchanged links retain every text and underline pixel'
          ).toBe(true);
        } finally {
          await launcher.release();
        }
      }
    );
  }
});
