// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFileSync, spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';
import { withSshGateway } from './ssh-proxy-test-helper.mjs';

it('opens an SMB share and downloads a file and folder through the transfer window', async (context) => {
  const fixture = fileURLToPath(new URL('./smb/', import.meta.url));
  const directory = await mkdtemp(join(tmpdir(), 'elder-smb-window-'));
  const container = `elder-smb-window-${process.pid}-${randomBytes(4).toString('hex')}`;
  const externalAddress = process.env.ELDER_SMB_UI_ADDRESS;
  const password = externalAddress
    ? process.env.ELDER_SMB_UI_PASSWORD
    : randomBytes(24).toString('hex');
  if (!password) throw new Error('SMB UI test password is missing');
  const podmanEnv = { ...process.env, ELDER_SMB_PASSWORD: password };
  const podman = (args: string[]): string =>
    execFileSync('podman', args, { encoding: 'utf8', env: podmanEnv });
  let server: ReturnType<typeof spawn> | undefined;
  let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
  const evidence = createTestEvidence(context);
  try {
    const address = externalAddress ?? '127.0.0.1';
    let port = process.env.ELDER_SMB_UI_PORT ?? '445';
    const share = process.env.ELDER_SMB_UI_SHARE ?? 'probe';
    const username = process.env.ELDER_SMB_UI_USER ?? 'elder';
    if (!externalAddress) {
      try {
        podman(['image', 'exists', 'localhost/elder-plan12-samba']);
      } catch {
        const architecture = process.arch === 'x64' ? 'amd64' : process.arch;
        podman([
          'build',
          '--platform',
          `linux/${architecture}`,
          '-t',
          'localhost/elder-plan12-samba',
          '-f',
          resolve(fixture, 'Containerfile'),
          fixture,
        ]);
      }
      server = spawn(
        'podman',
        [
          'run',
          '--rm',
          '--name',
          container,
          '-p',
          '127.0.0.1::445',
          '--env',
          'ELDER_SMB_PASSWORD',
          '-v',
          `${resolve(fixture, 'samba-entry.sh')}:/entry.sh:ro`,
          'localhost/elder-plan12-samba',
          'sh',
          '/entry.sh',
        ],
        { env: podmanEnv, stdio: ['ignore', 'pipe', 'pipe'] }
      );
      await new Promise<void>((ready, reject) => {
        let output = '';
        const timeout = setTimeout(
          () =>
            reject(
              new Error(`Samba startup timed out: ${output.slice(-1000)}`)
            ),
          30_000
        );
        const collect = (bytes: Buffer) => {
          if (output.includes('ready to serve connections')) return;
          output += bytes.toString();
          if (output.includes('ready to serve connections')) {
            clearTimeout(timeout);
            ready();
          }
        };
        server!.stdout!.on('data', collect);
        server!.stderr!.on('data', collect);
        server!.once('error', reject);
        server!.once('exit', (code) => {
          clearTimeout(timeout);
          reject(new Error(`Samba exited ${code}: ${output.slice(-1000)}`));
        });
      });
      port = podman(['port', container, '445/tcp']).trim().split(':').pop()!;
      podman([
        'exec',
        container,
        'sh',
        '-c',
        "printf 'single SMB file\\n' >/share/single.txt; mkdir -p /share/folder; printf 'nested SMB file\\n' >/share/folder/nested.txt",
      ]);
    }
    const local = join(directory, 'local');
    await mkdir(local);
    const config = join(directory, 'connection.ini');
    await writeFile(
      config,
      [
        '[general]',
        'type=smb',
        'name=SMB files',
        '[smb]',
        `address=${address}`,
        `port=${port}`,
        `share=${share}`,
        `username=${username}`,
        `local_directory=${local}`,
        'remote_directory=/',
        '',
      ].join('\n')
    );
    launcher = createGtkAppLauncher({
      appPath:
        process.env.ELDER_TERMS_TEST_FTP_APP ??
        fileURLToPath(
          new URL(
            '../../.build/elder-terms-terminal/elder-terms-file-transfer',
            import.meta.url
          )
        ),
      env: {
        LANGUAGE: 'en',
        LC_ALL: 'C.UTF-8',
        XDG_CONFIG_HOME: join(directory, 'config'),
        XDG_DATA_HOME: join(directory, 'data'),
      },
      onSystemOutput: evidence.recordSystemOutputEvent,
      xvfbTrayHost: true,
    });
    const app = await launcher.launch(['-c', config], {
      onOutput: evidence.recordAppOutputEvent,
    });
    const prompt = expectElementKind(
      await app.getById('file_transfer_prompt_entry'),
      'entry'
    );
    await waitForResult(async () => {
      expect(await prompt.text()).toBe(username);
    });
    await expectElementKind(
      await app.getById('file_transfer_prompt_accept_button'),
      'button'
    ).click();
    await waitForResult(async () => {
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_message_label'),
          'label'
        ).text()
      ).toBe('Password:');
    });
    await prompt.setText(password);
    await expectElementKind(
      await app.getById('file_transfer_prompt_accept_button'),
      'button'
    ).click();
    await waitForResult(
      async () => {
        expect(
          await expectElementKind(
            await app.getById('file_transfer_status_label'),
            'label'
          ).text()
        ).toBe('Ready');
      },
      { timeoutMs: 30_000 }
    );
    const tree = expectElementKind(
      await app.getById('file_transfer_remote_tree'),
      'table'
    ) as GtkTableElement;
    const rowFor = async (name: string): Promise<number> =>
      await waitForResult(async () => {
        for (let row = 0; row < (await tree.getRowCount()); row += 1)
          if ((await (await tree.cellAt(row, 0))?.info())?.name === name)
            return row;
        throw new Error(`Missing SMB row: ${name}`);
      });
    expect(await rowFor('single.txt')).toBeGreaterThanOrEqual(0);
    expect(await rowFor('folder')).toBeGreaterThanOrEqual(0);
    for (const name of ['single.txt', 'folder']) {
      for (const selected of await tree.selectedRows())
        await tree.deselectRow(selected);
      const row = await rowFor(name);
      await tree.selectRow(row);
      const cell = await tree.cellAt(row, 0);
      if (cell === undefined) throw new Error(`Missing SMB cell: ${name}`);
      const bounds = (await cell.capture()).bounds;
      await app.input.moveMouseTo(
        Math.round(bounds.x + bounds.width / 2),
        Math.round(bounds.y + bounds.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);
      const receive = expectElementKind(
        await app.getById('file_transfer_receive_item'),
        'menuItem'
      );
      await waitForResult(async () => {
        expect((await receive.info()).states).toContain('showing');
      });
      await receive.click();
    }
    await waitForResult(
      async () => {
        expect(await readFile(join(local, 'single.txt'), 'utf8')).toBe(
          'single SMB file\n'
        );
        expect(
          await readFile(join(local, 'folder', 'nested.txt'), 'utf8')
        ).toBe('nested SMB file\n');
      },
      { timeoutMs: 30_000 }
    );
    await evidence.captureEvidence('smb-downloaded', async () => app.capture());

    const localTree = expectElementKind(
      await app.getById('file_transfer_local_tree'),
      'table'
    ) as GtkTableElement;
    const rowForTable = async (
      table: GtkTableElement,
      name: string
    ): Promise<number> =>
      await waitForResult(async () => {
        for (let row = 0; row < (await table.getRowCount()); row += 1)
          if ((await (await table.cellAt(row, 0))?.info())?.name === name)
            return row;
        throw new Error(`Missing SMB row: ${name}`);
      });
    const contextMenu = async (
      table: GtkTableElement,
      name: string
    ): Promise<void> => {
      for (const selected of await table.selectedRows())
        await table.deselectRow(selected);
      const row = await rowForTable(table, name);
      await table.selectRow(row);
      const cell = await table.cellAt(row, 0);
      if (cell === undefined) throw new Error(`Missing SMB cell: ${name}`);
      const bounds = (await cell.capture()).bounds;
      await app.input.moveMouseTo(
        Math.round(bounds.x + bounds.width / 2),
        Math.round(bounds.y + bounds.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);
    };
    const acceptName = async (name: string): Promise<void> => {
      const entry = expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      );
      await waitForResult(async () =>
        expect((await entry.info()).states).toContain('showing')
      );
      await entry.setText(name);
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
    };
    await contextMenu(tree, 'single.txt');
    const newFolder = expectElementKind(
      await app.getById('file_transfer_remote_new_directory_item'),
      'menuItem'
    );
    expect((await newFolder.info()).states).toContain('sensitive');
    await newFolder.click();
    await acceptName('ui-folder');
    await rowFor('ui-folder');
    await contextMenu(tree, 'ui-folder');
    await expectElementKind(
      await app.getById('file_transfer_remote_rename_item'),
      'menuItem'
    ).click();
    await acceptName('ui-renamed');
    await rowFor('ui-renamed');
    await contextMenu(tree, 'ui-renamed');
    await expectElementKind(
      await app.getById('file_transfer_remote_delete_item'),
      'menuItem'
    ).click();
    await expectElementKind(
      await app.getById('file_transfer_prompt_accept_button'),
      'button'
    ).click();
    await waitForResult(async () => {
      const names = [];
      for (let row = 0; row < (await tree.getRowCount()); row += 1)
        names.push((await (await tree.cellAt(row, 0))?.info())?.name);
      expect(names).not.toContain('ui-renamed');
    });

    const upload = join(local, 'kept.txt');
    await writeFile(upload, 'first upload\n');
    await expectElementKind(
      await app.getById('file_transfer_local_refresh_button'),
      'button'
    ).click();
    await contextMenu(localTree, 'kept.txt');
    await expectElementKind(
      await app.getById('file_transfer_send_item'),
      'menuItem'
    ).click();
    await rowFor('kept.txt');
    await writeFile(upload, 'replacement upload\n');
    await contextMenu(localTree, 'kept.txt');
    await expectElementKind(
      await app.getById('file_transfer_send_item'),
      'menuItem'
    ).click();
    await waitForResult(async () => {
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_title_label'),
          'label'
        ).text()
      ).toBe('Destination already exists');
    });
    await expectElementKind(
      await app.getById('file_transfer_prompt_alternative_button'),
      'button'
    ).click();
    await waitForResult(async () => {
      expect((await localTree.info()).states).toContain('sensitive');
    });
    await rm(upload);
    await expectElementKind(
      await app.getById('file_transfer_local_refresh_button'),
      'button'
    ).click();
    await contextMenu(tree, 'kept.txt');
    await expectElementKind(
      await app.getById('file_transfer_receive_item'),
      'menuItem'
    ).click();
    await waitForResult(async () => {
      expect(await readFile(upload, 'utf8')).toBe('first upload\n');
    });
    await writeFile(upload, 'replacement upload\n');
    await expectElementKind(
      await app.getById('file_transfer_local_refresh_button'),
      'button'
    ).click();
    await contextMenu(localTree, 'kept.txt');
    await expectElementKind(
      await app.getById('file_transfer_send_item'),
      'menuItem'
    ).click();
    await waitForResult(async () => {
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_title_label'),
          'label'
        ).text()
      ).toBe('Destination already exists');
    });
    await expectElementKind(
      await app.getById('file_transfer_prompt_accept_button'),
      'button'
    ).click();
    await waitForResult(async () => {
      expect((await localTree.info()).states).toContain('sensitive');
    });
    if (!externalAddress)
      expect(podman(['exec', container, 'cat', '/share/kept.txt'])).toBe(
        'replacement upload\n'
      );
    await evidence.captureEvidence('smb-uploaded', async () => app.capture());
  } finally {
    if (launcher !== undefined) {
      await evidence.flushOutputs([], launcher);
      await launcher.release();
    }
    await evidence.release();
    if (server !== undefined) {
      try {
        podman(['rm', '-f', container]);
      } catch {
        /* The fixture is already gone. */
      }
      if (server.exitCode === null && server.signalCode === null)
        await once(server, 'exit');
    }
    await rm(directory, { recursive: true, force: true });
  }
}, 120_000);

