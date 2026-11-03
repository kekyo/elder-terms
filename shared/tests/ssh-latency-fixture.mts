// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

import { execFile, spawn } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { userInfo } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';

const exec = promisify(execFile);
const quote = (value: string): string => `'${value.split("'").join("'\\''")}'`;

/** Loopback-only SSH transport owned by one latency run. */
export interface SshLatencyFixture {
  /** PID of the detached listener, also used as the owned process group ID. */
  readonly serverPid: number;
  /** Ephemeral listening port on 127.0.0.1. */
  readonly port: number;
  /** Current local account used for the isolated test login. */
  readonly username: string;
  /** Private key generated solely for this fixture. */
  readonly clientKey: string;
  /** Host-key file generated from the fixture's host key. */
  readonly knownHosts: string;
  /** Terminates the fixture process group and saves bounded diagnostics. */
  stop(): Promise<void>;
}

/** Starts the peer through an isolated SSH server on the current PC.
 * @param output Private artifact directory for generated keys and configuration.
 * @param peerArgs Peer executable and arguments run as the forced command.
 * @returns The fixture; its owner must await stop in a finally block.
 */
export const startSshLatencyFixture = async (
  output: string,
  peerArgs: readonly string[]
): Promise<SshLatencyFixture> => {
  const directory = join(output, 'ssh-fixture');
  await mkdir(directory, { mode: 0o700 });
  const hostKey = join(directory, 'host');
  const clientKey = join(directory, 'client');
  const knownHosts = join(directory, 'known_hosts');
  const login = join(directory, 'login');
  const config = join(directory, 'sshd_config');
  const username = userInfo().username;
  await Promise.all(
    [hostKey, clientKey].map((path) =>
      exec('/usr/bin/ssh-keygen', ['-q', '-t', 'ed25519', '-N', '', '-f', path])
    )
  );
  await writeFile(
    join(directory, 'authorized_keys'),
    await readFile(`${clientKey}.pub`),
    { mode: 0o600 }
  );
  await writeFile(login, `#!/bin/sh\nexec ${peerArgs.map(quote).join(' ')}\n`, {
    mode: 0o700,
  });
  await writeFile(
    config,
    [
      `HostKey ${hostKey}`,
      'PidFile none',
      `AuthorizedKeysFile ${join(directory, 'authorized_keys')}`,
      'AuthenticationMethods publickey',
      'PubkeyAuthentication yes',
      'PasswordAuthentication no',
      'KbdInteractiveAuthentication no',
      'UsePAM no',
      'PermitRootLogin no',
      `AllowUsers ${username}`,
      'LogLevel VERBOSE',
      'PrintMotd no',
      'PrintLastLog no',
      'X11Forwarding no',
      'PermitTTY yes',
      'StrictModes no',
      'UseDNS no',
      `ForceCommand ${login}`,
      '',
    ].join('\n')
  );

  const server = spawn(
    '/usr/bin/socat',
    [
      '-d',
      '-d',
      'TCP-LISTEN:0,bind=127.0.0.1,reuseaddr,fork',
      `EXEC:/usr/sbin/sshd -i -e -f ${quote(config)}`,
    ],
    { detached: true, stdio: ['ignore', 'ignore', 'pipe'] }
  );
  let didClose = false;
  const closed = new Promise<void>((resolveClose) => {
    server.once('close', () => {
      didClose = true;
      resolveClose();
    });
  });
  let diagnostics = '';
  const signalGroup = (signal: NodeJS.Signals): void => {
    if (server.pid) {
      try {
        process.kill(-server.pid, signal);
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'ESRCH') throw error;
      }
    }
  };
  const groupExists = (): boolean => {
    if (!server.pid) return false;
    try {
      process.kill(-server.pid, 0);
      return true;
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === 'ESRCH') return false;
      throw error;
    }
  };
  const waitForClose = async (milliseconds: number): Promise<void> => {
    if (didClose) return;
    let timer: NodeJS.Timeout | undefined;
    try {
      await Promise.race([
        closed,
        new Promise<void>((resolveTimeout) => {
          timer = setTimeout(resolveTimeout, milliseconds);
        }),
      ]);
    } finally {
      if (timer) clearTimeout(timer);
    }
  };
  let stopping: Promise<void> | undefined;
  const stop = (): Promise<void> => {
    if (stopping === undefined)
      stopping = (async () => {
        signalGroup('SIGTERM');
        await waitForClose(1000);
        if (groupExists()) signalGroup('SIGKILL');
        await waitForClose(1000);
        if (!didClose) server.stderr?.destroy();
        await writeFile(join(directory, 'server.log'), diagnostics);
      })();
    return stopping;
  };
  try {
    const port = await new Promise<number>((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error(`SSH fixture did not listen: ${diagnostics}`)),
        10000
      );
      server.once('error', (error) => {
        clearTimeout(timeout);
        reject(error);
      });
      server.once('exit', (code) => {
        clearTimeout(timeout);
        reject(new Error(`SSH fixture exited ${code}: ${diagnostics}`));
      });
      server.stderr!.on('data', (bytes) => {
        diagnostics = (diagnostics + bytes.toString()).slice(-65536);
        const match = /listening on AF=2 127\.0\.0\.1:(\d+)/.exec(diagnostics);
        if (match) {
          clearTimeout(timeout);
          resolve(Number(match[1]));
        }
      });
    });
    await writeFile(
      knownHosts,
      `[127.0.0.1]:${port} ${(await readFile(`${hostKey}.pub`, 'utf8')).trim()}\n`,
      { mode: 0o600 }
    );
    return {
      serverPid: server.pid!,
      port,
      username,
      clientKey,
      knownHosts,
      stop,
    };
  } catch (error) {
    await stop();
    throw error;
  }
};
