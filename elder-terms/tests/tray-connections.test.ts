// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import {
  chmod,
  mkdtemp,
  readFile,
  rename,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import type { GtkApp } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const execute = promisify(execFile);
const callBus = async (
  app: GtkApp,
  destination: string,
  path: string,
  method: string,
  args: string[]
): Promise<string> => {
  const { stdout } = await execute(
    'gdbus',
    [
      'call',
      '--session',
      '--dest',
      destination,
      '--object-path',
      path,
      '--method',
      method,
      '--',
      ...args,
    ],
    { env: { ...process.env, ...(await app.environment()) } }
  );
  return stdout;
};
const menuItems = (layout: string): { id: string; label: string }[] =>
  Array.from(
    layout.matchAll(/\((\d+), \{[^}]*?'label': <'([^']*)'>/gu),
    (match) => ({ id: match[1]!, label: match[2]! })
  );

it('shows all saved connections in the tray and follows changes while the launcher is hidden', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-tray-connections-'));
  const capture = join(directory, 'launched.json');
  for (const kind of ['terminal', 'file-transfer']) {
    const child = join(directory, `${kind}.mjs`);
    await writeFile(
      child,
      `#!/usr/bin/env node\nimport fs from 'node:fs';fs.writeFileSync(${JSON.stringify(capture)}, JSON.stringify({kind:${JSON.stringify(kind)},args:process.argv.slice(2)}));\n`
    );
    await chmod(child, 0o700);
  }
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=tray\nopen_application=\n'
        );
        await writeFile(
          join(connections, 'Alpha_One.ini'),
          '[general]\nicon=noto:emoji_u1f44d\nopen_connection=Ctrl+Alt+X\n'
        );
        await writeFile(
          join(connections, 'Beta transfer.ini'),
          '[general]\ntype=sftp\nicon=material:action/home\n[ssh]\naddress=fixture.example\n'
        );
        await writeFile(
          join(connections, 'Conflict.ini'),
          '[general]\nopen_connection=Ctrl+Alt+X\n'
        );
        await writeFile(
          join(connections, 'SMB share.ini'),
          '[general]\ntype=smb\nconnection_id=bdd60b23-0e7e-4e29-aec5-a8e988f9e5db\n[smb]\naddress=files.example.test\nremote_directory=/team\n'
        );
      },
      async ({ app, connections }) => {
        expect(await app.getWindowCount()).toBe(0);
        await app.getTrayItem({ id: 'elder-terms' });
        const registered = await callBus(
          app,
          'org.kde.StatusNotifierWatcher',
          '/StatusNotifierWatcher',
          'org.freedesktop.DBus.Properties.Get',
          ['org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems']
        );
        const bus = registered.match(
          /['"]([^'"]+)\/StatusNotifierItem['"]/u
        )?.[1];
        expect(bus).toBeDefined();
        const menu = async (method: string, args: string[]): Promise<string> =>
          await callBus(
            app,
            bus!,
            '/StatusNotifierMenu',
            `com.canonical.dbusmenu.${method}`,
            args
          );
        const layout = async (): Promise<string> =>
          await menu('GetLayout', [
            '0',
            '-1',
            "['label', 'children-display', 'enabled']",
          ]);
        const first = await waitForResult(async () => {
          const value = await layout();
          expect(menuItems(value).map((item) => item.label)).toEqual([
            'Open elder-terms',
            'Connections',
            'Alpha__One (Ctrl+Alt+X)',
            'Beta transfer',
            'Conflict (Ctrl+Alt+X)',
            'SMB share',
            'Open windows',
            'No open windows',
            'Application settings',
            'About elder-terms',
            'Quit',
          ]);
          return value;
        });
        const connectionsItem = menuItems(first).find(
          (item) => item.label === 'Connections'
        )!;
        expect(
          await menu('GetLayout', [
            connectionsItem.id,
            '1',
            "['label', 'children-display']",
          ])
        ).toContain('Beta transfer');
        await waitForResult(async () => {
          const icons = await menu('GetLayout', [
            connectionsItem.id,
            '1',
            "['icon-data']",
          ]);
          expect(icons.match(/'icon-data'/gu)?.length).toBe(4);
        });
        const open = async (label: string): Promise<void> => {
          const item = menuItems(await layout()).find(
            (entry) => entry.label === label
          );
          expect(item).toBeDefined();
          await menu('Event', [item!.id, 'clicked', '<0>', '0']);
        };
        await open('Beta transfer');
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual({
            kind: 'file-transfer',
            args: ['-c', join(connections, 'Beta transfer.ini')],
          });
        });
        expect(await app.getWindowCount()).toBe(0);
        await rm(capture);
        await open('SMB share');
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual({
            kind: 'file-transfer',
            args: ['-c', join(connections, 'SMB share.ini')],
          });
        });
        await rm(capture);
        await rename(
          join(connections, 'Beta transfer.ini'),
          join(connections, 'Renamed transfer.ini')
        );
        await writeFile(
          join(connections, 'Alpha_One.ini'),
          '[general]\nicon=material:action/home\nopen_connection=Ctrl+Alt+Z\n'
        );
        await rm(join(connections, 'Conflict.ini'));
        await rm(join(connections, 'SMB share.ini'));
        await writeFile(
          join(connections, 'Added.ini'),
          '[general]\nconnection_id=862a2557-01de-47c8-9a64-5b0aceb2543a\n'
        );
        await waitForResult(async () => {
          const value = await layout();
          expect(menuItems(value).map((item) => item.label)).toEqual([
            'Open elder-terms',
            'Connections',
            'Added',
            'Alpha__One (Ctrl+Alt+Z)',
            'Renamed transfer',
            'Open windows',
            'No open windows',
            'Application settings',
            'About elder-terms',
            'Quit',
          ]);
          expect(value).not.toBe(first);
        });
        await open('Added');
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual({
            kind: 'terminal',
            args: ['-c', join(connections, 'Added.ini')],
          });
        });
        await (await app.getTrayItem({ id: 'elder-terms' })).click();
        const list = await app.getById('connection_list');
        if (list.kind === 'tree') await list.selectChildAt(1);
        else await expectElementKind(list, 'table').selectRow(1);
        await expectElementKind(
          await app.getById('settings_general_icon_reset'),
          'button'
        ).click();
        expect(
          await readFile(join(connections, 'Alpha_One.ini'), 'utf8')
        ).toContain('icon=material:action/home');
        await rm(capture);
        await open('Renamed transfer');
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual({
            kind: 'file-transfer',
            args: ['-c', join(connections, 'Renamed transfer.ini')],
          });
        });
        for (const name of ['Added', 'Alpha_One', 'Renamed transfer'])
          await rm(join(connections, `${name}.ini`));
        await waitForResult(async () => {
          const value = await layout();
          expect(value).toContain('No saved connections');
          expect(value).toContain("'enabled': <false>");
        });
      },
      {
        args: [],
        env: {
          ELDER_TERMS_TERMINAL_PATH: join(directory, 'terminal.mjs'),
          ELDER_TERMS_FILE_TRANSFER_PATH: join(directory, 'file-transfer.mjs'),
        },
      }
    );
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}, 60_000);

