import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { readFile } from 'node:fs/promises';
import { resolve, win32 } from 'node:path';
import { fileURLToPath } from 'node:url';
import { connectRemoteAgent } from 'agent-rover';

const root = fileURLToPath(new URL('../../../', import.meta.url));
const runId = randomBytes(5).toString('hex');
const password = `A${randomBytes(18).toString('hex')}z9!`;
const redact = (text: string): string =>
  text.split(password).join('[redacted]');
const fixture = await readFile(
  resolve(root, '.build/smb1-probe/windows-xp-fixture.exe')
);
const agent = await connectRemoteAgent({ section: 'winxp' });
let directory: string | undefined;
let setupStarted = false;
let cleaned = false;

const remote = async (path: string, args: string[]): Promise<string> => {
  const child = await agent.processes.launchManaged({
    path,
    arguments: args,
    environment: { ELDER_SMB_PASSWORD: password },
    captureStdout: true,
    captureStderr: true,
    createNoWindow: true,
  });
  try {
    const result = await child.waitForExit({ timeoutMs: 60_000 });
    const stdout = await child.stdoutText();
    const stderr = await child.stderrText();
    assert.equal(
      result.root.exitCode,
      0,
      redact(`${path}: ${stdout}\n${stderr}`)
    );
    return stdout.trim();
  } finally {
    await child.releaseAsync();
  }
};

const local = async (
  path: string,
  args: string[],
  env: Record<string, string>,
  timeout: number
): Promise<string> => {
  const child = spawn(path, args, {
    cwd: resolve(root, 'elder-terms-file-transfer'),
    env: { ...process.env, ...env },
    stdio: ['ignore', 'pipe', 'pipe'],
    signal: AbortSignal.timeout(timeout),
  });
  let output = '';
  const collect = (chunk: Buffer): void => {
    output += chunk.toString();
  };
  child.stdout.on('data', collect);
  child.stderr.on('data', collect);
  const [code] = await once(child, 'close');
  console.log(redact(output).trim());
  assert.equal(code, 0, 'Windows XP SMB1 test failed');
  return output;
};

