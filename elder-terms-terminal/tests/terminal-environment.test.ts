// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { chmod, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest } from './gtk-test-helpers.ts';
import {
  expectElementKind,
  withTemporaryDirectory,
} from '../../shared/tests/connection-test-helpers.ts';

const quote = (value: string): string => `'${value.split("'").join("'\\''")}'`;

for (const mode of ['shell', 'command']) {
  for (const [name, colorterm, noColor, forceColor] of [
    ['unset', undefined, undefined, undefined],
    ['empty', '', undefined, undefined],
    ['256color', '256color', undefined, undefined],
    ['truecolor', 'truecolor', undefined, undefined],
    ['NO_COLOR', undefined, '1', undefined],
    ['FORCE_COLOR', undefined, undefined, '0'],
  ]) {
    it(`advertises truecolor to a local ${mode} with ${name} inherited`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const peer = join(directory, 'environment.mjs');
        const shell = join(directory, 'shell');
        const report = join(directory, 'environment.json');
        const config = join(directory, 'connection.ini');
        await writeFile(
          peer,
          `import { writeFileSync } from 'node:fs';
process.stdin.setRawMode(true);
process.stdin.on('data', () => process.exit(0));
writeFileSync(${JSON.stringify(report)}, JSON.stringify({
  pid: process.pid,
  term: process.env.TERM ?? null,
  colorterm: process.env.COLORTERM ?? null,
  noColor: process.env.NO_COLOR ?? null,
  forceColor: process.env.FORCE_COLOR ?? null,
}));`
        );
        await writeFile(
          shell,
          `#!/bin/sh\nexec ${quote(process.execPath)} ${quote(peer)}\n`
        );
        await chmod(shell, 0o755);
        await writeFile(
          config,
          `[general]\nauto_close=false\n[local]\ncommand_line=${mode === 'command' ? `${quote(process.execPath)} ${quote(peer)}` : ''}\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app) => {
            const readEnvironment = async () =>
              JSON.parse(await readFile(report, 'utf8'));
            const actual = await waitForResult(readEnvironment);
            const expected = {
              term: 'xterm-256color',
              colorterm: 'truecolor',
              noColor: noColor ?? null,
              forceColor: forceColor ?? null,
            };
            expect(actual).toMatchObject(expected);
            if (name === 'unset' && mode === 'command') {
              await app.input.pressKey('x');
              const reconnect = expectElementKind(
                await app.getById('reconnect_button'),
                'button'
              );
              await waitForResult(async () =>
                expect((await reconnect.info()).states).toContain('sensitive')
              );
              await reconnect.click();
              const restarted = await waitForResult(async () => {
                const current = await readEnvironment();
                expect(current.pid).not.toBe(actual.pid);
                return current;
              });
              expect(restarted).toMatchObject(expected);
            }
          },
          {
            env: {
              SHELL: shell,
              TERM: 'dumb',
              COLORTERM: colorterm,
              NO_COLOR: noColor,
              FORCE_COLOR: forceColor,
            },
          }
        );
      });
    });
  }
}