it('presents only the ready tracked child and removes it after exit', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-tray-present-'));
  const capture = join(directory, 'present.txt');
  const pidPath = join(directory, 'pid');
  let pid: number | undefined;
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=tray\nopen_application=\n'
        );
        await writeFile(
          join(connections, 'Terminal.ini'),
          '[general]\ntype=local\n'
        );
      },
      async ({ app, x11MapRecorder }) => {
        await app.getTrayItem({ id: 'elder-terms' });
        const registered = await callBus(
          app,
          'org.kde.StatusNotifierWatcher',
          '/StatusNotifierWatcher',
          'org.freedesktop.DBus.Properties.Get',
          ['org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems']
        );
        const bus = registered.match(
          /['"]([^'"]+)\/StatusNotifierItem['"]/u
        )?.[1];
        expect(bus).toBeDefined();
        const menu = async (method: string, args: string[]): Promise<string> =>
          await callBus(
            app,
            bus!,
            '/StatusNotifierMenu',
            `com.canonical.dbusmenu.${method}`,
            args
          );
        const layout = async (): Promise<string> =>
          await menu('GetLayout', ['0', '-1', "['label', 'enabled']"]);
        const connection = menuItems(await layout()).find(
          (item) => item.label === 'Terminal'
        );
        expect(connection).toBeDefined();
        await menu('Event', [connection!.id, 'clicked', '<0>', '0']);
        pid = await waitForResult(async () =>
          Number(await readFile(pidPath, 'utf8'))
        );
        const ready = await waitForResult(async () => {
          const value = await layout();
          const item = menuItems(value).find(
            (entry) => entry.label === 'Terminal (Terminal)'
          );
          expect(item).toBeDefined();
          expect(value).toMatch(
            /Terminal \(Terminal\)'[^}]*'enabled': <true>/u
          );
          return item!;
        });
        await execute(
          'gdbus',
          [
            'emit',
            '--session',
            '--dest',
            bus!,
            '--object-path',
            '/net/kekyo/ElderTerms/TrackedWindow',
            '--signal',
            'net.kekyo.ElderTerms.TrackedWindow.Ready',
            '42',
          ],
          { env: { ...process.env, ...(await app.environment()) } }
        );
        expect(
          menuItems(await layout()).some((item) => item.id === ready.id)
        ).toBe(true);
        await x11MapRecorder?.flush();
        const childWindow = x11MapRecorder
          ?.events()
          .find((event) => event.name === 'Tracked window fixture');
        expect(childWindow).toBeDefined();
        const competitor = await x11MapRecorder?.focusCompetitor();
        expect(await x11MapRecorder?.focusedWindow()).toBe(competitor);
        await menu('Event', [ready.id, 'clicked', '<0>', '0']);
        await waitForResult(async () => {
          expect(await readFile(capture, 'utf8')).toBe('present\n');
          expect(await x11MapRecorder?.focusedWindow()).toBe(
            childWindow!.windowId
          );
        });
        await menu('Event', [ready.id, 'clicked', '<0>', '0']);
        await waitForResult(async () => {
          expect(await readFile(capture, 'utf8')).toBe('present\npresent\n');
        });
        expect(
          menuItems(await layout()).filter(
            (item) => item.label === 'Terminal (Terminal)'
          )
        ).toHaveLength(1);
        process.kill(pid!, 'SIGTERM');
        await waitForResult(async () => {
          expect(menuItems(await layout()).map((item) => item.label)).toContain(
            'No open windows'
          );
        });
        await menu('Event', [ready.id, 'clicked', '<0>', '0']);
        expect(await readFile(capture, 'utf8')).toBe('present\npresent\n');
      },
      {
        args: [],
        recordX11Maps: true,
        env: {
          G_DEBUG: 'fatal-criticals',
          ELDER_TERMS_TERMINAL_PATH: fileURLToPath(
            new URL(
              '../../.build/elder-terms/tracked-window-child',
              import.meta.url
            )
          ),
          ELDER_TERMS_TRACKED_WINDOW_CAPTURE: capture,
          ELDER_TERMS_TRACKED_WINDOW_PID: pidPath,
        },
      }
    );
  } finally {
    if (pid !== undefined) {
      try {
        process.kill(pid, 'SIGTERM');
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'ESRCH') throw error;
      }
    }
    await rm(directory, { recursive: true, force: true });
  }
}, 60_000);