try {
  const version = await remote('cmd.exe', ['/d', '/c', 'ver']);
  assert.match(version, /Windows XP.*5\.1\.2600/);
  console.log(version);
  const netbios = await remote('hostname.exe', []);
  const network = await remote('ipconfig.exe', []);
  const addresses = Array.from(
    network.matchAll(/IP Address[^:\r\n]*:\s*(\d+\.\d+\.\d+\.\d+)/g),
    (match) => match[1]
  );
  const address = process.env.ELDER_SMB_SERVER_IP ?? addresses[0];
  assert.ok(
    address && addresses.includes(address),
    'No matching Windows XP IPv4 address'
  );
  console.log(`Windows XP SMB1 server: ${netbios} (${address})`);
  const workingDirectory = await remote('cmd.exe', ['/d', '/c', 'cd']);
  directory = win32.resolve(
    workingDirectory,
    await agent.files.mkdtemp('elder-plan18-winxp-')
  );
  const executable = win32.join(directory, 'fixture.exe');
  await agent.files.writeFile(executable, fixture);
  setupStarted = true;
  const setup = JSON.parse(
    await remote(executable, ['setup', directory, runId])
  );
  assert.equal(setup.setup, true);
  console.log(`Fixture ready; original forceguest=${setup.previousForceGuest}`);
  for (const name of ['before.txt', '日本語😀.txt', 'after.txt']) {
    await agent.files.writeFile(
      win32.join(directory, 'names', name),
      Buffer.from(`SMB1 ${name}\n`)
    );
  }
  const env = {
    LC_ALL: 'C',
    ELDER_SMB_PASSWORD: password,
    ELDER_SMB_USER: `eu${runId}`,
    ELDER_SMB_NETBIOS_NAME: netbios,
    ELDER_SMB_SHARE: `ep${runId}`,
    ELDER_SMB_EMPTY_SHARE: `ee${runId}`,
    ELDER_SMB_NAMES_SHARE: `en${runId}`,
    ELDER_SMB_UNICODE_SHARE: `日本語 ${runId}`,
    ELDER_SMB_DENIED_SHARE: `ed${runId}`,
    ELDER_SMB_READONLY_SHARE: `er${runId}`,
    ELDER_SMB_WINDOWS: '1',
  };
  for (const transport of ['TCP', 'NBT']) {
    const output = await local(
      resolve(root, '.build/elder-terms-file-transfer/smb1-client-test'),
      [address, transport],
      env,
      180_000
    );
    assert.match(output, /PASS SMB1 client/);
    assert.deepEqual(
      await agent.files.readdir(win32.join(directory, 'share')),
      []
    );
    assert.deepEqual(
      await agent.files.readdir(win32.join(directory, 'empty')),
      []
    );
    console.log(
      `PASS Windows XP SMB1 ${transport}: native transfer, permissions, reconnect and worker cleanup`
    );
  }
  for (const port of ['445', '139']) {
    const share = win32.join(directory, 'share');
    await agent.files.writeFile(
      win32.join(share, 'single.txt'),
      Buffer.from('single SMB file\n')
    );
    await agent.files.mkdir(win32.join(share, 'folder'));
    await agent.files.writeFile(
      win32.join(share, 'folder', 'nested.txt'),
      Buffer.from('nested SMB file\n')
    );
    await local(
      process.execPath,
      [
        resolve(root, 'node_modules/vitest/vitest.mjs'),
        'run',
        'tests/smb-window.test.ts',
        '-t',
        'opens SMB 1',
      ],
      {
        ...env,
        ELDER_SMB_UI_ADDRESS: address,
        ELDER_SMB_UI_PORT: port,
        ELDER_SMB_UI_NETBIOS_NAME: netbios,
        ELDER_SMB_UI_DOMAIN: netbios,
        ELDER_SMB_UI_USER: env.ELDER_SMB_USER,
        ELDER_SMB_UI_PASSWORD: password,
        ELDER_SMB_UI_SHARE: env.ELDER_SMB_SHARE,
        ELDER_SMB_UI_EMPTY_SHARE: env.ELDER_SMB_EMPTY_SHARE,
        ELDER_SMB_UI_UNICODE_SHARE: env.ELDER_SMB_UNICODE_SHARE,
        ELDER_SMB_UI_DENIED_SHARE: env.ELDER_SMB_DENIED_SHARE,
      },
      240_000
    );
    for (const [path, expected] of [
      ['kept.txt', 'replacement upload\n'],
      ['送信😀\\日本語\\内容.txt', 'recursive SMB1 upload\n'],
      ['single.txt', 'single SMB file\n'],
      ['folder\\nested.txt', 'nested SMB file\n'],
    ]) {
      assert.equal(
        (await agent.files.readFile(win32.join(share, path))).toString('utf8'),
        expected
      );
    }
    assert.deepEqual(
      (await agent.files.readdir(share)).map((entry) => entry.name).sort(),
      ['folder', 'kept.txt', 'single.txt', '送信😀'].sort()
    );
    console.log(
      `PASS Windows XP SMB1 port ${port}: GUI transfer, management, reconnect and independent file verification`
    );
    for (const entry of await agent.files.readdir(share)) {
      await agent.files.remove(win32.join(share, entry.name), {
        recursive: true,
      });
    }
  }
} finally {
  try {
    if (directory && setupStarted) {
      const result = JSON.parse(
        await remote(win32.join(directory, 'fixture.exe'), [
          'cleanup',
          directory,
          runId,
        ])
      );
      assert.equal(result.cleaned, true);
    }
    cleaned = true;
  } finally {
    try {
      if (directory && cleaned) {
        await agent.files.remove(directory, { recursive: true });
        assert.equal(await agent.files.exists(directory), false);
      } else if (directory) {
        console.error(
          `Fixture cleanup requires attention: ${directory} (${runId})`
        );
      }
    } finally {
      agent.release();
      console.log(
        `Windows XP agent released; fixture cleanup ${cleaned ? 'completed' : 'failed'}`
      );
    }
  }
}
