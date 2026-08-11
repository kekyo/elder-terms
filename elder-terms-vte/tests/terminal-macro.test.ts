import { chmod, readFile, writeFile } from 'node:fs/promises';
import { createServer, type Server, type Socket } from 'node:net';
import { join } from 'node:path';
import type { AddressInfo } from 'node:net';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

const listenOnLocalhost = async (server: Server): Promise<number> =>
  new Promise<number>((resolve, reject) => {
    const rejectFromError = (error: Error): void => {
      reject(error);
    };
    server.once('error', rejectFromError);
    server.listen(0, '127.0.0.1', () => {
      server.off('error', rejectFromError);
      resolve((server.address() as AddressInfo).port);
    });
  });

const closeServer = async (server: Server): Promise<void> =>
  new Promise<void>((resolve, reject) => {
    if (!server.listening) {
      resolve();
      return;
    }
    server.close((error) => {
      if (error === undefined) {
        resolve();
      } else {
        reject(error);
      }
    });
  });

const connectionConfig = (port: number, macro: readonly string[]): string =>
  [
    '[general]',
    'type=telnet',
    '',
    '[terminal]',
    'auto_close=false',
    'return_code=lf',
    '',
    '[transfer]',
    'text_send_follow_return_code=true',
    '',
    '[telnet]',
    'address=127.0.0.1',
    `port=${port}`,
    '',
    ...macro,
    '',
  ].join('\n');

describe.concurrent('elder-terms-vte terminal macros', () => {
  it('sends expanded text with the configured Return code', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let socket: Socket | undefined;
      let received = Buffer.alloc(0);
      const server = createServer((accepted) => {
        socket = accepted;
        accepted.on('data', (bytes) => {
          const chunk = typeof bytes === 'string' ? Buffer.from(bytes) : bytes;
          received = Buffer.concat([received, chunk]);
        });
        accepted.write('CHALLENGE TOKEN\r\n');
      });
      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'send-macro.ini');
        await writeFile(
          configPath,
          connectionConfig(port, [
            '[macro.reply]',
            'regex=^CHALLENGE (?<token>[A-Z]+)$',
            'send=RESPONSE ${token}\\r\\n',
          ]),
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async () => {
          await waitForResult(async () => {
            expect(received.toString('utf8')).toContain('RESPONSE TOKEN\n');
          });
        });
      } finally {
        socket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('reports only command spawn failures in a message box', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let socket: Socket | undefined;
      const server = createServer((accepted) => {
        socket = accepted;
        accepted.write('RUN\r\n');
      });
      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'failed-command-macro.ini');
        await writeFile(
          configPath,
          connectionConfig(port, [
            '[macro.fail]',
            'regex=^RUN$',
            'command=elder-terms-command-that-does-not-exist',
          ]),
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          const dialog = await waitForResult(async () =>
            expectElementKind(
              await app.getById('macro_command_error_dialog'),
              'infoBar'
            )
          );
          expect((await dialog.info()).name).toContain('Macro command failed');
        });
      } finally {
        socket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('does not report a command that spawns and exits unsuccessfully', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const markerPath = join(directory, 'command-ran.txt');
      const commandPath = join(directory, 'command.sh');
      await writeFile(
        commandPath,
        '#!/bin/sh\nprintf invoked > "$1"\nexit 7\n',
        'utf8'
      );
      await chmod(commandPath, 0o755);

      let socket: Socket | undefined;
      const server = createServer((accepted) => {
        socket = accepted;
        accepted.write('RUN\r\n');
      });
      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'nonzero-command-macro.ini');
        await writeFile(
          configPath,
          connectionConfig(port, [
            '[macro.nonzero]',
            'regex=^RUN$',
            `command=${commandPath}`,
            `arguments=${markerPath};`,
          ]),
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await waitForResult(async () => {
            expect(await readFile(markerPath, 'utf8')).toBe('invoked');
          });
          expect(
            await app.findById('macro_command_error_dialog')
          ).toBeUndefined();
        });
      } finally {
        socket?.destroy();
        await closeServer(server);
      }
    });
  });
});
