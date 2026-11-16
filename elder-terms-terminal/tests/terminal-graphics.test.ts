// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest } from './gtk-test-helpers.ts';
import { withTemporaryDirectory } from '../../shared/tests/connection-test-helpers.ts';

for (const backend of ['cairo', 'opengl']) {
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
