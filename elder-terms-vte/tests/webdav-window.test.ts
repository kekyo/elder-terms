import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
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
          appPath: fileURLToPath(
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
