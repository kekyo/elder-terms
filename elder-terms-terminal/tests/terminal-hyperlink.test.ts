// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { chmod, mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { PNG } from 'pngjs';
import {
  defaultColumns,
  defaultRows,
  runGtkTest,
  withTemporaryDirectory,
} from './gtk-test-helpers';

const shellQuote = (value: string): string =>
  `'${value.split("'").join("'\\''")}'`;

describe.concurrent('elder-terms-terminal terminal links', () => {
  it('runs the configured argv after Ctrl+clicking an OSC 8 link', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const actionPath = join(directory, 'record-link-action.sh');
      const actionTemporaryPath = join(directory, 'action-arguments.tmp');
      const actionArgumentsPath = join(directory, 'action-arguments.txt');
      const shellPath = join(directory, 'osc-link-shell.sh');
      const shellReadyPath = join(directory, 'shell-ready.txt');
      const shellReleasePath = join(directory, 'shell-release.txt');
      const configPath = join(directory, 'connection.ini');

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

      const connectionSettings = [
        '[hyperlink]',
        'enabled=true',
        '',
        '[hyperlink.test]',
        'regex=^test://open(?<path>/[^:]+):(?<line>[1-9][0-9]*):(?<column>[1-9][0-9]*)$',
        `command=${actionPath}`,
        'arguments=--goto;${path|uri-decode}:${line}:${column};two words;',
        '',
      ].join('\n');
      await writeFile(configPath, connectionSettings, 'utf8');

      try {
        await runGtkTest(
          context,
          ['-c', configPath],
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
          }
        );
      } finally {
        await writeFile(shellReleasePath, 'release\n', 'utf8');
      }
    });
  });

  for (const kind of ['url', 'directory', 'file', 'home-directory']) {
    it(`opens a visible ${kind} with the built-in xdg-open rule`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const targetFile = join(directory, 'target.txt');
        await writeFile(targetFile, 'target\n', 'utf8');
        const homeDirectory = join(directory, 'local home');
        await mkdir(join(homeDirectory, 'Downloads'), { recursive: true });
        const target =
          kind === 'url'
            ? 'https://example.test/docs'
            : kind === 'directory'
              ? directory
              : kind === 'home-directory'
                ? '~/Downloads/'
                : targetFile;
        const expectedTarget =
          kind === 'home-directory' ? `${homeDirectory}/Downloads/` : target;
        const actionPath = join(directory, 'xdg-open');
        const actionTemporaryPath = join(directory, 'url.tmp');
        const actionArgumentsPath = join(directory, 'url.txt');
        const shellPath = join(directory, 'plain-url-shell.sh');
        const shellReadyPath = join(directory, 'shell-ready.txt');
        const shellReleasePath = join(directory, 'shell-release.txt');

        await writeFile(
          actionPath,
          [
            '#!/bin/sh',
            `printf '%s\n' "$1" > ${shellQuote(actionTemporaryPath)}`,
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
            `printf '%s\\r\\n' ${shellQuote(kind === 'home-directory' ? `"${target}"` : target)}`,
            `printf ready > ${shellQuote(shellReadyPath)}`,
            `while [ ! -f ${shellQuote(shellReleasePath)} ]; do sleep 0.02; done`,
            '',
          ].join('\n'),
          'utf8'
        );
        await chmod(shellPath, 0o755);

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
                  message: 'local shell should emit the visible link',
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
                    `${expectedTarget}\n`
                  );
                },
                {
                  message:
                    'Ctrl+click should run xdg-open for the visible link',
                  timeoutMs: 5_000,
                }
              );
            },
            {
              env: {
                PATH: `${directory}:${process.env.PATH ?? ''}`,
                SHELL: shellPath,
                HOME: homeDirectory,
              },
            }
          );
        } finally {
          await writeFile(shellReleasePath, 'release\n', 'utf8');
        }
      });
    });
  }

  for (const backend of ['opengl', 'cairo']) {
    it(`dims invalid paths and shows their modal error with ${backend}`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const missing = join(directory, 'missing');
        const peer = join(directory, 'paths.mjs');
        const config = join(directory, 'connection.ini');
        const output = `\x1b[?25l\x1b]10;#ffffff\x07\x1b]11;#000000\x07${missing}\r\n${directory}\r\nhttps://example.test/tmp\r\n`;
        await writeFile(
          peer,
          `process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write(${JSON.stringify(output)});`
        );
        await writeFile(
          config,
          `[terminal]\nlink_decoration=validated\n[local]\ncommand_line=${process.execPath} ${peer}\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const terminal = await app.getById('terminal_view');
            await waitForResult(async () => {
              const frame = PNG.sync.read((await terminal.capture()).image);
              const cellHeight = frame.height / defaultRows;
              const cellWidth = frame.width / defaultColumns;
              let underlinePixels = 0;
              let validPixels = 0;
              let urlPixels = 0;
              for (let y = 0; y < Math.floor(cellHeight * 3); y++) {
                let dimRow = 0;
                for (
                  let x = 0;
                  x < Math.floor(cellWidth * missing.length);
                  x++
                ) {
                  const offset = (y * frame.width + x) * 4;
                  const [r, g, b] = frame.data.subarray(offset, offset + 3);
                  if (y < cellHeight && r === 127 && g === 127 && b === 127)
                    dimRow++;
                  if (r === 255 && g === 255 && b === 255) {
                    if (y >= cellHeight && y < cellHeight * 2) validPixels++;
                    if (y >= cellHeight * 2) urlPixels++;
                  }
                }
                underlinePixels = Math.max(underlinePixels, dimRow);
              }
              expect(underlinePixels).toBeGreaterThan(
                missing.length * cellWidth * 0.6
              );
              expect(validPixels).toBeGreaterThan(50);
              expect(urlPixels).toBeGreaterThan(50);
            });
            await evidence.captureEvidence('invalid-path-dimmed', async () =>
              app.capture()
            );
            const { bounds } = await terminal.capture();
            await app.input.moveMouseTo(
              Math.trunc(bounds.x + (bounds.width / defaultColumns) * 2.5),
              Math.trunc(bounds.y + (bounds.height / defaultRows) * 0.5)
            );
            await app.input.setModifier('control', true);
            try {
              await app.input.setMouseButton('left', true);
              await app.input.setMouseButton('left', false);
            } finally {
              await app.input.setModifier('control', false);
            }
            const dialog = await app.getById('hyperlink_command_error_dialog');
            expect((await dialog.info()).states).toContain('showing');
            expect((await app.output()).stderr).toContain(
              `expanded path does not exist: ${missing}`
            );
            await evidence.captureEvidence('invalid-path-error', async () =>
              app.capture()
            );
          },
          {
            env:
              backend === 'cairo'
                ? { GDK_GL: 'disable' }
                : { LIBGL_ALWAYS_SOFTWARE: '1' },
          }
        );
      });
    });
  }
});