it('does not launch a replacement profile through a stale legacy tray item', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-tray-legacy-id-'));
  const capture = join(directory, 'launched.json');
  const terminal = join(directory, 'terminal.mjs');
  await writeFile(
    terminal,
    `#!/usr/bin/env node\nimport fs from 'node:fs';fs.writeFileSync(${JSON.stringify(capture)}, JSON.stringify(process.argv.slice(2)));\n`
  );
  await chmod(terminal, 0o700);
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=tray\nopen_application=\n'
        );
        await writeFile(join(connections, 'Legacy.ini'), '[terminal]\n');
      },
      async ({ app, connections }) => {
        const path = join(connections, 'Legacy.ini');
        await app.getTrayItem({ id: 'elder-terms' });
        const oldId = await waitForResult(async () => {
          const match = (await readFile(path, 'utf8')).match(
            /^connection_id=([0-9a-f-]{36})$/mu
          );
          expect(match).not.toBeNull();
          return match![1]!;
        });
        const registered = await callBus(
          app,
          'org.kde.StatusNotifierWatcher',
          '/StatusNotifierWatcher',
          'org.freedesktop.DBus.Properties.Get',
          ['org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems']
        );
        const bus = registered.match(
          /['"]([^'"]+)\/StatusNotifierItem['"]/u
        )?.[1];
        expect(bus).toBeDefined();
        const menu = async (method: string, args: string[]): Promise<string> =>
          await callBus(
            app,
            bus!,
            '/StatusNotifierMenu',
            `com.canonical.dbusmenu.${method}`,
            args
          );
        const layout = async (): Promise<string> =>
          await menu('GetLayout', [
            '0',
            '-1',
            "['label', 'children-display', 'enabled']",
          ]);
        const oldItem = await waitForResult(async () => {
          const item = menuItems(await layout()).find(
            (entry) => entry.label === 'Legacy'
          );
          expect(item).toBeDefined();
          return item!;
        });
        await menu('Event', [oldItem.id, 'clicked', '<0>', '0']);
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual([
            '-c',
            path,
          ]);
        });
        await rm(capture);
        const replacement = join(directory, 'replacement.ini');
        await writeFile(replacement, '[terminal]\n');
        await rename(replacement, path);
        await menu('Event', [oldItem.id, 'clicked', '<0>', '0']);
        await waitForResult(async () => {
          const current = menuItems(await layout()).find(
            (item) => item.label === 'Legacy'
          );
          expect(current).toBeDefined();
          expect(current!.id).not.toBe(oldItem.id);
          const newId = (await readFile(path, 'utf8')).match(
            /^connection_id=([0-9a-f-]{36})$/mu
          )?.[1];
          expect(newId).toBeDefined();
          expect(newId).not.toBe(oldId);
        });
        await expect(readFile(capture, 'utf8')).rejects.toMatchObject({
          code: 'ENOENT',
        });
      },
      {
        args: [],
        env: { ELDER_TERMS_TERMINAL_PATH: terminal },
      }
    );
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}, 60_000);

