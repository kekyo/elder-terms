// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import type { GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { readTerminalGridLayout, runGtkTest } from './gtk-test-helpers.ts';
import {
  expectElementKind,
  withTemporaryDirectory,
} from '../../shared/tests/connection-test-helpers.ts';

for (const backend of ['cairo', 'opengl']) {
  for (const [columns, rows] of [
    [80, 24],
    [110, 32],
  ]) {
    it(`replaces and deletes large Kitty images in recorded frames and reconnects (${backend}, ${columns}x${rows})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const peer = join(directory, 'large-image.mjs');
        const config = join(directory, 'connection.ini');
        const palettes = [
          ['ff0000', '00ff00', '0000ff', 'ffffff'],
          ['00ffff', 'ff00ff', 'ffff00', 'ff0000'],
        ];
        // Five MiB exceeds the default GL upload staging buffer while staying
        // within the image limit. Each chunk is an independently encoded unit.
        for (const [index, colors] of palettes.entries()) {
          const pixels = Buffer.alloc(1280 * 1024 * 4);
          for (let y = 0; y < 1024; y++)
            for (let x = 0; x < 1280; x++)
              pixels.writeUInt32BE(
                Number.parseInt(
                  colors[(y >= 512 ? 2 : 0) + (x >= 640 ? 1 : 0)] + 'ff',
                  16
                ),
                (y * 1280 + x) * 4
              );
          let output =
            '\x1b[H\x1b_Ga=T,i=51,p=9,f=32,s=1280,v=1024,c=40,r=12,C=1,q=2,m=1;\x1b\\';
          for (let offset = 0; offset < pixels.length; offset += 3072)
            output += `\x1b_Gm=1;${pixels.subarray(offset, offset + 3072).toString('base64')}\x1b\\`;
          output += '\x1b_Gm=0;\x1b\\';
          await writeFile(join(directory, `image-${index + 1}.txt`), output);
        }
        await writeFile(
          peer,
          `import { readFileSync, writeFileSync } from 'node:fs';
process.stdin.setRawMode(true);
const directory = ${JSON.stringify(directory)};
let replies = '';
let phase = 0;
process.stdin.on('data', (bytes) => {
  replies += bytes.toString();
  if (replies.includes('\\x1b[1;1R')) {
    writeFileSync(directory + '/ready.json', JSON.stringify({ pid: process.pid, phase }));
    replies = '';
    return;
  }
  for (const key of bytes.toString()) {
    if (key === 'q') process.exit(0);
    if (!['1', '2', '3'].includes(key)) continue;
    phase++;
    replies = '';
    process.stdout.write((key === '3' ? '\\x1b_Ga=d,d=I,i=51;\\x1b\\\\' :
      readFileSync(directory + '/image-' + key + '.txt', 'utf8')) + '\\x1b[6n');
  }
});
process.stdout.write('\\x1b[?25l\\x1b[2J\\x1b[H\\x1b[6n');`
        );
        await writeFile(
          config,
          `[general]\nbackground=#000000\nauto_close=false\n[terminal]\nwidth=${columns}\nheight=${rows}\n[local]\ncommand_line=${process.execPath} ${peer}\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const ready = async (): Promise<{ pid: number; phase: number }> =>
              JSON.parse(await readFile(join(directory, 'ready.json'), 'utf8'));
            const initial = await waitForResult(ready);
            const layout = await readTerminalGridLayout(app);
            expect([layout.columns, layout.rows]).toEqual([columns, rows]);
            const status = expectElementKind(
              await app.getById('fixture_terminal_state_label'),
              'label'
            );
            expect(await status.text()).toContain(`backend=${backend}`);
            await evidence.log('Large image grid', {
              columns,
              rows,
              cellWidth: layout.cellWidth,
              cellHeight: layout.cellHeight,
              imageBytes: 1280 * 1024 * 4,
            });
            const width = 40 * layout.cellWidth;
            const height = 12 * layout.cellHeight;
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
                `${(await app.environment()).DISPLAY}+${layout.gridX},${layout.gridY}`,
                '-c:v',
                'ffv1',
                '-threads',
                '1',
                join(evidence.directory, 'large-images.mkv'),
                '-f',
                'rawvideo',
                '-pix_fmt',
                'rgb24',
                '-threads',
                '1',
                'pipe:1',
              ],
              { env: await app.environment() }
            );
            const finished = once(recorder, 'close');
            const samples: number[] = [];
            let pending = Buffer.alloc(0);
            let stderr = '';
            recorder.stderr.on('data', (chunk: Buffer) => {
              stderr += chunk.toString();
            });
            recorder.stdout.on('data', (chunk: Buffer) => {
              pending = Buffer.concat([pending, chunk]);
              const size = width * height * 3;
              while (pending.length >= size) {
                let matched = -1;
                for (let phase = 0; phase < 3; phase++) {
                  let valid = true;
                  for (let y = 0; y < height && valid; y++) {
                    if (Math.abs(y - height / 2) <= 1) continue;
                    for (let x = 0; x < width; x++) {
                      if (Math.abs(x - width / 2) <= 1) continue;
                      const expected =
                        phase === 0
                          ? 0
                          : Number.parseInt(
                              palettes[phase - 1][
                                (y >= height / 2 ? 2 : 0) +
                                  (x >= width / 2 ? 1 : 0)
                              ],
                              16
                            );
                      if (
                        pending.readUIntBE((y * width + x) * 3, 3) !== expected
                      ) {
                        valid = false;
                        break;
                      }
                    }
                  }
                  if (valid) {
                    matched = phase;
                    break;
                  }
                }
                samples.push(matched);
                pending = pending.subarray(size);
              }
            });
            try {
              await waitForResult(async () => expect(samples).toContain(0));
              for (let round = 0; round < 3; round++) {
                for (const key of ['1', '2', '3']) {
                  const start = samples.length;
                  await app.input.pressKey(key);
                  await waitForResult(async () =>
                    expect((await ready()).phase).toBe(round * 3 + Number(key))
                  );
                  await waitForResult(async () =>
                    expect(samples.slice(start)).toContain(Number(key) % 3)
                  );
                  expect(await status.text()).toContain(`backend=${backend}`);
                  if (round === 0)
                    await evidence.captureEvidence(
                      `large-image-${key}`,
                      async () => app.capture()
                    );
                }
              }
              // End a session while its image is visible, then verify that a new
              // session can paint and delete images without retaining the old one.
              let start = samples.length;
              await app.input.pressKey('1');
              await waitForResult(async () =>
                expect(samples.slice(start)).toContain(1)
              );
              await app.input.pressKey('q');
              const reconnect = expectElementKind(
                await app.getById('reconnect_button'),
                'button'
              );
              await waitForResult(async () =>
                expect((await reconnect.info()).states).toContain('sensitive')
              );
              await reconnect.click();
              await waitForResult(async () =>
                expect((await ready()).pid).not.toBe(initial.pid)
              );
              start = samples.length;
              await waitForResult(async () =>
                expect(samples.slice(start)).toContain(0)
              );
              for (const key of ['2', '3', '1']) {
                start = samples.length;
                await app.input.pressKey(key);
                await waitForResult(async () =>
                  expect(samples.slice(start)).toContain(Number(key) % 3)
                );
              }
            } finally {
              recorder.kill('SIGINT');
              const [status] = await finished;
              await evidence.log('Large image video samples', {
                samples,
                stderr,
                status,
              });
              await evidence.captureEvidence('large-images-final', async () =>
                app.capture()
              );
            }
            expect(stderr).toBe('');
            const widgets: GtkWidgetElement[] = [layout.mainWindow];
            let closed = false;
            while (widgets.length > 0) {
              const widget = widgets.shift()!;
              if (
                widget.kind === 'button' &&
                (await widget.info()).name === 'Close'
              ) {
                await widget.click();
                closed = true;
                break;
              }
              if ('getChildCount' in widget) {
                for (
                  let index = 0;
                  index < (await widget.getChildCount());
                  index++
                ) {
                  const child = await widget.childAt(index);
                  if (child !== undefined) widgets.push(child);
                }
              }
            }
            expect(closed).toBe(true);
            await waitForResult(async () =>
              expect((await app.output()).exitCode).toBe(0)
            );
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
  }

  for (const storeFirst of [true, false]) {
    it(`${storeFirst ? 'stores Kitty images with a placement ID until explicitly displayed' : 'deletes Kitty images without injecting a reply into the application'} (${backend})`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const peer = join(directory, 'lifecycle.mjs');
        const config = join(directory, 'connection.ini');
        const upload = `\x1b_Ga=${storeFirst ? 't' : 'T'},i=42,p=7,f=32,s=1,v=1${storeFirst ? '' : ',c=12,r=6,C=1'};/wAA/w==\x1b\\`;
        await writeFile(
          peer,
          `import { writeFileSync } from 'node:fs';
process.stdin.setRawMode(true);
let stage = 0;
let received = '';
const query = '\\x1b[6n';
process.stdin.on('data', (bytes) => {
  if (stage % 2 === 0) {
    received += bytes.toString();
    if (received.endsWith('\\x1b[1;1R')) {
      writeFileSync(${JSON.stringify(directory)} + '/stage-' + stage + '.txt', received);
      received = '';
      stage++;
    }
  } else if (stage === 1 && bytes.includes(103)) {
    stage++;
    process.stdout.write('\\x1b_Ga=p,i=42,p=7,c=12,r=6,C=1,q=2;\\x1b\\\\' + query);
  } else if (stage === 3 && bytes.includes(100)) {
    stage++;
    process.stdout.write('\\x1b_Ga=d,d=I,i=42;\\x1b\\\\' + query);
  }
});
process.stdout.write('\\x1b[?25l\\x1b[2J\\x1b[H' + ${JSON.stringify(upload)} + query);`
        );
        await writeFile(
          config,
          `[general]\nbackground=#000000\n[local]\ncommand_line=${process.execPath} ${peer}\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const uploaded = await waitForResult(async () =>
              readFile(join(directory, 'stage-0.txt'), 'utf8')
            );
            expect(uploaded).toBe('\x1b_Gi=42,p=7;OK\x1b\\\x1b[1;1R');
            const terminal = await app.getById('terminal_view');
            const redPixels = async (): Promise<number> => {
              const frame = PNG.sync.read((await terminal.capture()).image);
              let count = 0;
              for (let offset = 0; offset < frame.data.length; offset += 4) {
                if (
                  frame.data[offset] === 255 &&
                  frame.data[offset + 1] === 0 &&
                  frame.data[offset + 2] === 0
                )
                  count++;
              }
              return count;
            };
            if (storeFirst) expect(await redPixels()).toBe(0);
            await app.input.pressKey('g');
            expect(
              await waitForResult(async () =>
                readFile(join(directory, 'stage-2.txt'), 'utf8')
              )
            ).toBe('\x1b[1;1R');
            await waitForResult(async () =>
              expect(await redPixels()).toBeGreaterThan(500)
            );
            await evidence.captureEvidence('kitty-placement', async () =>
              app.capture()
            );
            await app.input.pressKey('d');
            // The cursor report follows deletion in the same input stream,
            // so its arrival bounds the assertion without an idle timeout.
            expect(
              await waitForResult(async () =>
                readFile(join(directory, 'stage-4.txt'), 'utf8')
              )
            ).toBe('\x1b[1;1R');
            await waitForResult(async () => expect(await redPixels()).toBe(0));
            await evidence.captureEvidence('kitty-deleted', async () =>
              app.capture()
            );
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
  }

  it(`displays Kitty images with independently padded chunks and an empty final chunk (${backend})`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const peer = join(directory, 'image.mjs');
      const config = join(directory, 'connection.ini');
      const colors = ['ff0000', '00ff00', '0000ff', 'ffffff'];
      let output = '\x1b[?25l\x1b[2J\x1b[H';
      output += '\x1b_Ga=T,i=1,f=32,s=2,v=2,c=12,r=6,C=1,q=2,m=1;\x1b\\';
      for (const color of colors) {
        const payload = Buffer.from(`${color}ff`, 'hex').toString('base64');
        output += `\x1b_Gm=1;${payload}\x1b\\`;
      }
      output += '\x1b_Gm=0;\x1b\\';
      output += '\x1b[8;1HKitty image: red / green / blue / white';
      await writeFile(
        peer,
        `process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write(${JSON.stringify(output)});`
      );
      await writeFile(
        config,
        `[general]\nbackground=#000000\n[local]\ncommand_line=${process.execPath} ${peer}\n`
      );
      await runGtkTest(
        context,
        ['-c', config],
        async (app, evidence) => {
          const terminal = await app.getById('terminal_view');
          try {
            await waitForResult(async () => {
              const frame = PNG.sync.read((await terminal.capture()).image);
              const counts = new Map<string, number>();
              for (let i = 0; i < frame.data.length; i += 4) {
                const color = frame.data.subarray(i, i + 3).toString('hex');
                counts.set(color, (counts.get(color) ?? 0) + 1);
              }
              for (const color of colors)
                expect(counts.get(color) ?? 0, color).toBeGreaterThan(500);
            });
          } finally {
            await evidence.captureEvidence('kitty-chunks', async () =>
              app.capture()
            );
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
}
