// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { expectCapture, waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import {
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';
import { withSshGateway } from '../../shared/tests/ssh-proxy-test-helper.mjs';
import {
  openTransferDirectory,
  selectCurrentTransferDirectory,
} from './file-transfer-destination-helpers.ts';

it.for([2, 3, 4])(
  'transfers NFS v%i through SSH proxy',
  { timeout: 240000 },
  async (version, context) => {
    if (process.env.ELDER_NFS_VM_ACTIVE !== '1') {
      const child = spawn(
        process.execPath,
        [
          fileURLToPath(new URL('./nfs/run-vm.mts', import.meta.url)),
          '--ui-window-only',
          '--ui-ssh-proxy',
          `--ui-version=${version}`,
        ],
        { stdio: ['ignore', 'pipe', 'pipe'] }
      );
      let output = '';
      child.stdout.on('data', (data) => {
        output += data;
      });
      child.stderr.on('data', (data) => {
        output += data;
      });
      const watchdog = setTimeout(() => child.kill('SIGKILL'), 210000);
      try {
        const [status] = await once(child, 'exit');
        expect(status, output).toBe(0);
      } finally {
        clearTimeout(watchdog);
      }
      return;
    }
    const directory = await mkdtemp(join(tmpdir(), 'elder-nfs-proxy-window-'));
    const local = join(directory, 'local');
    await mkdir(local);
    await writeFile(join(local, 'proxy-upload.txt'), 'NFS proxy round-trip\n');
    const evidence = createTestEvidence(context);
    try {
      await withSshGateway(
        fileURLToPath(
          new URL('../../.build/shared/ssh-proxy-test-server', import.meta.url)
        ),
        directory,
        async (gateway) => {
          const config = join(directory, 'connection.ini');
          await writeFile(
            config,
            [
              '[general]',
              'type=nfs',
              'name=NFS proxy files',
              'auto_close=false',
              '[nfs]',
              'address=proxy-test.invalid',
              `version=${version === 4 ? '4.0' : version}`,
              'require_reserved_port=false',
              'filename_encoding=CP932',
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
                '../../.build/elder-terms-file-transfer/elder-terms-file-transfer',
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
              [
                '-c',
                config,
                `--test-ssh-known-hosts-file=${gateway.knownHosts}`,
              ],
              { onOutput: evidence.recordAppOutputEvent }
            );
            const title = expectElementKind(
              await app.getById('file_transfer_prompt_title_label'),
              'label'
            );
            const accept = expectElementKind(
              await app.getById('file_transfer_prompt_accept_button'),
              'button'
            );
            await waitForResult(async () =>
              expect(await title.text()).toContain('SSH proxy')
            );
            await accept.click();
            await waitForResult(async () =>
              expect(
                await expectElementKind(
                  await app.getById(
                    'file_transfer_prompt_monospace_message_label'
                  ),
                  'label'
                ).text()
              ).toContain('[SHA256]')
            );
            await accept.click();
            const remote = expectElementKind(
              await app.getById('file_transfer_remote_tree'),
              'table'
            );
            const localTree = expectElementKind(
              await app.getById('file_transfer_local_tree'),
              'table'
            );
            const remotePath = expectElementKind(
              await app.getById('file_transfer_remote_path_entry'),
              'entry'
            );
            const rowFor = async (
              tree: GtkTableElement,
              name: string
            ): Promise<number> =>
              await waitForResult(async () => {
                expect((await tree.info()).states).toContain('sensitive');
                for (let row = 0; row < (await tree.getRowCount()); row++)
                  if (
                    (await (await tree.cellAt(row, 0))?.info())?.name === name
                  )
                    return row;
                throw new Error(`Missing NFS proxy row: ${name}`);
              });
            const selectMenu = async (
              tree: GtkTableElement,
              name: string,
              id: string
            ): Promise<void> => {
              const row = await rowFor(tree, name);
              for (const selected of await tree.selectedRows())
                await tree.deselectRow(selected);
              await tree.selectRow(row);
              const bounds = (await (await tree.cellAt(row, 0))!.capture())
                .bounds;
              await app.input.moveMouseTo(
                Math.round(bounds.x + bounds.width / 2),
                Math.round(bounds.y + bounds.height / 2)
              );
              await app.input.setMouseButton('right', true);
              await app.input.setMouseButton('right', false);
              const item = expectElementKind(await app.getById(id), 'menuItem');
              await waitForResult(async () =>
                expect((await item.info()).states).toContain('showing')
              );
              await item.click();
            };
            const prefix = version === 4 ? '' : '/srv';
            if (prefix) await openTransferDirectory(app, true, prefix);
            await rowFor(remote, 'public');
            await rowFor(remote, 'readonly');
            await evidence.captureEvidence('nfs-proxy-export-list', async () =>
              app.capture()
            );
            await openTransferDirectory(app, true, prefix + '/public');
            await rowFor(remote, '日本語表');
            await selectCurrentTransferDirectory(app, true);
            await selectMenu(
              localTree,
              'proxy-upload.txt',
              'file_transfer_send_item'
            );
            await waitForResult(async () =>
              expect(
                await expectElementKind(
                  await app.getById('file_transfer_job_1_state'),
                  'label'
                ).text()
              ).toBe('Completed')
            );
            await openTransferDirectory(app, true, prefix + '/public');
            await rowFor(remote, 'proxy-upload.txt');
            await rm(join(local, 'proxy-upload.txt'));
            await selectCurrentTransferDirectory(app, false);
            await selectMenu(
              remote,
              'proxy-upload.txt',
              'file_transfer_receive_item'
            );
            await waitForResult(async () =>
              expect(
                await readFile(join(local, 'proxy-upload.txt'), 'utf8')
              ).toBe('NFS proxy round-trip\n')
            );
            await waitForResult(async () =>
              expect(
                await expectElementKind(
                  await app.getById('file_transfer_job_2_state'),
                  'label'
                ).text()
              ).toBe('Completed')
            );
            await openTransferDirectory(app, false, local);
            await selectMenu(
              remote,
              'proxy-upload.txt',
              'file_transfer_remote_rename_item'
            );
            await expectElementKind(
              await app.getById('file_transfer_prompt_entry'),
              'entry'
            ).setText('proxy-renamed.txt');
            await accept.click();
            await rowFor(remote, 'proxy-renamed.txt');
            await evidence.captureEvidence('nfs-proxy-round-trip', async () =>
              app.capture()
            );
            await selectMenu(
              remote,
              'proxy-renamed.txt',
              'file_transfer_remote_delete_item'
            );
            await waitForResult(async () =>
              expect(await title.text()).toBe('Delete selected item?')
            );
            await accept.click();
            await waitForResult(async () => {
              expect((await remote.info()).states).toContain('sensitive');
              const names = [];
              for (let row = 0; row < (await remote.getRowCount()); row++)
                names.push((await (await remote.cellAt(row, 0))?.info())?.name);
              expect(names).not.toContain('proxy-renamed.txt');
            });
            expect(await remotePath.text()).toBe(prefix + '/public');
            expect(
              gateway.requests.every(
                (request) => request.host === 'proxy-test.invalid'
              )
            ).toBe(true);
            expect(
              [
                ...new Set(gateway.requests.map((request) => request.port)),
              ].sort((a, b) => a - b)
            ).toEqual(version === 4 ? [2049] : [111, 2049, 20048]);
          } finally {
            await evidence.flushOutputs([], launcher);
            await launcher.release();
          }
        }
      );
    } finally {
      await evidence.release();
      await rm(directory, { recursive: true, force: true });
    }
  }
);