const proxyWindowTest = process.env.ELDER_SMB_UI_ADDRESS ? it.skip : it;
proxyWindowTest(
  'downloads and uploads over experimental SMB SSH proxy in the transfer window',
  async (context) => {
    const fixture = fileURLToPath(new URL('./smb/', import.meta.url));
    const directory = await mkdtemp(join(tmpdir(), 'elder-smb-proxy-window-'));
    const container = `elder-smb-proxy-window-${process.pid}-${randomBytes(4).toString('hex')}`;
    const password = randomBytes(24).toString('hex');
    const podmanEnv = { ...process.env, ELDER_SMB_PASSWORD: password };
    const podman = (args: string[]): string =>
      execFileSync('podman', args, { encoding: 'utf8', env: podmanEnv });
    const evidence = createTestEvidence(context);
    let server: ReturnType<typeof spawn> | undefined;
    try {
      try {
        podman(['image', 'exists', 'localhost/elder-plan12-samba']);
      } catch {
        const architecture = process.arch === 'x64' ? 'amd64' : process.arch;
        podman([
          'build',
          '--platform',
          `linux/${architecture}`,
          '-t',
          'localhost/elder-plan12-samba',
          '-f',
          resolve(fixture, 'Containerfile'),
          fixture,
        ]);
      }
      server = spawn(
        'podman',
        [
          'run',
          '--rm',
          '--name',
          container,
          '-p',
          '127.0.0.1::445',
          '--env',
          'ELDER_SMB_PASSWORD',
          '-v',
          `${resolve(fixture, 'samba-entry.sh')}:/entry.sh:ro`,
          'localhost/elder-plan12-samba',
          'sh',
          '/entry.sh',
        ],
        { env: podmanEnv, stdio: ['ignore', 'pipe', 'pipe'] }
      );
      await new Promise<void>((ready, reject) => {
        let output = '';
        const timeout = setTimeout(
          () =>
            reject(
              new Error(`Samba startup timed out: ${output.slice(-1000)}`)
            ),
          30_000
        );
        const collect = (bytes: Buffer) => {
          if (output.includes('ready to serve connections')) return;
          output += bytes.toString();
          if (output.includes('ready to serve connections')) {
            clearTimeout(timeout);
            ready();
          }
        };
        server!.stdout!.on('data', collect);
        server!.stderr!.on('data', collect);
        server!.once('error', reject);
        server!.once('exit', (code) => {
          clearTimeout(timeout);
          reject(new Error(`Samba exited ${code}: ${output.slice(-1000)}`));
        });
      });
      const port = Number(
        podman(['port', container, '445/tcp']).trim().split(':').pop()
      );
      podman([
        'exec',
        container,
        'sh',
        '-c',
        "printf 'proxy download\\n' >/share/proxy-download.txt",
      ]);
      const local = join(directory, 'local');
      await mkdir(local);
      const gatewayExecutable = fileURLToPath(
        new URL(
          '../../.build/elder-terms-terminal/ssh-proxy-test-server',
          import.meta.url
        )
      );
      await withSshGateway(gatewayExecutable, directory, async (gateway) => {
        const config = join(directory, 'connection.ini');
        await writeFile(
          config,
          [
            '[general]',
            'type=smb',
            'name=SMB proxy files',
            '[smb]',
            'address=proxy-test.invalid',
            `port=${port}`,
            'share=probe',
            'username=elder',
            `local_directory=${local}`,
            '[ssh_proxy]',
            'enabled=true',
            'address=127.0.0.1',
            `port=${gateway.port}`,
            'username=gateway-user',
            '',
          ].join('\n')
        );
        const launcher = createGtkAppLauncher({
          appPath: fileURLToPath(
            new URL(
              '../../.build/elder-terms-terminal/elder-terms-file-transfer',
              import.meta.url
            )
          ),
          env: {
            LANGUAGE: 'en',
            LC_ALL: 'C.UTF-8',
            XDG_CONFIG_HOME: join(directory, 'config'),
            XDG_DATA_HOME: join(directory, 'data'),
          },
          onSystemOutput: evidence.recordSystemOutputEvent,
          xvfbTrayHost: true,
        });
        try {
          const app = await launcher.launch(
            ['-c', config, `--test-ssh-known-hosts-file=${gateway.knownHosts}`],
            { onOutput: evidence.recordAppOutputEvent }
          );
          const title = expectElementKind(
            await app.getById('file_transfer_prompt_title_label'),
            'label'
          );
          await waitForResult(async () =>
            expect(await title.text()).toContain('SSH proxy')
          );
          const entry = expectElementKind(
            await app.getById('file_transfer_prompt_entry'),
            'entry'
          );
          await waitForResult(async () =>
            expect(await entry.text()).toBe('gateway-user')
          );
          const accept = expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          );
          await accept.click();
          const randomArt = expectElementKind(
            await app.getById('file_transfer_prompt_monospace_message_label'),
            'label'
          );
          await waitForResult(async () =>
            expect(await randomArt.text()).toContain('[SHA256]')
          );
          await accept.click();
          await waitForResult(async () =>
            expect(await title.text()).toBe('SMB authentication')
          );
          await waitForResult(async () =>
            expect(await entry.text()).toBe('elder')
          );
          await accept.click();
          await waitForResult(async () =>
            expect(
              await expectElementKind(
                await app.getById('file_transfer_prompt_message_label'),
                'label'
              ).text()
            ).toBe('Password:')
          );
          await entry.setText(password);
          await accept.click();
          await waitForResult(
            async () =>
              expect(
                await expectElementKind(
                  await app.getById('file_transfer_status_label'),
                  'label'
                ).text()
              ).toBe('Ready'),
            { timeoutMs: 30_000 }
          );

          const selectMenu = async (
            tableId: string,
            name: string,
            menuId: string
          ): Promise<void> => {
            const table = expectElementKind(
              await app.getById(tableId),
              'table'
            ) as GtkTableElement;
            const row = await waitForResult(async () => {
              for (
                let index = 0;
                index < (await table.getRowCount());
                index += 1
              )
                if (
                  (await (await table.cellAt(index, 0))?.info())?.name === name
                )
                  return index;
              throw new Error(`Missing SMB row: ${name}`);
            });
            for (const selected of await table.selectedRows())
              await table.deselectRow(selected);
            await table.selectRow(row);
            const cell = await table.cellAt(row, 0);
            if (!cell) throw new Error(`Missing SMB cell: ${name}`);
            const bounds = (await cell.capture()).bounds;
            await app.input.moveMouseTo(
              Math.round(bounds.x + bounds.width / 2),
              Math.round(bounds.y + bounds.height / 2)
            );
            await app.input.setMouseButton('right', true);
            await app.input.setMouseButton('right', false);
            await expectElementKind(
              await app.getById(menuId),
              'menuItem'
            ).click();
          };
          await selectMenu(
            'file_transfer_remote_tree',
            'proxy-download.txt',
            'file_transfer_receive_item'
          );
          await waitForResult(
            async () =>
              expect(
                await readFile(join(local, 'proxy-download.txt'), 'utf8')
              ).toBe('proxy download\n'),
            { timeoutMs: 30_000 }
          );
          await writeFile(join(local, 'proxy-upload.txt'), 'proxy upload\n');
          await expectElementKind(
            await app.getById('file_transfer_local_refresh_button'),
            'button'
          ).click();
          await selectMenu(
            'file_transfer_local_tree',
            'proxy-upload.txt',
            'file_transfer_send_item'
          );
          await waitForResult(
            async () =>
              expect(
                podman(['exec', container, 'cat', '/share/proxy-upload.txt'])
              ).toBe('proxy upload\n'),
            { timeoutMs: 30_000 }
          );
          expect(gateway.requests).toContainEqual({
            host: 'proxy-test.invalid',
            port,
          });
        } finally {
          await evidence.flushOutputs([], launcher);
          await launcher.release();
        }
      });
    } finally {
      await evidence.release();
      if (server !== undefined) {
        try {
          podman(['rm', '-f', container]);
        } catch {
          /* The fixture is already gone. */
        }
        if (server.exitCode === null && server.signalCode === null)
          await once(server, 'exit');
      }
      await rm(directory, { recursive: true, force: true });
    }
  },
  120_000
);
