import { spawn, type ChildProcess } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { createServer } from 'node:net';
import { constants } from 'node:fs';
import { access, chmod, mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import type { GtkApp, GtkToggleButtonElement } from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import { expectElementKind } from './test-helpers';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';

type TransferProtocol = 'xmodem' | 'ymodem' | 'zmodem';
type TransferDirection = 'send' | 'receive';

interface TransferFixture {
  readonly command: readonly string[];
  readonly configPath: string;
  readonly expectedPath: string;
  readonly markerPath: string;
  readonly payload: Buffer;
  readonly sourceUri: string | undefined;
}

interface TransferSizeCase {
  readonly byteLength: number;
  readonly label: string;
  readonly timeoutMs: number;
}

interface TransferConnectionStartOptions {
  readonly configPath: string;
  readonly loginScriptPath: string;
  readonly port: number;
}

interface TransferConnection {
  readonly close: () => Promise<void>;
}

interface TransferConnectionCase {
  readonly name: string;
  readonly start: (
    options: TransferConnectionStartOptions
  ) => Promise<TransferConnection>;
}

const protocols = ['xmodem', 'ymodem', 'zmodem'] as const;
const directions = ['send', 'receive'] as const;
const startDelaysMs = [250, 1000, 3000] as const;
const maxXmodemSendAdjustedDurationMs = 10_000;
const transferSizeCasesByProtocol: Record<
  TransferProtocol,
  readonly TransferSizeCase[]
> = {
  xmodem: [
    {
      byteLength: 160,
      label: '160B',
      timeoutMs: 60_000,
    },
    {
      byteLength: 16 * 1024,
      label: '16KB',
      timeoutMs: 90_000,
    },
    {
      byteLength: 480 * 1024,
      label: '480KB',
      timeoutMs: 120_000,
    },
  ],
  ymodem: [
    {
      byteLength: 320,
      label: '320B',
      timeoutMs: 60_000,
    },
    {
      byteLength: 32 * 1024,
      label: '32KB',
      timeoutMs: 90_000,
    },
    {
      byteLength: 960 * 1024,
      label: '960KB',
      timeoutMs: 120_000,
    },
  ],
  zmodem: [
    {
      byteLength: 10 * 1024,
      label: '10KB',
      timeoutMs: 60_000,
    },
    {
      byteLength: 1024 * 1024,
      label: '1MB',
      timeoutMs: 120_000,
    },
    {
      byteLength: 30 * 1024 * 1024,
      label: '30MB',
      timeoutMs: 360_000,
    },
  ],
};

const requiredCommands = [
  '/usr/bin/socat',
  '/usr/sbin/telnetd',
  '/usr/bin/rz',
  '/usr/bin/rb',
  '/usr/bin/rx',
  '/usr/bin/sz',
  '/usr/bin/sb',
  '/usr/bin/sx',
] as const;

const shellQuote = (value: string): string =>
  `'${value.split("'").join("'\\''")}'`;

const transferMenuItemId = (
  protocol: TransferProtocol,
  direction: TransferDirection
): string => `transfer_${protocol}_${direction}_item`;

const makeTransferPayload = (byteLength: number): Buffer =>
  randomBytes(byteLength);

const expectRequiredCommands = async (): Promise<void> => {
  await Promise.all(
    requiredCommands.map(async (command) => {
      await access(command, constants.X_OK);
    })
  );
};

const unusedLocalhostPort = async (): Promise<number> =>
  new Promise<number>((resolve, reject) => {
    const server = createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const address = server.address();
      if (address === null || typeof address === 'string') {
        reject(new Error('Server did not expose a TCP port.'));
        return;
      }
      server.close((error) => {
        if (error === undefined) {
          resolve(address.port);
        } else {
          reject(error);
        }
      });
    });
  });

const waitForSocatListening = async (
  child: ChildProcess,
  stderr: () => string
): Promise<void> => {
  await waitForResult(
    async () => {
      if (child.exitCode !== null || child.signalCode !== null) {
        throw new Error(`socat exited before listening: ${stderr()}`);
      }
      expect(stderr()).toMatch(/listening/i);
    },
    {
      message: 'socat should start listening for telnetd connections',
      timeoutMs: 5_000,
    }
  );
};

const stopProcessGroup = async (child: ChildProcess): Promise<void> => {
  if (child.pid === undefined) {
    return;
  }
  if (child.exitCode !== null || child.signalCode !== null) {
    return;
  }
  const pid = child.pid;

  try {
    process.kill(-pid, 'SIGTERM');
  } catch {
    return;
  }

  const timeout = setTimeout(() => {
    try {
      process.kill(-pid, 'SIGKILL');
    } catch {
      // The process group may have exited between the timeout and kill.
    }
  }, 2_000);
  try {
    await once(child, 'exit');
  } finally {
    clearTimeout(timeout);
  }
};

