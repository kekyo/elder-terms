// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { readFile, writeFile } from 'node:fs/promises';
import { createServer, type Socket } from 'node:net';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind } from './test-helpers.ts';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers.ts';
import { startSshLatencyFixture } from './performance/ssh-latency-fixture.mts';

describe.concurrent('terminal connection error recovery', () => {
  for (const autoClose of [false, true]) {
    for (const kind of ['local', 'telnet'] as const) {
      it(`retries ${kind} startup failures with auto_close=${autoClose}`, async (context) => {
        await withTemporaryDirectory(async (directory) => {
          const sockets = new Set<Socket>();
          const server = createServer((socket) => {
            sockets.add(socket);
            socket.on('error', () => {});
            socket.on('close', () => sockets.delete(socket));
            socket.write('SESSION_READY\r\n');
          });
          try {
            await new Promise<void>((resolve, reject) => {
              server.once('error', reject);
              server.listen(0, '127.0.0.1', resolve);
            });
            const address = server.address();
            if (!address || typeof address === 'string')
              throw new Error('Missing test server address');
            await new Promise<void>((resolve) => server.close(() => resolve()));
            const peer = join(directory, 'peer.sh');
            const config = join(directory, 'connection.ini');
            const log = join(directory, 'terminal.log');
            await writeFile(
              config,
              [
                '[general]',
                `type=${kind}`,
                `auto_close=${autoClose}`,
                '[local]',
                `command_line=${peer}`,
                '[telnet]',
                'address=127.0.0.1',
                `port=${address.port}`,
                '[log]',
                'enabled=true',
                `base_directory=${directory}`,
                'file_name_format=terminal.log',
                'mode=cooked',
                '',
              ].join('\n')
            );
            await runGtkTest(context, ['-c', config], async (app, evidence) => {
              const reconnect = expectElementKind(
                await app.getById('reconnect_button'),
                'button'
              );
              for (let attempt = 1; attempt <= 2; ++attempt) {
                await waitForResult(async () => {
                  expect(
                    (await app.output()).stderr.match(/Error:/g)
                  ).toHaveLength(attempt);
                  expect((await reconnect.info()).states).toContain('showing');
                  expect((await reconnect.info()).states).toContain(
                    'sensitive'
                  );
                  expect((await app.output()).exitCode).toBeNull();
                });
                if (attempt === 1) await reconnect.click();
              }
              await evidence.captureEvidence(
                'connection-error-reconnect',
                async () => app.capture()
              );
              if (kind === 'local') {
                await writeFile(
                  peer,
                  '#!/bin/sh\nprintf "SESSION_READY\\r\\n"\nexec cat\n',
                  { mode: 0o700 }
                );
              } else {
                await new Promise<void>((resolve) =>
                  server.listen(address.port, '127.0.0.1', resolve)
                );
              }
              await reconnect.click();
              await waitForResult(async () => {
                expect(await readFile(log, 'utf8')).toContain('SESSION_READY');
                expect((await reconnect.info()).states).not.toContain(
                  'showing'
                );
                expect(
                  (await (await app.getById('disconnected_notice')).info())
                    .states
                ).not.toContain('showing');
              });
              if (kind === 'telnet') {
                for (const socket of sockets) socket.end();
                await waitForResult(async () => {
                  if (autoClose) expect((await app.output()).exitCode).toBe(0);
                  else
                    expect((await reconnect.info()).states).toContain(
                      'showing'
                    );
                });
              }
            });
          } finally {
            for (const socket of sockets) socket.destroy();
            if (server.listening)
              await new Promise<void>((resolve) =>
                server.close(() => resolve())
              );
          }
        });
      });
    }

    for (const changed of [false, true]) {
      it(`retries a rejected ${changed ? 'changed' : 'unknown'} SSH host key with auto_close=${autoClose}`, async (context) => {
        await withTemporaryDirectory(async (directory) => {
          const peer = join(directory, 'peer.sh');
          await writeFile(
            peer,
            '#!/bin/sh\nstty -echo\nprintf "SESSION_READY\\r\\n"\nIFS= read -r line\n',
            { mode: 0o700 }
          );
          const fixture = await startSshLatencyFixture(directory, [peer]);
          try {
            await writeFile(
              fixture.knownHosts,
              changed
                ? `[127.0.0.1]:${fixture.port} ${(await readFile(`${fixture.clientKey}.pub`, 'utf8')).trim()}\n`
                : ''
            );
            const config = join(directory, 'ssh.ini');
            const log = join(directory, 'terminal.log');
            await writeFile(
              config,
              [
                '[general]',
                'type=ssh',
                `auto_close=${autoClose}`,
                '[ssh]',
                'address=127.0.0.1',
                `port=${fixture.port}`,
                `username=${fixture.username}`,
                `identity_file=${fixture.clientKey}`,
                '[log]',
                'enabled=true',
                `base_directory=${directory}`,
                'file_name_format=terminal.log',
                'mode=cooked',
                '',
              ].join('\n')
            );
            await runGtkTest(
              context,
              [
                '-c',
                config,
                `--test-ssh-known-hosts-file=${fixture.knownHosts}`,
              ],
              async (app, evidence) => {
                const title = expectElementKind(
                  await app.getById('ssh_prompt_title_label'),
                  'label'
                );
                const accept = expectElementKind(
                  await app.getById('ssh_prompt_accept_button'),
                  'button'
                );
                const cancel = expectElementKind(
                  await app.getById('ssh_prompt_cancel_button'),
                  'button'
                );
                const reconnect = expectElementKind(
                  await app.getById('reconnect_button'),
                  'button'
                );
                for (let attempt = 1; attempt <= 3; ++attempt) {
                  await waitForResult(async () => {
                    expect(await title.text()).toBe('SSH Authentication');
                    expect((await accept.info()).states).toContain('showing');
                    expect((await reconnect.info()).states).not.toContain(
                      'showing'
                    );
                  });
                  await accept.click();
                  await waitForResult(async () =>
                    expect(await title.text()).toBe(
                      changed ? 'SSH Host Key Changed' : 'SSH Host Key'
                    )
                  );
                  if (attempt === 3) {
                    await expectElementKind(
                      await app.getById(
                        changed
                          ? 'ssh_prompt_alternative_button'
                          : 'ssh_prompt_accept_button'
                      ),
                      'button'
                    ).click();
                    break;
                  }
                  await cancel.click();
                  await waitForResult(async () => {
                    expect(
                      await expectElementKind(
                        await app.getById('disconnected_notice_label'),
                        'label'
                      ).text()
                    ).toContain(
                      changed
                        ? 'SSH host key was not reset'
                        : 'SSH host key was not accepted'
                    );
                    expect((await reconnect.info()).states).toContain(
                      'showing'
                    );
                    expect((await reconnect.info()).states).toContain(
                      'sensitive'
                    );
                  });
                  await evidence.captureEvidence(
                    `ssh-host-key-error-${attempt}`,
                    async () => app.capture()
                  );
                  await reconnect.click();
                }
                await waitForResult(async () => {
                  expect(await readFile(log, 'utf8')).toContain(
                    'SESSION_READY'
                  );
                  expect((await reconnect.info()).states).not.toContain(
                    'showing'
                  );
                });
                await app.input.pressKey('Return');
                await waitForResult(async () => {
                  if (autoClose) expect((await app.output()).exitCode).toBe(0);
                  else
                    expect((await reconnect.info()).states).toContain(
                      'showing'
                    );
                });
              }
            );
          } finally {
            await fixture.stop();
          }
        });
      });
    }
  }
});
