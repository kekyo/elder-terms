// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  stat,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkTableElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { expectElementKind } from '../../shared/tests/connection-test-helpers.ts';

const binary = (name: string): string =>
  fileURLToPath(new URL(`../../.build/${name}`, import.meta.url));
const supportedGtk = process.env.ELDER_TERMS_NATIVE_ICON_GTK_LIBRARY_PATH;
const legacySystemGtk =
  process.env.ELDER_TERMS_NATIVE_ICON_EXPECT_LEGACY_GTK === '1';

for (const appKind of ['terminal', 'file-transfer'] as const) {
  for (const runtime of ['system-gtk', 'no-protocol', 'icons'] as const) {
    it.skipIf(runtime !== 'system-gtk' && !supportedGtk)(
      `keeps ${appKind} active on Wayland with ${runtime}`,
      async () => {
        const directory = await mkdtemp(join(tmpdir(), 'elder-wayland-icon-'));
        const runtimeDirectory = join(directory, 'runtime');
        await mkdir(runtimeDirectory, { mode: 0o700 });
        const config = join(directory, 'connection.ini');
        const icon = join(directory, 'red.png');
        const pixels = new PNG({ width: 2, height: 2 });
        pixels.data.fill(255);
        for (let index = 0; index < pixels.data.length; index += 4) {
          pixels.data[index + 1] = 0;
          pixels.data[index + 2] = 0;
        }
        await writeFile(icon, PNG.sync.write(pixels));
        const peer = join(directory, 'peer.mjs');
        const terminalLog = join(directory, 'terminal.log');
        const localFile = join(directory, 'listed.txt');
        await writeFile(localFile, 'local fixture\n');
        await writeFile(
          peer,
          "process.stdin.setRawMode(true);process.stdin.resume();process.stdout.write('WAYLAND_ICON_PTY_READY\\r\\n');"
        );
        await writeFile(
          config,
          appKind === 'terminal'
            ? `[general]\nname=Wayland terminal\nicon=file:${icon}\n[local]\ncommand_line=${process.execPath} ${peer}\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=terminal.log\nmode=cooked\n`
            : `[general]\nname=Wayland transfer\ntype=ftp\nicon=file:${icon}\n[ftp]\naddress=fixture.example\nlocal_directory=${directory}\nremote_directory=/remote\n`
        );

        const appPath = binary(
          appKind === 'terminal'
            ? 'elder-terms-terminal/elder-terms-terminal'
            : 'elder-terms-file-transfer/elder-terms-file-transfer'
        );
        const gtkPath = runtime === 'system-gtk' ? undefined : supportedGtk;
        const launcher = createGtkAppLauncher({
          appPath: '/usr/bin/env',
          args: [
            'GDK_BACKEND=wayland',
            'WAYLAND_DISPLAY=icon-test',
            `XDG_RUNTIME_DIR=${runtimeDirectory}`,
            ...(gtkPath
              ? [
                  `LD_LIBRARY_PATH=${gtkPath}:${process.env.LD_LIBRARY_PATH ?? ''}`,
                ]
              : []),
            appPath,
            ...(appKind === 'file-transfer' ? ['--test-fixture'] : []),
            '-c',
            config,
          ],
          accessibilitySession: 'minimal',
          env: {
            LANGUAGE: 'en',
            LC_ALL: 'C.UTF-8',
            XDG_CONFIG_HOME: directory,
            XDG_DATA_HOME: join(directory, 'data'),
            GTK_PATH: '/usr/lib/x86_64-linux-gnu/gtk-3.0',
          },
        });
        const server = spawn(
          binary('shared/native-window-icon-compositor'),
          ['icon-test', runtime === 'no-protocol' ? 'none' : 'icons'],
          {
            env: {
              ...(await launcher.environment()),
              XDG_RUNTIME_DIR: runtimeDirectory,
              WAYLAND_DISPLAY: 'icon-test',
            },
          }
        );
        const closed = once(server, 'close');
        let received = '';
        let errors = '';
        server.stdout.on('data', (data: Buffer) => {
          received += data.toString();
        });
        server.stderr.on('data', (data: Buffer) => {
          errors += data.toString();
        });
        let app: GtkApp | undefined;
        try {
          await waitForResult(async () => {
            expect(
              (await stat(join(runtimeDirectory, 'icon-test'))).isSocket()
            ).toBe(true);
            expect(received).toContain('READY\n');
          });
          const launched = await launcher.launch();
          app = launched;
          if (appKind === 'terminal') {
            expectElementKind(await launched.getById('main_window'), 'window');
            await waitForResult(async () => {
              expect(await readFile(terminalLog, 'utf8')).toContain(
                'WAYLAND_ICON_PTY_READY'
              );
              expect(
                (await (await launched.getById('terminal_view')).info()).states
              ).toContain('showing');
            });
          } else {
            expectElementKind(
              await launched.getById('file_transfer_window'),
              'window'
            );
            const tree = expectElementKind(
              await launched.getById('file_transfer_local_tree'),
              'table'
            ) as GtkTableElement;
            await waitForResult(async () => {
              const rows = await tree.getRowCount();
              const names = await Promise.all(
                Array.from(
                  { length: rows },
                  async (_, row) =>
                    (await (await tree.cellAt(row, 0))?.info())?.name
                )
              );
              expect(names).toContain('listed.txt');
            });
          }
          if (runtime === 'icons') {
            await waitForResult(async () => {
              expect(received).toMatch(/BUFFER \d+ 16 1 4294901760 /u);
              expect(received).toContain('COMMIT ');
            });
          }
          const saved = await readFile(config, 'utf8');
          const id = saved.match(/^connection_id=(.+)$/mu)?.[1];
          expect(id).toMatch(/^[0-9a-f-]{36}$/u);
          await waitForResult(async () => {
            expect(received).toContain(
              `APP_ID net.kekyo.elder-terms.connection.${id}\n`
            );
          });
        } finally {
          const evidence = fileURLToPath(
            new URL(
              `../../test-results/plan6/native-apps/${appKind}-${runtime}/`,
              import.meta.url
            )
          );
          await mkdir(evidence, { recursive: true });
          if (app) {
            const output = await app.output();
            await writeFile(join(evidence, 'stdout.log'), output.stdout);
            await writeFile(join(evidence, 'stderr.log'), output.stderr);
          }
          await launcher.release();
          server.kill('SIGTERM');
          const [code] = await closed;
          await writeFile(join(evidence, 'compositor.log'), received + errors);
          expect({ code, errors }).toEqual({ code: 0, errors: '' });
          if (
            runtime === 'no-protocol' ||
            (runtime === 'system-gtk' && legacySystemGtk)
          ) {
            expect(received).not.toContain('SET ');
          }
          await rm(directory, { recursive: true, force: true });
        }
      }
    );
  }
}