it('tracks duplicate terminal and transfer launches until each child exits', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-tray-windows-'));
  const child = join(directory, 'child.mjs');
  const capture = join(directory, 'children.json');
  const pids: number[] = [];
  await writeFile(
    child,
    `#!/usr/bin/env node
import fs from 'node:fs';
import net from 'node:net';
const output = ${JSON.stringify(capture)};
const server = net.createServer();
server.listen(${JSON.stringify(directory)} + '/' + process.pid + '.sock', () => {
  const previous = fs.existsSync(output) ? JSON.parse(fs.readFileSync(output, 'utf8')) : [];
  fs.writeFileSync(output, JSON.stringify([...previous, process.pid]));
});
`
  );
  await chmod(child, 0o700);
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=tray\nopen_application=\n'
        );
        await writeFile(
          join(connections, 'Terminal.ini'),
          '[general]\ntype=local\nconnection_id=38ef378f-315b-4881-8df0-a26f852f8771\n'
        );
        await writeFile(
          join(connections, 'Transfer.ini'),
          '[general]\ntype=sftp\nconnection_id=793d1c9f-0e3d-4b92-b9dd-99c6eeb472ec\n'
        );
      },
      async ({ app }) => {
        await app.getTrayItem({ id: 'elder-terms' });
        const registered = await callBus(
          app,
          'org.kde.StatusNotifierWatcher',
          '/StatusNotifierWatcher',
          'org.freedesktop.DBus.Properties.Get',
          ['org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems']
        );
        const bus = registered.match(
          /['"]([^'"]+)\/StatusNotifierItem['"]/u
        )![1]!;
        const menu = async (method: string, args: string[]): Promise<string> =>
          await callBus(
            app,
            bus,
            '/StatusNotifierMenu',
            `com.canonical.dbusmenu.${method}`,
            args
          );
        const layout = async (id: string): Promise<string> =>
          await menu('GetLayout', [id, '-1', "['label','enabled']"]);
        const windows = menuItems(await layout('0')).find(
          (item) => item.label === 'Open windows'
        );
        expect(windows).toBeDefined();
        for (const [index, name] of [
          'Terminal',
          'Terminal',
          'Transfer',
        ].entries()) {
          const item = menuItems(await layout('0')).find(
            (entry) => entry.label === name
          )!;
          await menu('Event', [item.id, 'clicked', '<0>', '0']);
          await waitForResult(async () => {
            const launched: number[] = JSON.parse(
              await readFile(capture, 'utf8')
            );
            expect(launched).toHaveLength(index + 1);
            pids.splice(0, pids.length, ...launched);
          });
        }
        const entries = await waitForResult(async () => {
          const value = await layout(windows!.id);
          expect(menuItems(value).map((item) => item.label)).toEqual([
            'Open windows',
            'Terminal (Terminal) #1',
            'Terminal (Terminal) #2',
            'Transfer (File transfer)',
          ]);
          // Fixtures never register a window, so presenting them remains disabled.
          expect(value.match(/'enabled': <false>/gu)).toHaveLength(3);
          return menuItems(value);
        });
        process.kill(pids[0]!, 'SIGTERM');
        await waitForResult(async () => {
          expect(
            menuItems(await layout(windows!.id)).map((item) => item.label)
          ).toEqual([
            'Open windows',
            'Terminal (Terminal)',
            'Transfer (File transfer)',
          ]);
        });
        await menu('Event', [entries[1]!.id, 'clicked', '<0>', '0']);
        expect(JSON.parse(await readFile(capture, 'utf8'))).toHaveLength(3);
        process.kill(pids[1]!, 'SIGKILL');
        process.kill(pids[2]!, 'SIGTERM');
        await waitForResult(async () => {
          expect(
            menuItems(await layout(windows!.id)).map((item) => item.label)
          ).toEqual(['Open windows', 'No open windows']);
        });
      },
      {
        args: [],
        env: {
          ELDER_TERMS_TERMINAL_PATH: child,
          ELDER_TERMS_FILE_TRANSFER_PATH: child,
        },
      }
    );
  } finally {
    for (const pid of pids) {
      try {
        process.kill(pid, 'SIGTERM');
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'ESRCH') throw error;
      }
    }
    await rm(directory, { recursive: true, force: true });
  }
}, 60_000);
