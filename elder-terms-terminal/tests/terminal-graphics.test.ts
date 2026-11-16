// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest } from './gtk-test-helpers.ts';
import { withTemporaryDirectory } from '../../shared/tests/connection-test-helpers.ts';

for (const backend of ['cairo', 'opengl']) {
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