const startSocatGnuTelnetd = async (
  options: TransferConnectionStartOptions
): Promise<TransferConnection> => {
  const telnetdCommand = `/usr/sbin/telnetd -h -E ${shellQuote(
    options.loginScriptPath
  )}`;
  const child = spawn(
    '/usr/bin/socat',
    [
      '-d',
      '-d',
      `TCP-LISTEN:${options.port},bind=127.0.0.1,reuseaddr,fork`,
      `EXEC:${telnetdCommand}`,
    ],
    {
      detached: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    }
  );
  let stderr = '';

  child.stderr?.on('data', (chunk: Buffer) => {
    stderr += chunk.toString('utf8');
  });
  await waitForSocatListening(child, () => stderr);

  return {
    close: async (): Promise<void> => {
      await stopProcessGroup(child);
    },
  };
};

const connectionCases: readonly TransferConnectionCase[] = [
  {
    name: 'telnetd',
    start: startSocatGnuTelnetd,
  },
];

const lrzszCommand = (
  protocol: TransferProtocol,
  direction: TransferDirection,
  fileName: string
): readonly string[] => {
  if (direction === 'send') {
    if (protocol === 'zmodem') {
      return ['/usr/bin/rz'];
    }
    if (protocol === 'ymodem') {
      return ['/usr/bin/rb'];
    }
    return ['/usr/bin/rx', fileName];
  }

  if (protocol === 'zmodem') {
    return ['/usr/bin/sz', fileName];
  }
  if (protocol === 'ymodem') {
    return ['/usr/bin/sb', fileName];
  }
  return ['/usr/bin/sx', fileName];
};

const writeLoginScript = async (
  path: string,
  remoteDirectory: string,
  markerPath: string,
  command: readonly string[]
): Promise<void> => {
  await writeFile(
    path,
    [
      '#!/bin/sh',
      `cd ${shellQuote(remoteDirectory)} || exit 1`,
      'stty raw -echo -ixon -ixoff -icanon min 1 time 0 2>/dev/null || true',
      `while [ ! -f ${shellQuote(markerPath)} ]; do sleep 0.02; done`,
      `exec ${command.map(shellQuote).join(' ')} 2>lrzsz.stderr`,
      '',
    ].join('\n'),
    'utf8'
  );
  await chmod(path, 0o755);
};

const writeTelnetConfig = async (
  path: string,
  port: number,
  transferBasePath: string
): Promise<void> => {
  await writeFile(
    path,
    [
      '[general]',
      'type=telnet',
      '',
      '[terminal]',
      'auto_close=false',
      '',
      '[telnet]',
      'address=127.0.0.1',
      `port=${port}`,
      '',
      '[transfer]',
      `base_path=${transferBasePath}`,
      '',
    ].join('\n'),
    'utf8'
  );
};

const createTransferFixture = async (
  directory: string,
  protocol: TransferProtocol,
  direction: TransferDirection,
  sizeCase: TransferSizeCase
): Promise<TransferFixture> => {
  const localDirectory = join(directory, 'local');
  const remoteDirectory = join(directory, 'remote');
  const receiveDirectory = join(directory, 'receive');
  await Promise.all([
    mkdir(localDirectory, { recursive: true }),
    mkdir(remoteDirectory, { recursive: true }),
    mkdir(receiveDirectory, { recursive: true }),
  ]);

  const payload = makeTransferPayload(sizeCase.byteLength);
  const configPath = join(directory, 'telnet.ini');
  const markerPath = join(directory, 'start-transfer.marker');
  const loginScriptPath = join(directory, 'login.sh');
  const port = await unusedLocalhostPort();

  if (direction === 'send') {
    const sourcePath = join(localDirectory, 'payload.bin');
    const remoteName = protocol === 'xmodem' ? 'received.bin' : 'payload.bin';
    const command = lrzszCommand(protocol, direction, remoteName);
    await writeFile(sourcePath, payload);
    await writeLoginScript(
      loginScriptPath,
      remoteDirectory,
      markerPath,
      command
    );
    await writeTelnetConfig(configPath, port, receiveDirectory);
    return {
      command,
      configPath,
      expectedPath: join(remoteDirectory, remoteName),
      markerPath,
      payload,
      sourceUri: pathToFileURL(sourcePath).href,
    };
  }

  const remoteName = 'remote.bin';
  const command = lrzszCommand(protocol, direction, remoteName);
  await writeFile(join(remoteDirectory, remoteName), payload);
  await writeLoginScript(loginScriptPath, remoteDirectory, markerPath, command);
  await writeTelnetConfig(configPath, port, receiveDirectory);
  return {
    command,
    configPath,
    expectedPath: join(
      receiveDirectory,
      protocol === 'xmodem' ? 'received.bin' : remoteName
    ),
    markerPath,
    payload,
    sourceUri: undefined,
  };
};

