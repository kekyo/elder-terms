// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { createServer, type Socket } from 'node:net';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir, userInfo } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createRequire } from 'node:module';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { expect, it } from 'vitest';

const execFileAsync = promisify(execFile);
const { PNG } = createRequire(import.meta.url)(
  'pngjs'
) as typeof import('pngjs');
const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);
const quote = (text: string): string => `'${text.split("'").join("'\\''")}'`;

it.each(['known', 'unknown', 'proxy', 'unknown-context-loss'])(
  'runs an SSH shell (%s), shared SFTP, remote resizes and reconnection',
  async (trust) => {
    const unknown = trust.startsWith('unknown');
    const contextLoss = trust === 'unknown-context-loss';
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-ssh-'));
    const hostKey = join(directory, 'host');
    const clientKey = join(directory, 'client');
    const knownHosts = join(directory, 'known_hosts');
    const config = join(directory, 'sshd_config');
    const login = join(directory, 'login');
    const size = join(directory, 'size');
    const reply = join(directory, 'reply');
    const sftpPath = join(directory, 'shared.txt');
    const sftpText = 'SHARED_SFTP:日本語\n';
    const username = userInfo().username;
    let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
    let server: ReturnType<typeof spawn> | undefined;
    let completion: Promise<unknown> | undefined;
    try {
      await Promise.all(
        [hostKey, clientKey].map(async (path) => {
          await execFileAsync('/usr/bin/ssh-keygen', [
            '-q',
            '-t',
            'ed25519',
            '-N',
            '',
            '-f',
            path,
          ]);
        })
      );
      await writeFile(
        join(directory, 'authorized_keys'),
        await readFile(`${clientKey}.pub`),
        { mode: 0o600 }
      );
      await writeFile(sftpPath, sftpText);
      await writeFile(
        login,
        `#!/bin/sh
case "$SSH_ORIGINAL_COMMAND" in
  /usr/lib/openssh/sftp-server*) exec /usr/lib/openssh/sftp-server ;;
esac
stty raw -echo
printf '\\033[6n'
dd bs=1 count=6 iflag=fullblock of=${quote(reply)} 2>/dev/null
stty cooked -echo
printf 'SSH_READY:日本語\\r\\n'
while IFS= read -r line; do
  stty size > ${quote(size)}
  printf 'RECEIVED:%s\\r\\n' "$line"
  [ "$line" = quit ] && break
done
printf 'SSH_EOF\\342\\202'
`,
        { mode: 0o700 }
      );
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
          'AllowTcpForwarding yes',
          'Subsystem sftp /usr/lib/openssh/sftp-server',
          'X11Forwarding no',
          'PermitTTY yes',
          'StrictModes no',
          'UseDNS no',
          `ForceCommand ${login}`,
          '',
        ].join('\n')
      );
      server = spawn(
        '/usr/bin/socat',
        [
          '-d',
          '-d',
          'TCP-LISTEN:0,bind=127.0.0.1,reuseaddr,fork',
          `EXEC:/usr/sbin/sshd -i -e -f ${quote(config)}`,
        ],
        { detached: true, stdio: ['ignore', 'ignore', 'pipe'] }
      );
      completion = once(server, 'close');
      let diagnostics = '';
      const port = await new Promise<number>((resolve, reject) => {
        server!.once('error', reject);
        server!.once('exit', (code) =>
          reject(new Error(`SSH fixture exited: ${code}: ${diagnostics}`))
        );
        server!.stderr!.on('data', (chunk: Buffer) => {
          diagnostics += chunk.toString();
          const match = /listening on AF=2 127\.0\.0\.1:(\d+)/.exec(
            diagnostics
          );
          if (match) resolve(Number(match[1]));
        });
      });
      if (!unknown)
        await writeFile(
          knownHosts,
          `[127.0.0.1]:${port} ${(await readFile(`${hostKey}.pub`, 'utf8')).trim()}\n`
        );
      launcher = createGtkAppLauncher({
        appPath: driverPath,
        args: [
          ...(trust === 'known' ? ['--zoom-modes'] : []),
          ...(contextLoss ? ['--context-loss'] : []),
          '--ssh',
          '127.0.0.1',
          String(port),
          username,
          '--identity',
          clientKey,
          '--known-hosts',
          knownHosts,
          '--ssh-config',
          '/dev/null',
          '--sftp-file',
          sftpPath,
          ...(trust === 'proxy'
            ? [
                '--proxy',
                '127.0.0.1',
                String(port),
                username,
                '--proxy-identity',
                clientKey,
              ]
            : []),
        ],
        env: { LIBGL_ALWAYS_SOFTWARE: '1' },
      });
      const app = await launcher.launch();
      const acceptUsername = async (): Promise<void> => {
        const entry = await app.getById('driver_ssh_prompt_entry');
        const accept = await app.getById('driver_ssh_prompt_accept_button');
        if (entry.kind !== 'entry' || accept.kind !== 'button')
          throw new Error('Missing SSH prompt');
        await toPass(async () =>
          expect((await entry.info()).states).toContain('showing')
        );
        expect(await entry.text()).toBe(username);
        await accept.click();
      };
      await acceptUsername();
      if (trust === 'proxy') await acceptUsername();
      if (unknown) {
        const message = await app.getById(
          'driver_ssh_prompt_monospace_message_label'
        );
        const accept = await app.getById('driver_ssh_prompt_accept_button');
        if (message.kind !== 'label' || accept.kind !== 'button')
          throw new Error('Missing host-key prompt');
        await toPass(async () => {
          expect((await message.info()).states).toContain('showing');
          expect(await message.text()).toContain('SHA256');
        });
        if (contextLoss) {
          const status = await app.getById('driver_status');
          const loseContext = await app.getById('driver_lose_gl_context');
          if (status.kind !== 'label' || loseContext.kind !== 'button')
            throw new Error('Context-loss controls are missing');
          await toPass(async () =>
            expect(await status.text()).toContain('backend=opengl')
          );
          const before = await message.capture();
          await loseContext.click();
          await toPass(async () => {
            expect(await status.text()).toContain('backend=cairo');
            expect(await status.text()).toContain('reason=3');
            expect((await message.info()).states).toContain('showing');
            expect(await message.text()).toContain('SHA256');
          });
          const evidence = fileURLToPath(
            new URL(
              '../../test-results/boxrig/context-loss-overlay/',
              import.meta.url
            )
          );
          await mkdir(evidence, { recursive: true });
          await writeFile(join(evidence, 'before.png'), before.image);
          const after = await message.capture();
          await writeFile(join(evidence, 'after.png'), after.image);
          const beforePixels = PNG.sync.read(before.image);
          const afterPixels = PNG.sync.read(after.image);
          expect([afterPixels.width, afterPixels.height]).toEqual([
            beforePixels.width,
            beforePixels.height,
          ]);
          let maximumDifference = 0;
          for (let index = 0; index < beforePixels.data.length; index++)
            maximumDifference = Math.max(
              maximumDifference,
              Math.abs(afterPixels.data[index] - beforePixels.data[index])
            );
          // GL and Cairo compositing can round the same color by one level.
          expect(maximumDifference).toBeLessThanOrEqual(2);
        }
        await accept.click();
      }
      const visible = async (): Promise<string> => {
        const line =
          (await app.output()).stdout
            .split('\n')
            .filter((value) => value.includes(' text='))
            .pop() ?? '';
        return Buffer.from(
          / text=(.*)/.exec(line)?.[1] ?? '',
          'base64'
        ).toString();
      };
      await toPass(async () => {
        expect(
          await visible(),
          `${diagnostics}\n${JSON.stringify(await app.output())}`
        ).toContain('SSH_READY:日本語');
        expect(await readFile(reply, 'utf8')).toBe('\x1b[1;1R');
      });
      const sendBreak = await app.getById('driver_break');
      if (sendBreak.kind !== 'button') throw new Error('Missing BREAK control');
      await sendBreak.click();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain('break_finished=1')
      );
      const sftp = await app.getById('driver_sftp');
      if (sftp.kind !== 'button') throw new Error('Missing SFTP control');
      await sftp.click();
      await toPass(async () => {
        const output = await app.output();
        expect(output.stdout, output.stderr).toContain('sftp_finished=1');
        const data = [...output.stdout.matchAll(/^sftp_data=(.*)$/gm)].map(
          (match) => Buffer.from(match[1], 'base64')
        );
        expect(Buffer.concat(data).toString()).toBe(sftpText);
        expect(diagnostics.match(/Accepted publickey for/g)).toHaveLength(
          trust === 'proxy' ? 2 : 1
        );
      });
      const window = await app.getById('driver_window');
      const status = await app.getById('driver_status');
      if (window.kind !== 'window' || status.kind !== 'label')
        throw new Error('Missing driver window');
      await window.activate();
      const terminalBounds = (
        await (await app.getById('driver_terminal')).capture()
      ).bounds;
      await app.input.moveMouseTo(terminalBounds.x + 10, terminalBounds.y + 10);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      const bounds = await window.bounds();
      for (const offset of [100, 160]) {
        const before = await status.text();
        await window.resizeTo(
          bounds.width + offset,
          bounds.height + offset / 2
        );
        await toPass(async () => expect(await status.text()).not.toBe(before));
        await app.input.pressKey('r');
        await app.input.pressKey('Return');
        await toPass(async () => {
          const grid = /grid=(\d+)x(\d+)/.exec(await status.text());
          expect((await readFile(size, 'utf8')).trim()).toBe(
            `${grid?.[2]} ${grid?.[1]}`
          );
        });
      }
      if (trust === 'known') {
        const mode = await app.getById('driver_preserve_window');
        if (mode.kind !== 'checkbox')
          throw new Error('Missing window zoom mode');
        await mode.toggle();
        const fixedBounds = await window.bounds();
        for (const id of ['driver_zoom_in', 'driver_zoom_out']) {
          const before = await status.text();
          const zoom = await app.getById(id);
          if (zoom.kind !== 'button') throw new Error('Missing zoom button');
          await zoom.click();
          await toPass(async () => {
            expect(await status.text()).not.toBe(before);
            expect(await window.bounds()).toEqual(fixedBounds);
            expect(
              (await app.output()).stdout
                .trim()
                .split('\n')
                .filter((line) => line.startsWith('backend='))
                .pop()
            ).toContain('ready=1');
          });
          await app.input.pressKey('r');
          await app.input.pressKey('Return');
          await toPass(async () => {
            const grid = /grid=(\d+)x(\d+)/.exec(await status.text());
            expect((await readFile(size, 'utf8')).trim(), await visible()).toBe(
              `${grid?.[2]} ${grid?.[1]}`
            );
            expect(await window.bounds()).toEqual(fixedBounds);
          });
        }
      }
      for (const key of ['q', 'u', 'i', 't', 'Return'])
        await app.input.pressKey(key);
      await toPass(async () => {
        expect((await app.output()).stdout).toMatch(
          /connection=closed generation=1 .*error=0/
        );
        expect(await visible()).toContain('SSH_EOF�');
      });
      await sftp.click();
      await toPass(async () => {
        const output = await app.output();
        expect(
          output.stdout.match(/sftp_finished=1/g),
          output.stderr
        ).toHaveLength(2);
        const data = [...output.stdout.matchAll(/^sftp_data=(.*)$/gm)].map(
          (match) => Buffer.from(match[1], 'base64')
        );
        expect(Buffer.concat(data).toString()).toBe(sftpText.repeat(2));
        expect(diagnostics.match(/Accepted publickey for/g)).toHaveLength(
          trust === 'proxy' ? 2 : 1
        );
      });
      const reconnect = await app.getById('driver_reconnect');
      if (reconnect.kind !== 'button') throw new Error('Missing reconnect');
      await reconnect.click();
      await acceptUsername();
      if (trust === 'proxy') await acceptUsername();
      await toPass(async () => {
        expect((await app.output()).stdout).toContain(
          'connection=connected generation=2'
        );
        expect((await visible()).match(/SSH_READY:日本語/g)).toHaveLength(2);
      });
      if (trust === 'proxy') {
        const sockets: Socket[] = [];
        const received: Buffer[] = [];
        const target = createServer((socket) => {
          sockets.push(socket);
          socket.on('data', (bytes: Buffer) =>
            received.push(Buffer.from(bytes))
          );
          socket.write('\x1b[6nTELNET_PROXY_READY');
        });
        target.listen(0, '127.0.0.1');
        await once(target, 'listening');
        const targetAddress = target.address();
        if (!targetAddress || typeof targetAddress === 'string')
          throw new Error('Missing TELNET fixture port');
        const authenticatedBefore =
          diagnostics.match(/Accepted publickey for/g)?.length ?? 0;
        const telnetLauncher = createGtkAppLauncher({
          appPath: driverPath,
          args: [
            '--telnet',
            '127.0.0.1',
            String(targetAddress.port),
            '--proxy',
            '127.0.0.1',
            String(port),
            username,
            '--proxy-identity',
            clientKey,
            '--known-hosts',
            knownHosts,
            '--ssh-config',
            '/dev/null',
          ],
        });
        try {
          const telnetApp = await telnetLauncher.launch();
          const entry = await telnetApp.getById('driver_ssh_prompt_entry');
          const accept = await telnetApp.getById(
            'driver_ssh_prompt_accept_button'
          );
          if (entry.kind !== 'entry' || accept.kind !== 'button')
            throw new Error('Missing proxy prompt');
          await toPass(async () =>
            expect((await entry.info()).states).toContain('showing')
          );
          await accept.click();
          await toPass(async () => {
            const output = await telnetApp.output();
            const cooked = Buffer.concat(
              [...output.stdout.matchAll(/^cooked=(.*)$/gm)].map((match) =>
                Buffer.from(match[1], 'base64')
              )
            ).toString();
            expect(cooked, output.stderr).toContain('TELNET_PROXY_READY');
            expect(
              Buffer.concat(received).includes(Buffer.from('\x1b[1;1R'))
            ).toBe(true);
            expect(diagnostics.match(/Accepted publickey for/g)).toHaveLength(
              authenticatedBefore + 1
            );
          });
        } finally {
          await telnetLauncher.release();
          for (const socket of sockets) socket.destroy();
          const closing = once(target, 'close');
          target.close();
          await closing;
        }
      }
    } finally {
      await launcher?.release();
      if (server?.pid && server.exitCode === null && server.signalCode === null)
        process.kill(-server.pid, 'SIGTERM');
      if (completion) await completion;
      await rm(directory, { recursive: true, force: true });
    }
  }
);
