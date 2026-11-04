// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { writeFile } from 'node:fs/promises';
import { createServer } from 'node:net';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkApp } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import {
  withTemporaryDirectory,
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

import { createWebdavTestServer } from './webdav-test-server.ts';

describe.concurrent('file transfer connection error recovery', () => {
  for (const autoClose of [false, true]) {
    for (const protocol of [
      'sftp',
      'ftp',
      'ftps',
      'webdav',
      'http',
      'https',
      'smb',
      'nfs',
    ]) {
      it(`retries initial ${protocol} connection errors with auto_close=${autoClose}`, async (context) => {
        await withTemporaryDirectory(async (directory) => {
          const server = createServer();
          await new Promise<void>((resolve, reject) => {
            server.once('error', reject);
            server.listen(0, '127.0.0.1', resolve);
          });
          const address = server.address();
          if (!address || typeof address === 'string')
            throw new Error('Missing test server address');
          await new Promise<void>((resolve) => server.close(() => resolve()));
          const type =
            protocol === 'ftps'
              ? 'ftp'
              : protocol === 'https'
                ? 'http'
                : protocol;
          const config = join(directory, 'connection.ini');
          await writeFile(
            config,
            [
              '[general]',
              `type=${type}`,
              `auto_close=${autoClose}`,
              `[${type}]`,
              'address=127.0.0.1',
              `port=${address.port}`,
              `local_directory=${directory}`,
              ...(type === 'sftp'
                ? ['[ssh]', 'address=127.0.0.1', `port=${address.port}`]
                : []),
              ...(['sftp', 'ftp', 'smb'].includes(type)
                ? ['username=test']
                : []),
              ...(type === 'ftp'
                ? [`tls_mode=${protocol === 'ftps' ? 'implicit' : 'none'}`]
                : []),
              ...(type === 'webdav'
                ? ['scheme=http', 'authentication=none', 'base_path=/dav/']
                : []),
              ...(type === 'http'
                ? [`base_url=${protocol}://127.0.0.1:${address.port}/`]
                : []),
              ...(type === 'nfs'
                ? [
                    'version=4.0',
                    `nfs_port=${address.port}`,
                    'connect_timeout_seconds=5',
                    'request_timeout_seconds=5',
                  ]
                : []),
              '',
            ].join('\n')
          );
          const evidence = createTestEvidence(context);
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
          const apps: GtkApp[] = [];
          try {
            const app = await launcher.launch(['-c', config], {
              onOutput: evidence.recordAppOutputEvent,
            });
            apps.push(app);
            const accept = expectElementKind(
              await app.getById('file_transfer_prompt_accept_button'),
              'button'
            );
            const title = expectElementKind(
              await app.getById('file_transfer_prompt_title_label'),
              'label'
            );
            const reconnect = expectElementKind(
              await app.getById('file_transfer_reconnect_button'),
              'button'
            );
            for (let attempt = 1; attempt <= 2; ++attempt) {
              if (['sftp', 'ftp', 'smb'].includes(type)) {
                await waitForResult(async () => {
                  expect(await title.text()).toBe(
                    protocol === 'sftp'
                      ? 'SSH Authentication'
                      : `${protocol.toUpperCase()} authentication`
                  );
                  expect((await accept.info()).states).toContain('showing');
                });
                await accept.click();
                if (type !== 'sftp') {
                  await waitForResult(async () =>
                    expect(
                      await expectElementKind(
                        await app.getById('file_transfer_prompt_message_label'),
                        'label'
                      ).text()
                    ).toBe('Password:')
                  );
                  await accept.click();
                }
              }
              await waitForResult(async () =>
                expect((await reconnect.info()).states).toContain('showing')
              );
              await waitForResult(async () =>
                expect(
                  await expectElementKind(
                    await app.getById(
                      'file_transfer_connection_notice_title_label'
                    ),
                    'label'
                  ).text()
                ).toBe(
                  `Failed to start ${protocol === 'webdav' ? 'WebDAV' : protocol === 'https' ? 'HTTP' : protocol.toUpperCase()}`
                )
              );
              await waitForResult(async () => {
                expect((await app.output()).exitCode).toBeNull();
                expect((await reconnect.info()).states).toContain('showing');
                expect((await reconnect.info()).states).toContain('sensitive');
                expect(
                  (await (await app.getById('file_transfer_local_tree')).info())
                    .states
                ).toContain('sensitive');
              });
              const localTree = expectElementKind(
                await app.getById('file_transfer_local_tree'),
                'table'
              );
              await writeFile(
                join(directory, `offline-${attempt}.txt`),
                'local browsing'
              );
              await expectElementKind(
                await app.getById('file_transfer_local_refresh_button'),
                'button'
              ).click();
              await waitForResult(async () => {
                const names: string[] = [];
                for (let row = 0; row < (await localTree.getRowCount()); ++row)
                  names.push(
                    (await (await localTree.cellAt(row, 0))?.info())?.name ?? ''
                  );
                expect(names).toContain(`offline-${attempt}.txt`);
              });
              if (attempt === 1) await reconnect.click();
            }
            await evidence.captureEvidence(
              'initial-connection-error-reconnect',
              async () => app.capture()
            );
          } finally {
            await evidence.flushOutputs(apps, launcher);
            await launcher.release();
            await evidence.release();
          }
        });
      });
    }

    it(`browses WebDAV after recovering from initial connection errors with auto_close=${autoClose}`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const server = await createWebdavTestServer('none', undefined);
        await server.close();
        let serverRunning = false;
        const evidence = createTestEvidence(context);
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
        const apps: GtkApp[] = [];
        try {
          const config = join(directory, 'webdav.ini');
          await writeFile(
            config,
            `[general]\ntype=webdav\nauto_close=${autoClose}\n[webdav]\nscheme=http\nauthentication=none\naddress=127.0.0.1\nport=${server.port}\nbase_path=/dav/\nlocal_directory=${directory}\n`
          );
          const app = await launcher.launch(['-c', config], {
            onOutput: evidence.recordAppOutputEvent,
          });
          apps.push(app);
          await waitForResult(async () =>
            expect(
              await expectElementKind(
                await app.getById(
                  'file_transfer_connection_notice_title_label'
                ),
                'label'
              ).text()
            ).toBe('Failed to start WebDAV')
          );
          const reconnect = expectElementKind(
            await app.getById('file_transfer_reconnect_button'),
            'button'
          );
          await waitForResult(async () =>
            expect((await reconnect.info()).states).toContain('showing')
          );
          await server.restart();
          serverRunning = true;
          server.files.set('/dav/recovered.txt', {
            content: Buffer.from('recovered'),
            reportedSize: 9,
          });
          await reconnect.click();
          const tree = expectElementKind(
            await app.getById('file_transfer_remote_tree'),
            'table'
          );
          await waitForResult(async () => {
            const names: string[] = [];
            for (let row = 0; row < (await tree.getRowCount()); ++row)
              names.push(
                (await (await tree.cellAt(row, 0))?.info())?.name ?? ''
              );
            expect(names).toContain('recovered.txt');
            expect((await reconnect.info()).states).not.toContain('showing');
          });
          await evidence.captureEvidence('initial-error-recovered', async () =>
            app.capture()
          );
        } finally {
          await evidence.flushOutputs(apps, launcher);
          await launcher.release();
          if (serverRunning) await server.close();
          await evidence.release();
        }
      });
    });
  }
});
