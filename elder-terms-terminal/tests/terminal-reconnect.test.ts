// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { readFile, writeFile } from 'node:fs/promises';
import { createConnection, createServer, type Socket } from 'node:net';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind } from './test-helpers';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';
import { startSshLatencyFixture } from './performance/ssh-latency-fixture.mjs';

describe.concurrent('terminal reconnection', () => {
  it('restarts a local command after exit and exchanges new input', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const peer = join(directory, 'local.sh');
      await writeFile(
        peer,
        '#!/bin/sh\nstty -echo\nprintf "SESSION_READY\\r\\n"\nIFS= read -r line\nprintf "ACK_%s\\r\\n" "$line"\n',
        { mode: 0o700 }
      );
      const config = join(directory, 'local.ini');
      const log = join(directory, 'terminal.log');
      await writeFile(
        config,
        [
          '[general]',
          'type=local',
          'auto_close=false',
          '[terminal]',
          'return_code=lf',
          '[local]',
          `command_line=${peer}`,
          '[log]',
          'enabled=true',
          `base_directory=${directory}`,
          'file_name_format=terminal.log',
          'mode=cooked',
          '',
        ].join('\n')
      );
      await runGtkTest(context, ['-c', config], async (app) => {
        for (let round = 1; round <= 3; ++round) {
          await waitForResult(async () =>
            expect(
              (await readFile(log, 'utf8')).match(/SESSION_READY/g)
            ).toHaveLength(round)
          );
          const bounds = (await (await app.getById('terminal_view')).capture())
            .bounds;
          await app.input.moveMouseTo(bounds.x + 20, bounds.y + 20);
          await app.input.setMouseButton('left', true);
          await app.input.setMouseButton('left', false);
          for (const key of ['p', 'i', 'n', 'g', 'Return'])
            await app.input.pressKey(key);
          const reconnect = expectElementKind(
            await app.getById('reconnect_button'),
            'button'
          );
          await waitForResult(async () => {
            expect(
              (await readFile(log, 'utf8')).match(/ACK_ping/g)
            ).toHaveLength(round);
            expect((await reconnect.info()).states).toContain('showing');
            expect((await reconnect.info()).states).toContain('sensitive');
          });
          if (round < 3) await reconnect.click();
        }
      });
    });
  });

  for (const ending of ['logout', 'network loss'] as const) {
    it(`reconnects a working SSH terminal after ${ending}`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const received = join(directory, 'received');
        const peer = join(directory, 'peer.sh');
        await writeFile(received, '');
        await writeFile(
          peer,
          [
            '#!/bin/sh',
            'stty -echo',
            'printf "SESSION_READY\\r\\n"',
            'while IFS= read -r line; do',
            '  [ "$line" = logout ] && exit 0',
            `  printf '%s\\n' "$line" >> '${received}'`,
            '  printf "ACK_%s\\r\\n" "$line"',
            'done',
            '',
          ].join('\n'),
          { mode: 0o700 }
        );
        const fixture = await startSshLatencyFixture(directory, [peer]);
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
        try {
          await new Promise<void>((resolve, reject) => {
            bridge.once('error', reject);
            bridge.listen(0, '127.0.0.1', resolve);
          });
          const address = bridge.address();
          if (!address || typeof address === 'string')
            throw new Error('Missing bridge address');
          const knownHosts = join(directory, 'known_hosts');
          await writeFile(
            knownHosts,
            (await readFile(fixture.knownHosts, 'utf8')).replace(
              `[127.0.0.1]:${fixture.port}`,
              `[127.0.0.1]:${address.port}`
            )
          );
          const config = join(directory, 'ssh.ini');
          const log = join(directory, 'terminal.log');
          await writeFile(
            config,
            [
              '[general]',
              'type=ssh',
              `auto_close=${ending === 'network loss'}`,
              '[terminal]',
              'return_code=lf',
              '[ssh]',
              'address=127.0.0.1',
              `port=${address.port}`,
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
            ['-c', config, `--test-ssh-known-hosts-file=${knownHosts}`],
            async (app, evidence) => {
              for (let round = 1; round <= 2; ++round) {
                const accept = expectElementKind(
                  await app.getById('ssh_prompt_accept_button'),
                  'button'
                );
                await waitForResult(async () =>
                  expect((await accept.info()).states).toContain('showing')
                );
                await accept.click();
                await waitForResult(async () => {
                  const text = await readFile(log, 'utf8');
                  expect(text.match(/SESSION_READY/g)).toHaveLength(round);
                });
                if (round === 1 && ending === 'network loss') {
                  await expectElementKind(
                    await app.getById('application_menu_button'),
                    'toggleButton'
                  ).click();
                  await expectElementKind(
                    await app.getById('settings_menu_item'),
                    'menuItem'
                  ).click();
                  await expectElementKind(
                    await app.getById('settings_general_auto_close_combo'),
                    'comboBox'
                  ).selectChildAt(2);
                  await expectElementKind(
                    await app.getById('settings_apply_button'),
                    'button'
                  ).click();
                  await expectElementKind(
                    await app.getById('settings_cancel_button'),
                    'button'
                  ).click();
                }
                const terminal = await app.getById('terminal_view');
                const bounds = (await terminal.capture()).bounds;
                await app.input.moveMouseTo(bounds.x + 20, bounds.y + 20);
                await app.input.setMouseButton('left', true);
                await app.input.setMouseButton('left', false);
                for (const key of ['p', 'i', 'n', 'g', 'Return'])
                  await app.input.pressKey(key);
                await waitForResult(async () => {
                  expect(
                    (await readFile(received, 'utf8')).match(/ping/g)
                  ).toHaveLength(round);
                  expect(
                    (await readFile(log, 'utf8')).match(/ACK_ping/g)
                  ).toHaveLength(round);
                });
                if (ending === 'logout') {
                  for (const key of [...'logout', 'Return'])
                    await app.input.pressKey(key);
                } else {
                  for (const socket of sockets) socket.destroy();
                }
                const reconnect = expectElementKind(
                  await app.getById('reconnect_button'),
                  'button'
                );
                await waitForResult(async () => {
                  expect(
                    (await (await app.getById('disconnected_notice')).info())
                      .states
                  ).toContain('showing');
                  expect((await reconnect.info()).states).toContain('showing');
                  expect((await reconnect.info()).states).toContain(
                    'sensitive'
                  );
                });
                await evidence.captureEvidence(
                  `ssh-${ending}-${round}`,
                  async () => (await app.getById('main_window')).capture()
                );
                if (round === 1) await reconnect.click();
              }
            }
          );
        } finally {
          for (const socket of sockets) socket.destroy();
          if (bridge.listening)
            await new Promise<void>((resolve, reject) =>
              bridge.close((error) => (error ? reject(error) : resolve()))
            );
          await fixture.stop();
        }
      });
    });
  }
});
