// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import {
  mkdtemp,
  mkdir,
  readFile,
  readdir,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { expectElementKind } from '../../shared/tests/connection-test-helpers.ts';
import { createWebdavTestServer } from './webdav-test-server.ts';

it.each(['SFTP', 'WebDAV cancellation', 'WebDAV disconnection'])(
  'copies and retires desktop file drops (%s)',
  async (protocol) => {
    const dir = await mkdtemp(join(tmpdir(), 'elder-pane-drop-'));
    const runtimeDirectory = join(dir, 'runtime');
    await mkdir(runtimeDirectory, { mode: 0o700 });
    const server =
      protocol !== 'SFTP' ? await createWebdavTestServer('none') : undefined;
    let serverClosed = false;
    server?.files.set('/dav/unknown-held.bin', {
      content: Buffer.alloc(2048, 42),
      reportedSize: undefined,
    });
    const folderName = server ? 'nested' : 'archive';
    const launcher = createGtkAppLauncher({
      appPath: fileURLToPath(
        new URL(
          '../../.build/elder-terms-file-transfer/elder-terms-file-transfer',
          import.meta.url
        )
      ),
      env: {
        LANGUAGE: 'en',
        LC_ALL: 'C.UTF-8',
        XDG_CONFIG_HOME: dir,
        XDG_RUNTIME_DIR: runtimeDirectory,
      },
      xvfbTrayHost: false,
      xvfbScreen: '2000x1000x24',
    });
    try {
      const cursorProbe = join(dir, 'drag-cursor');
      await promisify(execFile)('cc', [
        fileURLToPath(
          new URL('../../shared/tests/x11-pointer.c', import.meta.url)
        ),
        '-lX11',
        '-lXfixes',
        '-o',
        cursorProbe,
      ]);
      const file = 'drag 日本.txt';
      await mkdir(join(dir, 'received'));
      await writeFile(join(dir, file), 'round trip\n');
      const config = join(dir, 'connection.ini');
      await writeFile(
        config,
        server
          ? `[general]\ntype=webdav\n[webdav]\nscheme=http\nauthentication=none\naddress=127.0.0.1\nport=${server.port}\nbase_path=/dav/\nremote_directory=/\nlocal_directory=${dir}\n`
          : `[general]\ntype=sftp\n[ssh]\naddress=fixture.example\n[sftp]\nremote_directory=/remote\nlocal_directory=${dir}\n`
      );
      const app = await launcher.launch(
        server ? ['-c', config] : ['--test-fixture', '-c', config]
      );
      const local = expectElementKind(
        await app.getById('file_transfer_local_tree'),
        'table'
      );
      const remote = expectElementKind(
        await app.getById('file_transfer_remote_tree'),
        'table'
      );
      const row = async (tree: GtkTableElement, name: string) =>
        await waitForResult(async () => {
          expect((await tree.info()).states).toContain('sensitive');
          for (let i = 0; i < (await tree.getRowCount()); i++) {
            const cell = await tree.cellAt(i, 0);
            if (cell && (await cell.info()).name === name)
              return (await cell.capture()).bounds;
          }
          throw Error('Missing file ' + name);
        });
      const drag = async (
        tree: GtkTableElement,
        dest: GtkTableElement,
        target: string
      ) => {
        const from = await row(tree, file);
        const to = await row(dest, target);
        const x = from.x + Math.floor(from.width / 2),
          y = from.y + Math.floor(from.height / 2);
        const tx = to.x + Math.floor(to.width / 2),
          ty = to.y + Math.floor(to.height / 2);
        await app.input.moveMouseTo(x, y);
        await app.input.setMouseButton('left', true);
        try {
          await app.input.moveMouseTo(x + 16, y);
          await app.input.moveMouseTo(tx - 12, ty);
          await app.input.moveMouseTo(tx, ty);
        } finally {
          await app.input.setMouseButton('left', false);
        }
      };
      await drag(local, remote, server ? 'hello.txt' : 'readme.txt');
      await drag(remote, local, 'received');
      await waitForResult(async () =>
        expect(await readFile(join(dir, 'received', file), 'utf8')).toBe(
          'round trip\n'
        )
      );
      expect(await readFile(join(dir, file), 'utf8')).toBe('round trip\n');

      const destination = join(dir, 'External destination');
      await mkdir(destination);
      const env = await app.environment();
      // Nautilus needs its own desktop bus: the automation bus belongs to the application under test.
      const busConfig = join(dir, 'desktop-bus.conf');
      await writeFile(
        busConfig,
        '<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>' +
          '<policy context="default"><allow own="*"/><allow send_destination="*"/>' +
          '<allow receive_sender="*"/></policy></busconfig>'
      );
      const manager = spawn(
        'dbus-run-session',
        [
          '--config-file=' + busConfig,
          '--',
          'nautilus',
          '--new-window',
          destination,
        ],
        {
          env: {
            ...env,
            GSK_RENDERER: 'cairo',
            GDK_BACKEND: 'x11',
            GIO_USE_VFS: 'local',
            GTK_A11Y: 'none',
            GTK_MODULES: '',
            GTK_USE_PORTAL: '0',
          },
          stdio: 'ignore',
        }
      );
      const xdo = async (...args: string[]) =>
        (await promisify(execFile)('xdotool', args, { env })).stdout.trim();
      try {
        const managerId = await waitForResult(
          async () =>
            await xdo(
              'search',
              '--onlyvisible',
              '--class',
              'org.gnome.Nautilus'
            ),
          { timeoutMs: 60000 }
        );
        await xdo('windowsize', managerId, '575', '650');
        await xdo('windowmove', '--sync', managerId, '1390', '80');
        const window = expectElementKind(
          await app.getById('file_transfer_window'),
          'window'
        );
        await window.moveTo(40, 70);
        await window.activate();
        const exports = async () =>
          (await readdir(runtimeDirectory))
            .filter((name) => name.startsWith('elder-terms-drop-'))
            .sort();
        const dropOnDesktop = async (
          name: string,
          cancel: boolean,
          includeFolder: boolean
        ) => {
          const from = await row(remote, name);
          const x = from.x + Math.floor(from.width / 2);
          const y = from.y + Math.floor(from.height / 2);
          if (includeFolder) {
            const folder = await row(remote, folderName);
            await app.input.moveMouseTo(
              folder.x + Math.floor(folder.width / 2),
              folder.y + Math.floor(folder.height / 2)
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await app.input.setModifier('control', true);
            try {
              await app.input.moveMouseTo(x, y);
              await app.input.setMouseButton('left', true);
              await app.input.setMouseButton('left', false);
            } finally {
              await app.input.setModifier('control', false);
            }
            await waitForResult(async () =>
              expect(await remote.selectedRows()).toHaveLength(2)
            );
          }
          await app.input.moveMouseTo(x, y);
          await app.input.setMouseButton('left', true);
          try {
            await app.input.moveMouseTo(x + 20, y);
            await waitForResult(async () =>
              expect(
                (await promisify(execFile)(cursorProbe, [], { env })).stdout
              ).toMatch(/dnd-/)
            );
            let position = 1700;
            await waitForResult(async () => {
              position = position === 1700 ? 1710 : 1700;
              await app.input.moveMouseTo(position, 380);
              expect(
                (await promisify(execFile)(cursorProbe, [], { env })).stdout
              ).toContain('copy');
            });
            if (cancel) await app.input.pressKey('Escape');
          } finally {
            await app.input.setMouseButton('left', false);
          }
        };
        await dropOnDesktop(file, true, false);
        await waitForResult(async () => {
          expect(await readdir(destination)).toEqual([]);
          expect(await exports()).toEqual([]);
        });
        await dropOnDesktop(file, false, true);
        await waitForResult(async () =>
          expect(await readFile(join(destination, file), 'utf8')).toBe(
            'round trip\n'
          )
        );
        await waitForResult(async () =>
          expect(
            await readFile(
              join(destination, folderName, server ? 'child.txt' : 'old.log'),
              'utf8'
            )
          ).toBe(server ? 'child\n' : 'old remote log\n')
        );
        if (server) {
          const completed = await exports();
          await dropOnDesktop('unknown-held.bin', false, false);
          const summary = expectElementKind(
            await app.getById('file_transfer_jobs_summary'),
            'label'
          );
          await waitForResult(async () =>
            expect(await summary.text()).toContain('1024 bytes')
          );
          expect((await exports()).length).toBe(completed.length + 1);
          // File managers can negotiate the URI target before the drop is committed.
          const active = await waitForResult(async () => {
            for (let id = 1; ; ++id) {
              const label = await app.findById(`file_transfer_job_${id}_state`);
              if (!label) throw new Error('No active download');
              const state = expectElementKind(label, 'label');
              if ((await state.text()) === 'Transferring') return { id, state };
            }
          });
          if (protocol === 'WebDAV disconnection') {
            await server.close();
            serverClosed = true;
            await waitForResult(async () =>
              expect(await active.state.text()).toBe('Awaiting decision')
            );
            await expectElementKind(
              await app.getById('file_transfer_prompt_cancel_button'),
              'button'
            ).click();
          } else {
            await expectElementKind(
              await app.getById(`file_transfer_job_${active.id}_cancel`),
              'button'
            ).click();
          }
          const { state } = active;
          await waitForResult(async () => {
            expect(await state.text()).toBe(
              serverClosed ? 'Failed' : 'Cancelled'
            );
            expect(await exports()).toEqual(completed);
            expect(await readdir(destination)).not.toContain(
              'unknown-held.bin'
            );
          });
        }
      } finally {
        manager.kill();
      }
    } finally {
      if (server && !serverClosed) await server.close();
      await launcher.release();
      await rm(dir, { recursive: true, force: true });
    }
  }
);