const startConnection = async (
  connectionCase: TransferConnectionCase,
  fixture: TransferFixture
): Promise<TransferConnection> => {
  const config = await readFile(fixture.configPath, 'utf8');
  const portLine = config
    .split(/\r?\n/u)
    .find((line) => line.startsWith('port='));
  if (portLine === undefined) {
    throw new Error('TELNET config did not contain a port.');
  }

  return connectionCase.start({
    configPath: fixture.configPath,
    loginScriptPath: join(fixture.configPath, '..', 'login.sh'),
    port: Number(portLine.slice('port='.length)),
  });
};

const expectTransferButtonSensitive = async (
  button: GtkToggleButtonElement
): Promise<void> => {
  await waitForResult(
    async () => {
      const info = await button.info();
      expect(info.states).toContain('enabled');
      expect(info.states).toContain('sensitive');
    },
    {
      message: 'transfer button should be sensitive',
      timeoutMs: 5_000,
    }
  );
};

const activateTransfer = async (
  app: GtkApp,
  protocol: TransferProtocol,
  direction: TransferDirection
): Promise<void> => {
  const transferButton = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  await expectTransferButtonSensitive(transferButton);
  await transferButton.click();

  const item = await waitForResult(
    async () => {
      const menuItem = expectElementKind(
        await app.getById(transferMenuItemId(protocol, direction)),
        'menuItem'
      );
      expect((await menuItem.info()).states).toContain('showing');
      return menuItem;
    },
    {
      message: `${protocol} ${direction} menu item should be visible`,
      timeoutMs: 5_000,
    }
  );
  await item.click();
};

const waitForFileBytes = async (
  path: string,
  expected: Buffer,
  protocol: TransferProtocol,
  timeoutMs: number
): Promise<void> => {
  await waitForResult(
    async () => {
      const actual = await readFile(path);
      const comparable =
        protocol === 'xmodem' ? actual.subarray(0, expected.length) : actual;
      expect(comparable.length).toBe(expected.length);
      expect(Buffer.compare(comparable, expected)).toBe(0);
    },
    {
      message: `transfer result should match ${path}`,
      timeoutMs,
    }
  );
};

describe('elder-terms-vte XYZMODEM transfer e2e', () => {
  for (const connectionCase of connectionCases) {
    for (const protocol of protocols) {
      for (const direction of directions) {
        for (const startDelayMs of startDelaysMs) {
          for (const sizeCase of transferSizeCasesByProtocol[protocol]) {
            it(
              `${connectionCase.name} ${protocol} ${direction} ${sizeCase.label} starts after ${startDelayMs}ms`,
              async (context) => {
                await expectRequiredCommands();
                await withTemporaryDirectory(async (directory) => {
                  const fixture = await createTransferFixture(
                    directory,
                    protocol,
                    direction,
                    sizeCase
                  );
                  const connection = await startConnection(
                    connectionCase,
                    fixture
                  );
                  try {
                    const args = [
                      '-c',
                      fixture.configPath,
                      ...(fixture.sourceUri === undefined
                        ? []
                        : [`--test-transfer-source-uri=${fixture.sourceUri}`]),
                    ];
                    await runGtkTest(context, args, async (app, evidence) => {
                      await evidence.log('XYZMODEM e2e case', {
                        command: fixture.command,
                        connection: connectionCase.name,
                        direction,
                        protocol,
                        size: sizeCase.label,
                        sizeBytes: sizeCase.byteLength,
                        startDelayMs,
                      });
                      await waitForActivityIndicatorImageState(
                        app,
                        'conn',
                        'on'
                      );

                      const startedAtMs = performance.now();
                      if (direction === 'send') {
                        await writeFile(fixture.markerPath, 'start', 'utf8');
                        await delay(startDelayMs);
                        await activateTransfer(app, protocol, direction);
                      } else {
                        await activateTransfer(app, protocol, direction);
                        await delay(startDelayMs);
                        await writeFile(fixture.markerPath, 'start', 'utf8');
                      }

                      await waitForFileBytes(
                        fixture.expectedPath,
                        fixture.payload,
                        protocol,
                        sizeCase.timeoutMs
                      );
                      const elapsedMs = performance.now() - startedAtMs;
                      const adjustedElapsedMs = Math.max(
                        0,
                        elapsedMs - startDelayMs
                      );
                      await evidence.log('XYZMODEM e2e result', {
                        adjustedElapsedMs,
                        elapsedMs,
                      });
                      if (protocol === 'xmodem' && direction === 'send') {
                        expect(adjustedElapsedMs).toBeLessThan(
                          maxXmodemSendAdjustedDurationMs
                        );
                      }
                    });
                  } finally {
                    await connection.close();
                  }
                });
              },
              sizeCase.timeoutMs + 30_000
            );
          }
        }
      }
    }
  }
});
