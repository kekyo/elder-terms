import { chmod, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  defaultColumns,
  defaultRows,
  runGtkTest,
  withTemporaryDirectory,
} from './gtk-test-helpers';

const shellQuote = (value: string): string =>
  `'${value.split("'").join("'\\''")}'`;

describe.concurrent('elder-terms-vte OSC 8 hyperlinks', () => {
  it('runs the configured argv after Ctrl+clicking an OSC 8 link', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const actionPath = join(directory, 'record-link-action.sh');
      const actionTemporaryPath = join(directory, 'action-arguments.tmp');
      const actionArgumentsPath = join(directory, 'action-arguments.txt');
      const shellPath = join(directory, 'osc-link-shell.sh');
      const shellReadyPath = join(directory, 'shell-ready.txt');
      const shellReleasePath = join(directory, 'shell-release.txt');

      await writeFile(
        actionPath,
        [
          '#!/bin/sh',
          `{ printf '%s\\n' "$#"; printf '%s\\n' "$@"; } > ${shellQuote(
            actionTemporaryPath
          )}`,
          `mv ${shellQuote(actionTemporaryPath)} ${shellQuote(
            actionArgumentsPath
          )}`,
          '',
        ].join('\n'),
        'utf8'
      );
      await chmod(actionPath, 0o755);

      await writeFile(
        shellPath,
        [
          '#!/bin/sh',
          "printf '\\033]8;;test://open/tmp/source%%20file.cpp:42:7\\033\\\\LINK\\033]8;;\\033\\\\\\r\\n'",
          `printf ready > ${shellQuote(shellReadyPath)}`,
          `while [ ! -f ${shellQuote(shellReleasePath)} ]; do sleep 0.02; done`,
          '',
        ].join('\n'),
        'utf8'
      );
      await chmod(shellPath, 0o755);

      const globalSettings = [
        '[hyperlink]',
        'enabled=true',
        '',
        '[hyperlink.test]',
        'regex=^test://open(?<path>/[^:]+):(?<line>[1-9][0-9]*):(?<column>[1-9][0-9]*)$',
        `command=${actionPath}`,
        'arguments=--goto;${path|uri-decode}:${line}:${column};two words;',
        '',
      ].join('\n');

      try {
        await runGtkTest(
          context,
          [],
          async (app) => {
            await waitForResult(
              async () => {
                expect(await readFile(shellReadyPath, 'utf8')).toBe('ready');
              },
              {
                message: 'local shell should emit the OSC 8 hyperlink',
                timeoutMs: 5_000,
              }
            );

            const terminal = await app.getById('terminal_view');
            const { bounds } = await terminal.capture();
            const linkX = Math.trunc(
              bounds.x + (bounds.width / defaultColumns) * 1.5
            );
            const linkY = Math.trunc(
              bounds.y + (bounds.height / defaultRows) * 0.5
            );

            await waitForResult(
              async () => {
                await app.input.moveMouseTo(linkX, linkY);
                await app.input.setModifier('control', true);
                try {
                  await app.input.setMouseButton('left', true);
                  await app.input.setMouseButton('left', false);
                } finally {
                  await app.input.setModifier('control', false);
                }
                expect(await readFile(actionArgumentsPath, 'utf8')).toBe(
                  '3\n--goto\n/tmp/source file.cpp:42:7\ntwo words\n'
                );
              },
              {
                message: 'Ctrl+click should run the configured hyperlink argv',
                timeoutMs: 5_000,
              }
            );
          },
          {
            env: { SHELL: shellPath },
            globalSettings,
          }
        );
      } finally {
        await writeFile(shellReleasePath, 'release\n', 'utf8');
      }
    });
  });
});