it.for([false, true])(
  'rejects an unusable NFS SSH route with reserved ports %s',
  async (reserved, context) => {
    const directory = await mkdtemp(
      join(tmpdir(), 'elder-nfs-proxy-rejected-')
    );
    const evidence = createTestEvidence(context);
    const net = await import('node:net');
    let directConnections = 0;
    let gatewayConnections = 0;
    const direct = net.createServer((socket) => {
      ++directConnections;
      socket.destroy();
    });
    const gateway = net.createServer((socket) => {
      ++gatewayConnections;
      socket.destroy();
    });
    direct.listen(0, '127.0.0.1');
    gateway.listen(0, '127.0.0.1');
    await Promise.all([once(direct, 'listening'), once(gateway, 'listening')]);
    const directAddress = direct.address();
    const gatewayAddress = gateway.address();
    if (
      !directAddress ||
      typeof directAddress === 'string' ||
      !gatewayAddress ||
      typeof gatewayAddress === 'string'
    )
      throw new Error('Missing fixture ports');
    const config = join(directory, 'connection.ini');
    await writeFile(
      config,
      [
        '[general]',
        'type=nfs',
        'auto_close=false',
        '[nfs]',
        'address=127.0.0.1',
        `nfs_port=${directAddress.port}`,
        `mount_port=${directAddress.port}`,
        'known_exports=/srv/public',
        'discover_exports=false',
        `require_reserved_port=${reserved}`,
        '[ssh_proxy]',
        'enabled=true',
        'address=127.0.0.1',
        `port=${gatewayAddress.port}`,
        'username=gateway-user',
        '',
      ].join('\n')
    );
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
        XDG_CONFIG_HOME: join(directory, 'config'),
        XDG_DATA_HOME: join(directory, 'data'),
      },
      onSystemOutput: evidence.recordSystemOutputEvent,
      xvfbTrayHost: true,
    });
    try {
      const app = await launcher.launch(['-c', config], {
        onOutput: evidence.recordAppOutputEvent,
      });
      const title = expectElementKind(
        await app.getById('file_transfer_prompt_title_label'),
        'label'
      );
      await waitForResult(async () =>
        expect(await title.text()).toMatch(/SSH proxy|Failed to start NFS/)
      );
      if ((await title.text()).includes('SSH proxy'))
        await expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        ).click();
      await waitForResult(async () =>
        expect(await title.text()).toBe('Failed to start NFS')
      );
      if (reserved)
        expect(
          await expectElementKind(
            await app.getById('file_transfer_prompt_message_label'),
            'label'
          ).text()
        ).toContain('reserved');
      expect(gatewayConnections).toBe(reserved ? 0 : 1);
      expect(directConnections).toBe(0);
      // Accessible labels change before GTK lays out and paints the next frame.
      // Follow the title's current bounds while waiting for the failure screen.
      const capture = await waitForResult(async () => {
        const bounds = (await title.capture()).bounds;
        const current = await app.capture();
        await expectCapture(current, 'nfs-proxy-error-render').toContainText(
          'Failed to start NFS',
          {
            region: bounds,
            preprocess: { scale: 3 },
            pageSegmentationModes: ['singleLine'],
          }
        );
        return current;
      });
      await evidence.captureEvidence('nfs-proxy-rejected', async () => capture);
    } finally {
      await evidence.flushOutputs([], launcher);
      await launcher.release();
      await evidence.release();
      await Promise.all([
        new Promise<void>((resolve) => direct.close(() => resolve())),
        new Promise<void>((resolve) => gateway.close(() => resolve())),
      ]);
      await rm(directory, { recursive: true, force: true });
    }
  }
);
