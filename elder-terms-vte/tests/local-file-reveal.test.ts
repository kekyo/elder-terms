import { execFile, spawn, type ChildProcess } from 'node:child_process';
import {
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { promisify } from 'node:util';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';

const execute = promisify(execFile);

for (const mode of [
  'portal',
  'held',
  'cancel',
  'dismissed',
  'close',
  'portal-unavailable',
  'manager-only',
  'symlink',
  'broken-symlink',
  'unreadable',
  'directory',
  'fallback',
  'all-fail',
  'nautilus',
] as const) {
  it(`reveals a local file through the desktop: ${mode}`, async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-local-reveal-'));
    const evidence = createTestEvidence(context);
    const local = join(directory, 'local');
    const nested = join(local, '資料 # folder');
    const name = mode === 'directory' ? 'child folder' : 'inside 日本語 #.txt';
    const target = join(nested, name);
    const data = join(directory, 'data');
    const config = join(directory, 'config');
    const calls = join(directory, 'calls');
    const desktopCalls = join(directory, 'desktop-calls');
    await mkdir(nested, { recursive: true });
    await mkdir(join(data, 'applications'), { recursive: true });
    await mkdir(config);
    await symlink('/usr/share/mime', join(data, 'mime'));
    await writeFile(calls, '');
    await writeFile(desktopCalls, '');
    if (mode === 'symlink' || mode === 'broken-symlink') {
      await symlink(
        mode === 'symlink' ? local : join(local, 'missing'),
        target
      );
    } else if (mode === 'directory') {
      await mkdir(target);
    } else {
      await writeFile(target, 'selected file');
      if (mode === 'unreadable') await chmod(target, 0);
    }
    await writeFile(join(nested, 'other.txt'), 'must not be selected');
    const executable = join(directory, 'desktop.mjs');
    await writeFile(
      executable,
      `#!${process.execPath}\nimport { appendFile } from 'node:fs/promises';\nawait appendFile(${JSON.stringify(desktopCalls)}, JSON.stringify(process.argv.slice(2)) + '\\n');\n`
    );
    await chmod(executable, 0o755);
    await writeFile(
      join(data, 'applications/fixture.desktop'),
      `[Desktop Entry]\nType=Application\nName=Fixture\nExec="${executable}" %f\nMimeType=inode/directory;\nTerminal=false\n` +
        (mode === 'all-fail' ? `Path=${directory}/missing\n` : '')
    );
    await writeFile(
      join(config, 'mimeapps.list'),
      '[Default Applications]\ninode/directory=fixture.desktop;\n'
    );
    const profile = join(directory, 'ftp.ini');
    await writeFile(
      profile,
      `[general]\ntype=ftp\n[ftp]\naddress=fixture.example\nusername=fixture-user\nlocal_directory=${nested}\nremote_directory=/remote\n`
    );
    const transferApplication = fileURLToPath(
      new URL(
        '../../.build/elder-terms-vte/elder-terms-file-transfer',
        import.meta.url
      )
    );
    const launcher = createGtkAppLauncher({
      // env execs either application so both controllers use this driver's display.
      appPath: mode === 'nautilus' ? '/usr/bin/env' : transferApplication,
      accessibilitySession: 'minimal',
      env: {
        LANGUAGE: 'en',
        LC_ALL: 'C.UTF-8',
        XDG_CONFIG_HOME: config,
        XDG_CONFIG_DIRS: config,
        XDG_DATA_HOME: data,
        XDG_DATA_DIRS: data,
        XDG_CACHE_HOME: join(directory, 'cache'),
        XDG_STATE_HOME: join(directory, 'state'),
      },
      onSystemOutput: evidence.recordSystemOutputEvent,
    });
    const apps: GtkApp[] = [];
    let fixture: ChildProcess | undefined;
    let fixtureExit: Promise<number | null> | undefined;
    let manager: GtkApp | undefined;
    try {
      const environment = await launcher.environment();
      if (mode === 'nautilus') {
        manager = await launcher.launch(
          ['/usr/bin/nautilus', '--new-window', local],
          {
            env: {
              XDG_DATA_DIRS: '/usr/local/share:/usr/share',
              GSETTINGS_SCHEMA_DIR: '/usr/share/glib-2.0/schemas',
            },
            onOutput: evidence.recordAppOutputEvent,
          }
        );
        await waitForResult(async () => {
          const owner = await execute(
            'gdbus',
            [
              'call',
              '--session',
              '--dest',
              'org.freedesktop.DBus',
              '--object-path',
              '/org/freedesktop/DBus',
              '--method',
              'org.freedesktop.DBus.NameHasOwner',
              'org.freedesktop.FileManager1',
            ],
            { env: environment }
          );
          expect(owner.stdout).toContain('true');
          expect(await manager!.getWindowCount()).toBeGreaterThan(0);
        });
      } else {
        fixture = spawn(
          fileURLToPath(
            new URL(
              '../../.build/elder-terms-vte/file-manager-fixture',
              import.meta.url
            )
          ),
          [mode, calls],
          { env: environment, stdio: 'ignore' }
        );
        fixtureExit = new Promise((resolve) => fixture!.once('exit', resolve));
        await waitForResult(async () =>
          expect(await readFile(calls, 'utf8')).toBe('Ready\n')
        );
      }
      const app = await launcher.launch(
        [
          ...(mode === 'nautilus' ? [transferApplication] : []),
          '--test-fixture',
          '-c',
          profile,
        ],
        { onOutput: evidence.recordAppOutputEvent }
      );
      apps.push(app);
      await expectElementKind(
        await app.getById('file_transfer_prompt_secondary_entry'),
        'entry'
      ).setText('secret');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      const tree = expectElementKind(
        await app.getById('file_transfer_local_tree'),
        'table'
      );
      const row = await waitForResult(async () => {
        expect((await tree.info()).states).toContain('sensitive');
        for (let index = 0; index < (await tree.getRowCount()); index++) {
          if ((await (await tree.cellAt(index, 0))?.info())?.name === name)
            return index;
        }
        throw new Error(`Missing file ${name}`);
      });
      await tree.selectRow(row);
      const cell = await tree.cellAt(row, 0);
      if (!cell) throw new Error('Missing selected cell');
      const bounds = (await cell.capture()).bounds;
      await app.input.moveMouseTo(
        Math.round(bounds.x + bounds.width / 2),
        Math.round(bounds.y + bounds.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);
      const item = expectElementKind(
        await app.getById('file_transfer_local_open_directory_item'),
        'menuItem'
      );
      await waitForResult(async () =>
        expect((await item.info()).states).toContain('showing')
      );
      await item.click();
      const records = async () =>
        (await readFile(calls, 'utf8')).trim().split('\n').slice(1);
      const open = `OpenDirectory\t${target}`;
      const show = `ShowItems\t${pathToFileURL(target).href}`;
      const expected: string[] = [];
      if (mode === 'nautilus') {
        const managerProcess = await execute(
          'gdbus',
          [
            'call',
            '--session',
            '--dest',
            'org.freedesktop.DBus',
            '--object-path',
            '/org/freedesktop/DBus',
            '--method',
            'org.freedesktop.DBus.GetConnectionUnixProcessID',
            'org.freedesktop.FileManager1',
          ],
          { env: environment }
        );
        const managerProcessId = /uint32 (\d+)/u.exec(
          managerProcess.stdout
        )?.[1];
        if (!managerProcessId)
          throw new Error('Missing file manager process ID');
        // Read selected grid cells directly; toolkit-specific child wrappers
        // and clipboard keyboard shortcuts are not needed to observe selection.
        await waitForResult(async () => {
          const result = await execute(
            fileURLToPath(
              new URL(
                '../../.build/elder-terms-vte/file-manager-selection-helper',
                import.meta.url
              )
            ),
            [managerProcessId],
            { env: environment }
          );
          const selected = result.stdout.split('\n');
          await evidence.log('Nautilus accessible selection', {
            selected,
            tree: result.stderr,
          });
          expect(selected).toContain(name);
          expect(selected).not.toContain('other.txt');
        });
        await evidence.captureEvidence('nautilus-file-selected', async () =>
          manager!.capture()
        );
      } else if (mode === 'directory') {
        await waitForResult(async () =>
          expect(await readFile(desktopCalls, 'utf8')).toBe(
            `${JSON.stringify([target])}\n`
          )
        );
      } else if (
        mode === 'symlink' ||
        mode === 'broken-symlink' ||
        mode === 'unreadable' ||
        mode === 'manager-only'
      ) {
        expected.push(show);
        await waitForResult(async () =>
          expect(await records()).toEqual(expected)
        );
      } else {
        expected.push(open);
        if (mode === 'portal') {
          expected.push('Response\t0');
        } else if (
          mode === 'portal-unavailable' ||
          mode === 'fallback' ||
          mode === 'all-fail'
        ) {
          expected.push(show);
        } else {
          await waitForResult(async () =>
            expect(await records()).toEqual(expected)
          );
          expect((await tree.info()).states).not.toContain('sensitive');
          expect(await readFile(desktopCalls, 'utf8')).toBe('');
          if (mode === 'close') {
            const pending: GtkWidgetElement[] = [
              await app.getById('file_transfer_header_bar'),
            ];
            let closed = false;
            while (pending.length) {
              const widget = pending.shift()!;
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
                  if (child) pending.push(child);
                }
              }
            }
            expect(closed).toBe(true);
            expected.push('Close');
            await waitForResult(async () =>
              expect(await records()).toEqual(expected)
            );
            await waitForResult(async () =>
              expect((await app.output()).exitCode).toBe(0)
            );
          } else {
            const response = mode === 'held' ? 0 : mode === 'cancel' ? 1 : 2;
            await execute(
              'gdbus',
              [
                'call',
                '--session',
                '--dest',
                'org.example.ElderTerms.FileManagerFixture',
                '--object-path',
                '/fixture',
                '--method',
                'org.example.ElderTerms.FileManagerFixture.Respond',
                String(response),
              ],
              { env: environment }
            );
            expected.push(`Response\t${response}`);
          }
        }
        await waitForResult(async () =>
          expect(await records()).toEqual(expected)
        );
        if (mode === 'fallback') {
          await waitForResult(async () =>
            expect(await readFile(desktopCalls, 'utf8')).toBe(
              `${JSON.stringify([nested])}\n`
            )
          );
        }
      }
      if (mode === 'all-fail' || mode === 'dismissed') {
        const notice = await app.getById(
          'file_transfer_operation_error_dialog'
        );
        expect((await notice.info()).states).toContain('showing');
      } else if (mode !== 'close') {
        await waitForResult(async () =>
          expect((await tree.info()).states).toContain('sensitive')
        );
      }
      if (mode !== 'nautilus') {
        await execute(
          'gdbus',
          [
            'call',
            '--session',
            '--dest',
            'org.example.ElderTerms.FileManagerFixture',
            '--object-path',
            '/fixture',
            '--method',
            'org.example.ElderTerms.FileManagerFixture.Barrier',
          ],
          { env: environment }
        );
        expect(await records()).toEqual(expected);
      }
      if (mode !== 'fallback' && mode !== 'directory')
        expect(await readFile(desktopCalls, 'utf8')).toBe('');
    } catch (error) {
      if (apps.length) {
        try {
          await evidence.captureEvidence('reveal-failure', async () =>
            apps[0].capture()
          );
        } catch (captureError) {
          await evidence.log('Reveal failure capture unavailable', {
            error: captureError,
          });
        }
      }
      throw error;
    } finally {
      try {
        await evidence.flushOutputs(apps, launcher);
      } finally {
        if (fixture && fixture.exitCode === null) fixture.kill();
        if (fixtureExit) await fixtureExit;
        if (manager) await manager.release();
        await launcher.release();
        await evidence.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  }, 90_000);
}
