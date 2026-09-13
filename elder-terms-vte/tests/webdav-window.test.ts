import {
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkTableElement,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';
import { createWebdavTestServer } from './webdav-test-server';

describe('WebDAV window', () => {
  for (const [scheme, authentication, initialDirectory] of [
    ['http', 'none', '/'],
    ['http', 'basic', '/'],
    ['http', 'digest', '/'],
    ['https', 'basic', '/'],
    ['http', 'none', '/hold'],
  ] as const) {
    it(`opens real ${scheme} ${initialDirectory} using ${authentication} authentication in the common window`, async (context) => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-webdav-'));
      let tls: { cert: string; key: string } | undefined;
      const certPath = join(directory, 'ca.pem');
      if (scheme === 'https') {
        const keyPath = join(directory, 'key.pem');
        const child = spawn(
          'openssl',
          [
            'req',
            '-x509',
            '-newkey',
            'rsa:2048',
            '-nodes',
            '-days',
            '2',
            '-subj',
            '/CN=127.0.0.1',
            '-addext',
            'subjectAltName=IP:127.0.0.1',
            '-keyout',
            keyPath,
            '-out',
            certPath,
          ],
          { stdio: 'ignore' }
        );
        const [code] = await once(child, 'close');
        expect(code).toBe(0);
        tls = {
          cert: await readFile(certPath, 'utf8'),
          key: await readFile(keyPath, 'utf8'),
        };
      }
      const server = await createWebdavTestServer(authentication, tls);
      const evidence = createTestEvidence(context);
      const apps: GtkApp[] = [];
      let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
      try {
        const local = join(directory, 'local');
        await mkdir(local);
        const configPath = join(directory, 'webdav.ini');
        await writeFile(
          configPath,
          [
            '[general]',
            'type=webdav',
            'name=DAV files',
            '[webdav]',
            `scheme=${scheme}`,
            `ca_file=${scheme === 'https' ? certPath : ''}`,
            'address=127.0.0.1',
            `port=${server.port}`,
            'base_path=/dav/',
            `remote_directory=${initialDirectory}`,
            `authentication=${authentication}`,
            'username=alice',
            `local_directory=${local}`,
            '',
          ].join('\n')
        );
        launcher = createGtkAppLauncher({
          appPath:
            process.env.ELDER_TERMS_TEST_FTP_APP ??
            fileURLToPath(
              new URL(
                '../../.build/elder-terms-vte/elder-terms-file-transfer',
                import.meta.url
              )
            ),
          env: {
            LANGUAGE: 'en',
            LC_ALL: 'C.UTF-8',
            XDG_CONFIG_HOME: join(directory, 'config'),
          },
          onSystemOutput: evidence.recordSystemOutputEvent,
          xvfbTrayHost: true,
        });
        const app = await launcher.launch(['-c', configPath], {
          onOutput: evidence.recordAppOutputEvent,
        });
        apps.push(app);
        if (initialDirectory === '/hold') {
          await server.held;
          const pending: GtkWidgetElement[] = [
            await app.getById('file_transfer_header_bar'),
          ];
          let closed = false;
          while (pending.length > 0) {
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
                index += 1
              ) {
                const child = await widget.childAt(index);
                if (child !== undefined) pending.push(child);
              }
            }
          }
          expect(closed).toBe(true);
          await waitForResult(async () => {
            const output = await app.output();
            expect(output.exitCode, output.stderr).toBe(0);
            expect(output.exitSignal).toBeNull();
          });
          return;
        }
        if (authentication !== 'none') {
          const password = expectElementKind(
            await app.getById('file_transfer_prompt_secondary_entry'),
            'entry'
          );
          await password.setText('secret');
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
        }
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('file_transfer_status_label'),
              'label'
            ).text()
          ).toBe('Ready');
        });
        const tree = (await app.getById(
          'file_transfer_remote_tree'
        )) as GtkTableElement;
        const names: string[] = [];
        for (let row = 0; row < (await tree.getRowCount()); row += 1) {
          const name = (await (await tree.cellAt(row, 0))?.info())?.name ?? '';
          names.push(name);
          if (name === 'unknown.txt') {
            expect(
              (await (await tree.cellAt(row, 1))?.info())?.name ?? ''
            ).toBe('');
          }
        }
        expect(names).toEqual(
          expect.arrayContaining([
            'hello.txt',
            '資料 #+%.txt',
            'unknown.txt',
            'nested',
          ])
        );
        expect(names).not.toContain('Not the resource identity');
        await evidence.captureEvidence('webdav-ready', async () =>
          expectElementKind(
            await app.getById('file_transfer_window'),
            'window'
          ).capture()
        );
        expect(
          server.requests.some(
            (request) => request.method === 'PROPFIND' && request.authenticated
          )
        ).toBe(true);
        expect(
          await expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          ).text()
        ).toBe('/');
        const remotePath = expectElementKind(
          await app.getById('file_transfer_remote_path_entry'),
          'entry'
        );
        const bounds = (await remotePath.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round(bounds.x + bounds.width / 2),
          Math.round(bounds.y + bounds.height / 2)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await waitForResult(async () => {
          expect((await remotePath.info()).states).toContain('focused');
        });
        await remotePath.setText('/nested');
        await app.input.pressKey('Return');
        await waitForResult(async () => {
          const listed: string[] = [];
          for (let row = 0; row < (await tree.getRowCount()); row += 1)
            listed.push(
              (await (await tree.cellAt(row, 0))?.info())?.name ?? ''
            );
          expect(listed).toContain('child.txt');
        });
        await expectElementKind(
          await app.getById('file_transfer_remote_up_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await remotePath.text()).toBe('/');
        });

        await waitForResult(async () => {
          expect(await tree.getRowCount()).toBe(4);
        });
        for (let row = 0; row < 4; row += 1) await tree.selectRow(row);
        expect(await tree.selectedRows()).toHaveLength(4);
        const firstCell = await tree.cellAt(0, 0);
        if (firstCell === undefined)
          throw new Error('Remote collection has no selectable row');
        const cellBounds = (await firstCell.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round(cellBounds.x + cellBounds.width / 2),
          Math.round(cellBounds.y + cellBounds.height / 2)
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
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('file_transfer_status_label'),
              'label'
            ).text()
          ).toBe('Received 4 items');
          expect(await readFile(join(local, 'hello.txt'), 'utf8')).toBe(
            'Hello DAV!\r\n'
          );
          expect(await readFile(join(local, '資料 #+%.txt'))).toHaveLength(0);
          expect(await readFile(join(local, 'unknown.txt'), 'utf8')).toBe(
            'unknown length\n'
          );
          expect(await readFile(join(local, 'nested/child.txt'), 'utf8')).toBe(
            'child\n'
          );
        });

        const localTree = expectElementKind(
          await app.getById('file_transfer_local_tree'),
          'table'
        ) as GtkTableElement;
        const rowFor = async (table: GtkTableElement, name: string) =>
          await waitForResult(async () => {
            expect((await table.info()).states).toContain('sensitive');
            for (let row = 0; row < (await table.getRowCount()); row += 1)
              if ((await (await table.cellAt(row, 0))?.info())?.name === name)
                return row;
            throw new Error(`Missing DAV row: ${name}`);
          });
        const contextMenu = async (table: GtkTableElement, name: string) => {
          const row = await rowFor(table, name);
          for (const selected of await table.selectedRows())
            await table.deselectRow(selected);
          await table.selectRow(row);
          const cell = await table.cellAt(row, 0);
          if (cell === undefined) throw new Error(`Missing DAV cell: ${name}`);
          const bounds = (await cell.capture()).bounds;
          await app.input.moveMouseTo(
            Math.round(bounds.x + bounds.width / 2),
            Math.round(bounds.y + bounds.height / 2)
          );
          await app.input.setMouseButton('right', true);
          await app.input.setMouseButton('right', false);
        };
        const navigate = async (path: string) => {
          // Native input needs window focus after the menu and inline prompt.
          await expectElementKind(
            await app.getById('file_transfer_window'),
            'window'
          ).activate();
          const bounds = (await remotePath.capture()).bounds;
          await app.input.moveMouseTo(
            Math.round(bounds.x + bounds.width / 2),
            Math.round(bounds.y + bounds.height / 2)
          );
          await app.input.setMouseButton('left', true);
          await app.input.setMouseButton('left', false);
          await waitForResult(async () => {
            expect((await remotePath.info()).states).toContain('focused');
          });
          await remotePath.setText(path);
          await app.input.pressKey('Return');
          await waitForResult(async () => {
            expect(await remotePath.text()).toBe(path);
            expect((await tree.info()).states).toContain('sensitive');
          });
        };
        const acceptName = async (name: string) => {
          const entry = expectElementKind(
            await app.getById('file_transfer_prompt_entry'),
            'entry'
          );
          await waitForResult(async () => {
            expect((await entry.info()).states).toContain('showing');
          });
          await entry.setText(name);
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
        };
        await contextMenu(tree, 'hello.txt');
        const newFolder = expectElementKind(
          await app.getById('file_transfer_remote_new_directory_item'),
          'menuItem'
        );
        await newFolder.click();
        await acceptName('UI folder');
        await rowFor(tree, 'UI folder');
        expect(server.directories.has('/dav/UI folder')).toBe(true);
        await navigate('/UI folder');
        await waitForResult(async () => {
          expect(await tree.getRowCount()).toBe(0);
        });
        const emptyBounds = (await tree.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round(emptyBounds.x + emptyBounds.width / 2),
          Math.round(emptyBounds.y + emptyBounds.height / 2)
        );
        await app.input.setMouseButton('right', true);
        await app.input.setMouseButton('right', false);
        await waitForResult(async () => {
          expect((await newFolder.info()).states).toContain('showing');
        });
        expect((await newFolder.info()).states).toContain('sensitive');
        for (const id of [
          'file_transfer_receive_item',
          'file_transfer_remote_rename_item',
          'file_transfer_remote_delete_item',
        ])
          expect((await (await app.getById(id)).info()).states).not.toContain(
            'sensitive'
          );
        await newFolder.click();
        await acceptName('empty child');
        await rowFor(tree, 'empty child');
        expect(server.directories.has('/dav/UI folder/empty child')).toBe(true);
        await contextMenu(tree, 'empty child');
        await newFolder.click();
        await expectElementKind(
          await app.getById('file_transfer_prompt_entry'),
          'entry'
        ).setText('cancelled folder');
        await expectElementKind(
          await app.getById('file_transfer_prompt_cancel_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect((await tree.info()).states).toContain('sensitive');
        });
        expect(server.directories.has('/dav/UI folder/cancelled folder')).toBe(
          false
        );
        await contextMenu(tree, 'empty child');
        await newFolder.click();
        await acceptName('empty child');
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('file_transfer_prompt_title_label'),
              'label'
            ).text()
          ).toBe('Failed to create folder');
        });
        await expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        ).click();
        await rowFor(tree, 'empty child');
        expect(server.directories.has('/dav/UI folder/empty child')).toBe(true);
        await navigate('/');
        await rowFor(tree, 'UI folder');

        await mkdir(join(local, 'send-tree/nested'), { recursive: true });
        await writeFile(
          join(local, 'send-tree/資料 #+%.txt'),
          'upload from shared UI'
        );
        await writeFile(join(local, 'send-tree/nested/empty.txt'), '');
        await expectElementKind(
          await app.getById('file_transfer_local_refresh_button'),
          'button'
        ).click();
        await contextMenu(localTree, 'send-tree');
        await expectElementKind(
          await app.getById('file_transfer_send_item'),
          'menuItem'
        ).click();
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('file_transfer_status_label'),
              'label'
            ).text()
          ).toBe('Sent 1 item');
          expect(
            server.files.get('/dav/send-tree/資料 #+%.txt')?.content.toString()
          ).toBe('upload from shared UI');
          expect(
            server.files.get('/dav/send-tree/nested/empty.txt')?.content
          ).toHaveLength(0);
        });
        await writeFile(
          join(local, 'send-tree/資料 #+%.txt'),
          'replacement from shared UI'
        );
        await contextMenu(localTree, 'send-tree');
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
          expect(
            await expectElementKind(
              await app.getById('file_transfer_status_label'),
              'label'
            ).text()
          ).toBe('Sent 1 item');
          expect(
            server.files.get('/dav/send-tree/資料 #+%.txt')?.content.toString()
          ).toBe('replacement from shared UI');
        });
        await contextMenu(tree, 'send-tree');
        await expectElementKind(
          await app.getById('file_transfer_remote_rename_item'),
          'menuItem'
        ).click();
        await acceptName('renamed 日本語 #');
        await rowFor(tree, 'renamed 日本語 #');
        expect(
          server.files
            .get('/dav/renamed 日本語 #/資料 #+%.txt')
            ?.content.toString()
        ).toBe('replacement from shared UI');
        expect(server.directories.has('/dav/send-tree')).toBe(false);
        await contextMenu(tree, 'renamed 日本語 #');
        await expectElementKind(
          await app.getById('file_transfer_remote_delete_item'),
          'menuItem'
        ).click();
        await expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('file_transfer_status_label'),
              'label'
            ).text()
          ).toBe('Deleted 1 item');
          expect(server.directories.has('/dav/renamed 日本語 #')).toBe(false);
          expect(server.files.has('/dav/renamed 日本語 #/資料 #+%.txt')).toBe(
            false
          );
        });
        await contextMenu(localTree, 'send-tree');
        await expectElementKind(
          await app.getById('file_transfer_local_new_directory_item'),
          'menuItem'
        ).click();
        await acceptName('local folder');
        await rowFor(localTree, 'local folder');
        expect(await readdir(join(local, 'local folder'))).toEqual([]);
        await evidence.captureEvidence('webdav-file-management', async () =>
          expectElementKind(
            await app.getById('file_transfer_window'),
            'window'
          ).capture()
        );
        await writeFile(
          join(local, 'cancel-upload.txt'),
          Buffer.alloc(131072, 'c')
        );
        await expectElementKind(
          await app.getById('file_transfer_local_refresh_button'),
          'button'
        ).click();
        await contextMenu(localTree, 'cancel-upload.txt');
        await expectElementKind(
          await app.getById('file_transfer_send_item'),
          'menuItem'
        ).click();
        await server.uploadHeld;
        await expectElementKind(
          await app.getById('file_transfer_cancel_button'),
          'button'
        ).click();
        const temporary = [...server.files.keys()].find((path) =>
          path.startsWith('/dav/cancel-upload.txt.elder-terms-part-')
        );
        if (temporary === undefined)
          throw new Error(
            'The fixture must retain the upload whose final response was withheld'
          );
        const notice = await waitForResult(async () => {
          const dialog = await app.getById(
            'file_transfer_operation_error_dialog'
          );
          expect((await dialog.info()).states).toContain('showing');
          expect((await dialog.info()).states).not.toContain('modal');
          const parent = await app.getById('file_transfer_window');
          expect((await parent.info()).states).not.toContain('enabled');
          const pending: GtkWidgetElement[] = [dialog];
          const labels: string[] = [];
          while (pending.length) {
            const widget = pending.shift()!;
            labels.push((await widget.info()).name ?? '');
            if ('getChildCount' in widget)
              for (
                let index = 0;
                index < (await widget.getChildCount());
                index += 1
              ) {
                const child = await widget.childAt(index);
                if (child !== undefined) pending.push(child);
              }
          }
          expect(labels.join('\n')).toContain(temporary.slice('/dav'.length));
          expect(
            await expectElementKind(
              await app.getById('file_transfer_status_label'),
              'label'
            ).text()
          ).toBe('Transfer cancelled');
          return dialog;
        });
        expect(server.files.has('/dav/cancel-upload.txt')).toBe(false);
        expect(
          server.requests.some(
            (request) =>
              request.method === 'DELETE' && request.url === temporary
          )
        ).toBe(false);
        await evidence.captureEvidence(
          'webdav-cancelled-upload-notice',
          async () => notice.capture()
        );
      } catch (error) {
        await evidence.log('WebDAV UI failure', error);
        if (apps.length) {
          try {
            await evidence.captureEvidence('webdav-ui-failure', async () =>
              apps[0].capture()
            );
          } catch (captureError) {
            await evidence.log('Failure capture unavailable', captureError);
          }
        }
        throw error;
      } finally {
        try {
          await evidence.log('WebDAV requests', server.requests);
          await evidence.flushOutputs(apps, launcher);
        } finally {
          if (launcher !== undefined) await launcher.release();
          await server.close();
          await evidence.release();
          await rm(directory, { recursive: true, force: true });
        }
      }
    });
  }
});
