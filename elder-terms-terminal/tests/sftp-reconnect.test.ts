// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { appendFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { createConnection, createServer, type Socket } from 'node:net';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';
import { withTemporaryDirectory } from './gtk-test-helpers';
import { startSshLatencyFixture } from './performance/ssh-latency-fixture.mjs';

for (const shared of [false, true]) {
  it(`reconnects SFTP after an established SSH connection drops: shared=${shared}`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const remote = join(directory, 'remote');
      await mkdir(remote);
      await writeFile(join(remote, 'before.txt'), 'before');
      const peer = join(directory, 'peer.sh');
      await writeFile(
        peer,
        '#!/bin/sh\nif [ -n "$SSH_ORIGINAL_COMMAND" ]; then exec /usr/lib/openssh/sftp-server; fi\nprintf "READY\\n"\nexec cat\n',
        { mode: 0o700 }
      );
      const fixture = await startSshLatencyFixture(directory, [peer]);
      await appendFile(
        join(directory, 'ssh-fixture', 'sshd_config'),
        'Subsystem sftp /usr/lib/openssh/sftp-server\n'
      );
      const sockets = new Set<Socket>();
      const bridge = createServer((incoming) => {
        const upstream = createConnection({
          host: '127.0.0.1',
          port: fixture.port,
        });
        sockets.add(incoming);
        sockets.add(upstream);
        incoming.pipe(upstream).pipe(incoming);
        incoming.on('error', () => upstream.destroy());
        upstream.on('error', () => incoming.destroy());
        incoming.on('close', () => {
          sockets.delete(incoming);
          upstream.destroy();
        });
        upstream.on('close', () => {
          sockets.delete(upstream);
          incoming.destroy();
        });
      });
      const evidence = createTestEvidence(context);
      const launcher = createGtkAppLauncher({
        appPath: fileURLToPath(
          new URL(
            `../../.build/elder-terms-terminal/${shared ? 'elder-terms-terminal' : 'elder-terms-file-transfer'}`,
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
        await new Promise<void>((resolve, reject) => {
          bridge.once('error', reject);
          bridge.listen(0, '127.0.0.1', resolve);
        });
        const address = bridge.address();
        if (!address || typeof address === 'string')
          throw new Error('No bridge address');
        const knownHosts = join(directory, 'known_hosts');
        await writeFile(
          knownHosts,
          (await readFile(fixture.knownHosts, 'utf8')).replace(
            `[127.0.0.1]:${fixture.port}`,
            `[127.0.0.1]:${address.port}`
          )
        );
        const config = join(directory, 'connection.ini');
        await writeFile(
          config,
          [
            '[general]',
            `type=${shared ? 'ssh' : 'sftp'}`,
            'auto_close=false',
            '[ssh]',
            'address=127.0.0.1',
            `port=${address.port}`,
            `username=${fixture.username}`,
            `identity_file=${fixture.clientKey}`,
            '[sftp]',
            'address=127.0.0.1',
            `port=${address.port}`,
            `username=${fixture.username}`,
            `identity_file=${fixture.clientKey}`,
            `remote_directory=${remote}`,
            `local_directory=${directory}`,
            '',
          ].join('\n')
        );
        const app = await launcher.launch(
          ['-c', config, `--test-ssh-known-hosts-file=${knownHosts}`],
          { onOutput: evidence.recordAppOutputEvent }
        );
        const acceptInitial = expectElementKind(
          await app.getById(
            shared
              ? 'ssh_prompt_accept_button'
              : 'file_transfer_prompt_accept_button'
          ),
          'button'
        );
        await waitForResult(async () =>
          expect((await acceptInitial.info()).states).toContain('showing')
        );
        await acceptInitial.click();
        if (shared) {
          const menu = expectElementKind(
            await app.getById('transfer_button'),
            'toggleButton'
          );
          await waitForResult(async () =>
            expect((await menu.info()).states).toContain('sensitive')
          );
          await menu.click();
          await expectElementKind(
            await app.getById('transfer_sftp_item'),
            'menuItem'
          ).click();
        }
        const tree = expectElementKind(
          await app.getById('file_transfer_remote_tree'),
          'table'
        );
        const expectFile = async (name: string): Promise<void> => {
          await waitForResult(async () => {
            const names: string[] = [];
            for (let row = 0; row < (await tree.getRowCount()); ++row)
              names.push(
                (await (await tree.cellAt(row, 0))?.info())?.name ?? ''
              );
            expect(names).toContain(name);
          });
        };
        await expectFile('before.txt');
        for (const socket of sockets) socket.destroy();
        await expectElementKind(
          await app.getById('file_transfer_remote_refresh_button'),
          'button'
        ).click();
        const reconnect = expectElementKind(
          await app.getById('file_transfer_reconnect_button'),
          'button'
        );
        await waitForResult(async () =>
          expect((await reconnect.info()).states).toContain('showing')
        );
        await writeFile(join(remote, 'after.txt'), 'after');
        await reconnect.click();
        const accept = expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        );
        await waitForResult(async () =>
          expect((await accept.info()).states).toContain('showing')
        );
        await accept.click();
        await expectFile('after.txt');
        expect((await reconnect.info()).states).not.toContain('showing');
        if (shared)
          expect(
            (await (await app.getById('reconnect_button')).info()).states
          ).toContain('showing');
        await evidence.captureEvidence('sftp-reconnected', async () =>
          app.capture()
        );
        await evidence.flushOutputs([app], launcher);
      } finally {
        await launcher.release();
        await evidence.release();
        for (const socket of sockets) socket.destroy();
        if (bridge.listening)
          await new Promise<void>((resolve) => bridge.close(() => resolve()));
        await fixture.stop();
      }
    });
  });
}
