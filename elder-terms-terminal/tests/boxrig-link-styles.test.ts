// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdir, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';

const appPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('link styling retains terminal attributes', () => {
  for (const backend of ['opengl', 'cairo']) {
    for (const decoration of ['none', 'underline', 'validated']) {
      it(`preserves colors, inverse and double underlines with ${backend}/${decoration}`, async () => {
        const targets = [
          'file:///tmp',
          'file:///tmp',
          'file:///tmp',
          '/proc/self/elder-terms-missing-link',
          'https://example.test/',
        ];
        const styles = [
          '38;2;210;120;100;48;2;40;50;60',
          '7;38;2;210;120;100;48;2;40;50;60',
          '38;2;50;90;100;48;2;235;235;235',
          '38;2;210;120;100;48;2;40;50;60',
          '7;38;2;210;120;100;48;2;40;50;60',
        ];
        const fixture =
          '\x1b[?25l' +
          targets
            .map(
              (target, index) =>
                `\x1b[0;4:2;58:2::80:160:200;${styles[index]}m\x1b]8;;${target}\x1b\\WW\x1b]8;;\x1b\\ WW\x1b[0m\r\n`
            )
            .join('');
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
            `--regex-link-decoration=${decoration}`,
            `--osc8-link-decoration=${decoration}`,
            '--local',
            '--',
            process.execPath,
            '-e',
            `process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write(${JSON.stringify(fixture)});`,
          ]);
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
            ).toContain('WW WW');
            const cells = /cell=(\d+)x(\d+)/.exec(status)!;
            width = Number(cells[1]);
            height = Number(cells[2]);
          });
          const compare = (frame: PNG, row: number) => {
            let different = 0;
            for (let y = row * height; y < (row + 1) * height; y++) {
              for (let x = 0; x < 2 * width; x++) {
                const left = (y * frame.width + x) * 4;
                const right = (y * frame.width + x + 3 * width) * 4;
                if (
                  !frame.data
                    .subarray(left, left + 3)
                    .equals(frame.data.subarray(right, right + 3))
                )
                  different++;
              }
            }
            return different;
          };
          const directory = fileURLToPath(
            new URL('../../.build/test-results/link-styles/', import.meta.url)
          );
          await mkdir(directory, { recursive: true });
          await toPass(async () => {
            const capture = await terminal.capture();
            const frame = PNG.sync.read(capture.image);
            for (let row = 0; row < targets.length; row++) {
              if (decoration === 'validated' && row < 4)
                expect(compare(frame, row), `row ${row}`).toBeGreaterThan(0);
              else expect(compare(frame, row)).toBe(0);
            }
            await writeFile(
              `${directory}${backend}-${decoration}-pending.png`,
              capture.image
            );
          });
          const validate = await app.getById('driver_validate_paths');
          if (validate.kind !== 'button')
            throw new Error('Missing validation control');
          await validate.click();
          await toPass(async () => {
            const capture = await terminal.capture();
            const frame = PNG.sync.read(capture.image);
            for (let row = 0; row < targets.length; row++) {
              if (decoration === 'validated' && row === 3)
                expect(compare(frame, row), `row ${row}`).toBeGreaterThan(0);
              else expect(compare(frame, row)).toBe(0);
            }
            await writeFile(
              `${directory}${backend}-${decoration}-validated.png`,
              capture.image
            );
          });
        } finally {
          await launcher.release();
        }
      });
    }
  }
});
