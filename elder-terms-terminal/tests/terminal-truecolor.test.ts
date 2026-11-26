// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  pressKeyWithModifiers,
  readTerminalGridLayout,
  readTerminalGridPixels,
  runGtkTest,
} from './gtk-test-helpers.ts';
import {
  expectElementKind,
  withTemporaryDirectory,
} from '../../shared/tests/connection-test-helpers.ts';

for (const backend of ['cairo', 'opengl']) {
  it(`fills adjacent block cells and preserves font glyphs, colors and copied text (${backend})`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const peer = join(directory, 'blocks.mjs');
      const config = join(directory, 'connection.ini');
      const blocks = '██▀▄▌▐▖▗▘▝▚▞';
      // Occupied quadrants are listed in reading order: top-left to bottom-right.
      const quadrants = [
        '1111',
        '1111',
        '1100',
        '0011',
        '1010',
        '0101',
        '0010',
        '0001',
        '1000',
        '0100',
        '1001',
        '0110',
      ];
      const fontGlyphs = '░▒▓─│█́AZ';
      const palettes = [
        ['00ffff', '000000'],
        ['ff00ff', '000000'],
        ['00ff00', '0000ff'],
      ];
      const outputs = palettes.map(([foreground, background], index) => {
        const rgb = (hex: string) => [...Buffer.from(hex, 'hex')].join(';');
        const attributes =
          index === 2
            ? `38;2;${rgb(background)};48;2;${rgb(foreground)};7`
            : `38;2;${rgb(foreground)};48;2;${rgb(background)}`;
        return `\x1b[H\x1b[${attributes}m${blocks}\x1b[0m\r\n\x1b[38;2;255;255;255m${fontGlyphs}\x1b[0m`;
      });
      await writeFile(
        peer,
        `process.stdin.setRawMode(true);
let phase = 0;
const outputs = ${JSON.stringify(outputs)};
process.stdin.on('data', bytes => {
  if (bytes.includes(103)) process.stdout.write(outputs[++phase % outputs.length]);
});
process.stdout.write('\\x1b[?25l\\x1b[2J' + outputs[0]);`
      );
      await writeFile(
        config,
        `[general]\nbackground=#000000\n[terminal]\nambiguous_width=narrow\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await runGtkTest(
        context,
        ['-c', config],
        async (app, evidence) => {
          const layout = await readTerminalGridLayout(app);
          const status = expectElementKind(
            await app.getById('fixture_terminal_state_label'),
            'label'
          );
          expect(await status.text()).toContain(`backend=${backend}`);
          const { terminal, cellWidth: width, cellHeight: height } = layout;
          for (const [phase, colors] of palettes.entries()) {
            if (phase > 0) await app.input.pressKey('g');
            await waitForResult(async () => {
              const frame = readTerminalGridPixels(
                await terminal.capture(),
                layout
              );
              for (const [column, occupied] of quadrants.entries()) {
                for (let y = 0; y < height; y++) {
                  for (let x = 0; x < width; x++) {
                    // Odd cell sizes have a one-pixel rounding boundary. All
                    // outer edges and the seam between full cells remain exact.
                    if (
                      column > 1 &&
                      (Math.abs(x - width / 2) < 1 ||
                        Math.abs(y - height / 2) < 1)
                    )
                      continue;
                    const filled =
                      occupied[
                        (y >= height / 2 ? 2 : 0) + (x >= width / 2 ? 1 : 0)
                      ] === '1';
                    const offset = (y * frame.width + column * width + x) * 4;
                    expect(
                      frame.data.readUIntBE(offset, 3),
                      `phase=${phase}, cell=${column}, pixel=${x},${y}`
                    ).toBe(Number.parseInt(colors[filled ? 0 : 1], 16));
                  }
                }
              }
              for (let column = 0; column < 8; column++) {
                let ink = 0;
                for (let y = height; y < height * 2; y++)
                  for (let x = column * width; x < (column + 1) * width; x++)
                    if (
                      frame.data.readUIntBE((y * frame.width + x) * 4, 3) !== 0
                    )
                      ink++;
                expect(ink, `font glyph ${column}`).toBeGreaterThan(0);
                if (column !== 5) expect(ink).toBeLessThan(width * height);
              }
            });
            await evidence.captureEvidence(`blocks-${phase}`, async () =>
              app.capture()
            );
          }
          for (const [row, length, expected] of [
            [0, blocks.length, blocks],
            [1, 8, fontGlyphs],
          ] as const) {
            await app.input.pressKey('Escape');
            await app.input.moveMouseTo(
              layout.gridX + 1,
              Math.trunc(layout.gridY + height * (row + 0.5))
            );
            await app.input.setMouseButton('left', true);
            await app.input.moveMouseTo(
              layout.gridX + width * length - 1,
              Math.trunc(layout.gridY + height * (row + 0.5))
            );
            await app.input.setMouseButton('left', false);
            await pressKeyWithModifiers(app, ['control', 'shift'], 'c');
            await waitForResult(async () => {
              const copied = await promisify(execFile)(
                fileURLToPath(
                  new URL(
                    '../../.build/elder-terms-terminal/clipboard-read-helper',
                    import.meta.url
                  )
                ),
                [],
                { env: await app.environment() }
              );
              expect(copied.stdout).toBe(expected);
            });
          }
        },
        {
          env: {
            GDK_SCALE: '1',
            ...(backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' }),
          },
        }
      );
    });
  });

  it(`answers default-color queries and animates a detected truecolor foreground (${backend})`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const peer = join(directory, 'gradient.mjs');
      const config = join(directory, 'connection.ini');
      const report = join(directory, 'replies.txt');
      await writeFile(
        peer,
        `import { writeFileSync } from 'node:fs';
process.stdin.setRawMode(true);
const shades = [92, 133, 174, 215];
let phase = 0;
const paint = () => {
  const row = (text) => [...text].map((letter, index) => {
    const gray = shades[(index + phase) % shades.length];
    return '\\x1b[38;2;' + [gray, gray, gray].join(';') + 'm' + letter;
  }).join('');
  process.stdout.write('\\x1b[H' + row('Working') + '\\r\\n' + row('████████') + '\\x1b[0m');
};
let replies = '';
let ready = false;
let timer;
process.stdin.on('data', (chunk) => {
  if (!ready) {
    replies += chunk.toString();
    if (replies.includes('\\x07') && replies.includes('\\x1b\\\\')) {
      ready = true;
      paint();
      writeFileSync(${JSON.stringify(report)}, replies);
    }
  } else if (!timer && chunk.includes(103) && process.env.COLORTERM === 'truecolor') {
    timer = setInterval(() => { phase++; paint(); }, 150);
  }
});
process.stdout.write('\\x1b[?25l\\x1b[2J\\x1b[H\\x1b]10;?\\x07\\x1b]11;?\\x1b\\\\');`
      );
      await writeFile(
        config,
        `[general]\nbackground=#101010\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await runGtkTest(
        context,
        ['-c', config],
        async (app, evidence) => {
          const replies = await waitForResult(async () =>
            readFile(report, 'utf8')
          );
          expect(replies).toBe(
            '\x1b]10;rgb:d0d0/d0d0/d0d0\x07\x1b]11;rgb:1010/1010/1010\x1b\\'
          );
          await evidence.log('Default color replies', { replies });
          const terminal = await app.getById('terminal_view');
          const before = await terminal.capture();
          // Record the foreground at native scale, including both the text and
          // solid glyphs that make exact RGB samples independent of antialiasing.
          const width = Math.min(before.bounds.width, 200);
          const height = Math.min(before.bounds.height, 64);
          const env = await app.environment();
          const recorder = spawn(
            'ffmpeg',
            [
              '-y',
              '-loglevel',
              'error',
              '-f',
              'x11grab',
              '-framerate',
              '30',
              '-draw_mouse',
              '0',
              '-video_size',
              `${width}x${height}`,
              '-i',
              `${env.DISPLAY}+${before.bounds.x},${before.bounds.y}`,
              '-c:v',
              'ffv1',
              '-threads',
              '1',
              join(evidence.directory, 'truecolor.mkv'),
              '-f',
              'rawvideo',
              '-pix_fmt',
              'rgb24',
              '-threads',
              '1',
              'pipe:1',
            ],
            { env }
          );
          const finished = once(recorder, 'close');
          let pending = Buffer.alloc(0);
          let stderr = '';
          const positions: number[] = [];
          recorder.stderr.on('data', (chunk: Buffer) => {
            stderr += chunk.toString();
          });
          recorder.stdout.on('data', (chunk: Buffer) => {
            pending = Buffer.concat([pending, chunk]);
            const size = width * height * 3;
            while (pending.length >= size) {
              // Locate the leftmost solid part of the brightest band in each
              // recorded frame. Moving text must produce a full repeating cycle.
              let position = -1;
              for (let x = 0; x < width && position < 0; x++) {
                let count = 0;
                for (let y = 0; y < height; y++) {
                  const offset = (y * width + x) * 3;
                  if (
                    pending[offset] === 215 &&
                    pending[offset + 1] === 215 &&
                    pending[offset + 2] === 215
                  )
                    count++;
                }
                if (count >= 8) position = x;
              }
              if (position >= 0) positions.push(position);
              pending = pending.subarray(size);
            }
          });
          try {
            await waitForResult(async () =>
              expect(positions.length).toBeGreaterThan(2)
            );
            await app.input.pressKey('g');
            await waitForResult(async () => {
              const phases = positions.filter(
                (position, index) =>
                  index === 0 || position !== positions[index - 1]
              );
              expect(new Set(phases).size).toBe(4);
              expect(phases.length).toBeGreaterThanOrEqual(5);
              expect(phases[4]).toBe(phases[0]);
            });
          } finally {
            recorder.kill('SIGINT');
            const [status] = await finished;
            await evidence.log('Truecolor video samples', {
              positions,
              stderr,
              status,
            });
            await evidence.captureEvidence('truecolor', async () =>
              app.capture()
            );
          }
          expect(stderr).toBe('');
        },
        {
          env: {
            COLORTERM: undefined,
            NO_COLOR: undefined,
            FORCE_COLOR: undefined,
            GDK_SCALE: '1',
            ...(backend === 'cairo'
              ? { GDK_GL: 'disable' }
              : { LIBGL_ALWAYS_SOFTWARE: '1' }),
          },
        }
      );
    });
  });
}
