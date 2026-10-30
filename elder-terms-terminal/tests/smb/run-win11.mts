import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { readFile, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { win32 } from 'node:path';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { connectRemoteAgent, type RemoteAgent } from 'agent-rover';
import { withSshGateway } from '../ssh-proxy-test-helper.mjs';

const probe = fileURLToPath(
  new URL('../../../.build/smb-probe/smb-probe', import.meta.url)
);
const terminalDirectory = fileURLToPath(new URL('../../', import.meta.url));
const fixtureSource = await readFile(
  new URL('./windows-fixture.ps1', import.meta.url)
);
const runId = randomBytes(12).toString('hex');
const password = `A${randomBytes(18).toString('hex')}a9!`;
const diagnostic = (text: string): string =>
  text.split(password).join('[redacted]').slice(0, 2000);

const remoteCommand = async (
  agent: RemoteAgent,
  script: string,
  action:
    | 'inspect'
    | 'setup'
    | 'verify'
    | 'ui-seed'
    | 'ui-verify'
    | 'ui-clear'
    | 'cleanup',
  clientAddress: string
): Promise<Record<string, unknown>> => {
  const remote = await agent.processes.launchManaged({
    path: 'powershell.exe',
    arguments: [
      '-NoProfile',
      '-NonInteractive',
      '-ExecutionPolicy',
      'Bypass',
      '-File',
      script,
      '-Action',
      action,
      '-RunId',
      runId,
      '-ClientAddress',
      clientAddress,
    ],
    environment: { ELDER_SMB_PASSWORD: password },
    captureStdout: true,
    captureStderr: true,
    createNoWindow: true,
  });
  try {
    let snapshot;
    try {
      snapshot = await remote.waitForExit({ timeoutMs: 60_000 });
    } catch {
      let stderr = '';
      try {
        stderr = diagnostic(await remote.stderrText());
      } catch {
        /* No captured output. */
      }
      throw new Error(`Windows fixture ${action} did not exit: ${stderr}`);
    }
    if (snapshot.root.exitCode !== 0) {
      const stderr = diagnostic(await remote.stderrText());
      const stdout = diagnostic(await remote.stdoutText());
      throw new Error(
        `Windows fixture ${action} failed with exit ${snapshot.root.exitCode}: ${stderr || stdout}`
      );
    }
    const output = (await remote.stdoutText()).trim();
    try {
      return output === ''
        ? {}
        : (JSON.parse(output) as Record<string, unknown>);
    } catch {
      throw new Error(
        `Windows fixture ${action} returned an invalid response: ${diagnostic(output)}`
      );
    }
  } finally {
    await remote.releaseAsync();
  }
};

const routeSource = (address: string): string | undefined => {
  try {
    const route = execFileSync('ip', ['-4', 'route', 'get', address], {
      encoding: 'utf8',
      timeout: 5000,
    });
    return route.match(/\bsrc (\d+\.\d+\.\d+\.\d+)\b/u)?.[1];
  } catch {
    return undefined;
  }
};

const probeShare = async (
  address: string,
  share: string,
  user: string,
  mode: string,
  expectedExit: number,
  expectedFailure: RegExp | undefined,
  wrongPassword: boolean
): Promise<void> => {
  const child = spawn(probe, [address, share, user, '445', mode], {
    env: {
      ...process.env,
      ELDER_SMB_PASSWORD: wrongPassword ? 'deliberately-incorrect' : password,
      ELDER_SMB_REPARSE_PATH: 'outside-link',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
    signal: AbortSignal.timeout(45_000),
  });
  let output = '';
  child.stdout.setEncoding('utf8').on('data', (text: string) => {
    output += text;
  });
  child.stderr.setEncoding('utf8').on('data', (text: string) => {
    output += text;
  });
  const exitCode = await new Promise<number | null>((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
  });
  assert.equal(
    exitCode,
    expectedExit,
    `${share}/${mode} returned an unexpected status: ${diagnostic(output)}`
  );
  if (expectedExit === 0) {
    assert.ok(
      /PASS list\/read\/write\/exclusive-create\/no-replace\/replace\/lock\/readonly\/mtime\/delete/u.test(
        output
      ),
      'Probe did not complete the expected file operations'
    );
    assert.ok(
      /PASS reparse attribute without traversal/u.test(output),
      'Windows junction did not expose its reparse attribute without traversal'
    );
  } else {
    assert.ok(
      expectedFailure?.test(output),
      'Probe did not fail for the expected SMB operation and reason'
    );
  }
  console.log(`PASS ${share}/${mode} (${expectedExit})`);
};

const transferShare = async (
  driver: string,
  address: string,
  share: string,
  user: string,
  readOnly: boolean
): Promise<void> => {
  const args = [address, share, user, '445'];
  if (readOnly) args.push('readonly');
  const child = spawn(driver, args, {
    env: { ...process.env, ELDER_SMB_PASSWORD: password },
    stdio: ['ignore', 'pipe', 'pipe'],
    signal: AbortSignal.timeout(180_000),
  });
  let output = '';
  child.stdout.setEncoding('utf8').on('data', (text: string) => {
    output += text;
  });
  child.stderr.setEncoding('utf8').on('data', (text: string) => {
    output += text;
  });
  const exitCode = await new Promise<number | null>((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
  });
  assert.equal(
    exitCode,
    0,
    `Windows SMB transfer driver failed: ${diagnostic(output)}`
  );
  assert.match(
    output,
    readOnly
      ? /PASS SMB client read-only access refusal/u
      : /PASS SMB client recursive upload\/download\/commit\/metadata\/cancellation\/delete/u
  );
  console.log(
    `PASS win11 SMB native transfer ${readOnly ? 'readonly' : share}`
  );
};

const agent = await connectRemoteAgent({ section: 'win11' });
let remoteDirectory: string | undefined;
let script: string | undefined;
let setupStarted = false;
let cleanupSucceeded = false;
let failure: unknown;
try {
  remoteDirectory = await agent.files.mkdtemp('elder-plan12-smb-');
  const uploadedScript = win32.join(remoteDirectory, 'windows-fixture.ps1');
  script = uploadedScript;
  await agent.files.writeFile(
    uploadedScript,
    Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), fixtureSource])
  );

  const inspected = await remoteCommand(agent, uploadedScript, 'inspect', '');
  assert.equal(
    inspected.administrator,
    true,
    'The win11 agent must be restarted as administrator before SMB verification'
  );
  const addresses = inspected.addresses;
  assert.ok(
    Array.isArray(addresses) &&
      addresses.every((address) => typeof address === 'string')
  );
  const preferred = process.env.ELDER_SMB_SERVER_IP;
  const candidates = preferred ? [preferred] : (addresses as string[]);
  if (preferred)
    assert.ok(
      addresses.includes(preferred),
      'ELDER_SMB_SERVER_IP is not an address of the win11 agent'
    );
  const target = candidates
    .map((address) => ({ address, client: routeSource(address) }))
    .find((candidate) => candidate.client !== undefined);
  assert.ok(target?.client, 'No route to a win11 IPv4 address was found');

  setupStarted = true;
  const fixture = await remoteCommand(
    agent,
    uploadedScript,
    'setup',
    target.client
  );
  const user = fixture.user;
  const plain = fixture.plainShare;
  const encrypted = fixture.encryptedShare;
  const readOnly = fixture.readOnlyShare;
  assert.ok(
    typeof user === 'string' &&
      typeof plain === 'string' &&
      typeof encrypted === 'string' &&
      typeof readOnly === 'string'
  );

  const serverDriver = process.env.ELDER_SMB_SERVER_DRIVER;
  if (process.env.ELDER_SMB_ENUM_DRIVER || serverDriver) {
    const directory = await mkdtemp(resolve(tmpdir(), 'elder-smb-enum-win-'));
    const check = async (extra: Record<string, string>): Promise<void> => {
      const child = spawn(
        serverDriver ?? process.env.ELDER_SMB_ENUM_DRIVER!,
        [target.address, plain, user, '445'],
        {
          env: {
            ...process.env,
            ELDER_SMB_PASSWORD: password,
            ELDER_SMB_EMPTY_SHARE: String(fixture.emptyShare),
            ELDER_SMB_UNICODE_SHARE: String(fixture.unicodeShare),
            ELDER_SMB_DENIED_SHARE: String(fixture.deniedShare),
            ELDER_SMB_EXPECT_SHARES: [
              plain,
              encrypted,
              readOnly,
              fixture.emptyShare,
              fixture.unicodeShare,
              fixture.deniedShare,
            ].join('\n'),
            ...extra,
          },
          stdio: ['ignore', 'pipe', 'pipe'],
          signal: AbortSignal.timeout(60000),
        }
      );
      let output = '';
      child.stdout.on('data', (bytes) => {
        output += bytes;
      });
      child.stderr.on('data', (bytes) => {
        output += bytes;
      });
      const [code] = await once(child, 'close');
      assert.equal(code, 0, diagnostic(output));
      assert.match(output, /PASS SMB client/);
      console.log(
        `PASS win11 ${serverDriver ? 'server browsing and transfer' : 'share enumeration'} ${extra.ELDER_SMB_PROXY_PORT ? 'SSH proxy' : 'direct'}: ${diagnostic(output).trim()}`
      );
    };
    try {
      await check({});
      await withSshGateway(
        resolve(
          terminalDirectory,
          '../.build/elder-terms-terminal/ssh-proxy-test-server'
        ),
        directory,
        async (gateway) => {
          await check({
            ELDER_SMB_PROXY_PORT: String(gateway.port),
            ELDER_SMB_PROXY_KNOWN_HOSTS: gateway.knownHosts,
          });
          assert.deepEqual(
            gateway.requests,
            Array.from({ length: serverDriver ? 5 : 1 }, () => ({
              host: 'proxy-test.invalid',
              port: 445,
            }))
          );
        },
        target.address
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  } else if (process.env.ELDER_SMB_SSH_PROXY_DRIVER) {
    const localDirectory = await mkdtemp(
      resolve(tmpdir(), 'elder-smb-win-proxy-')
    );
    try {
      for (const [share, mode] of [
        [plain, 'transfer'],
        [encrypted, 'encrypted'],
        [readOnly, 'readonly'],
      ]) {
        await withSshGateway(
          resolve(
            terminalDirectory,
            '../.build/elder-terms-terminal/ssh-proxy-test-server'
          ),
          localDirectory,
          async (gateway) => {
            const child = spawn(
              process.env.ELDER_SMB_SSH_PROXY_DRIVER!,
              [target.address, share, user, '445'],
              {
                env: {
                  ...process.env,
                  ELDER_SMB_PASSWORD: password,
                  ELDER_SMB_PROXY_PORT: String(gateway.port),
                  ELDER_SMB_PROXY_KNOWN_HOSTS: gateway.knownHosts,
                  ELDER_SMB_PROXY_MODE: mode,
                },
                stdio: ['ignore', 'pipe', 'pipe'],
                signal: AbortSignal.timeout(60_000),
              }
            );
            let output = '';
            child.stdout.on('data', (data) => {
              output += data;
            });
            child.stderr.on('data', (data) => {
              output += data;
            });
            const [code] = await once(child, 'close');
            assert.equal(code, 0, diagnostic(output));
            assert.match(output, /PASS SMB client SSH proxy/);
            assert.deepEqual(gateway.requests, [
              { host: 'proxy-test.invalid', port: 445 },
            ]);
          },
          target.address
        );
        console.log(`PASS win11 SMB SSH proxy ${mode}`);
      }
      const afterProxy = await remoteCommand(
        agent,
        uploadedScript,
        'verify',
        ''
      );
      assert.equal(afterProxy.sentinelIntact, true);
      assert.equal(afterProxy.sharedDirectoryClean, true);
    } finally {
      await rm(localDirectory, { recursive: true, force: true });
    }
  } else {
    await probeShare(target.address, plain, user, 'basic', 0, undefined, false);
    await probeShare(target.address, plain, user, 'smb2', 0, undefined, false);
    await probeShare(
      target.address,
      encrypted,
      user,
      'encrypt',
      0,
      undefined,
      false
    );
    await probeShare(
      target.address,
      encrypted,
      user,
      'encrypt2',
      1,
      /SMB connect failed:.*(?:remote closed connection|encryption|dialect|not supported|reset|STATUS_NOT_SUPPORTED)/iu,
      false
    );
    await probeShare(
      target.address,
      `missing-${runId}`,
      user,
      'basic',
      1,
      /SMB connect failed:.*STATUS_BAD_NETWORK_NAME/iu,
      false
    );
    await probeShare(
      target.address,
      plain,
      user,
      'basic',
      1,
      /SMB connect failed:.*STATUS_LOGON_FAILURE/iu,
      true
    );
    await probeShare(
      target.address,
      readOnly,
      user,
      'basic',
      1,
      /SMB mkdir failed: -13\b/u,
      false
    );
    const verified = await remoteCommand(agent, uploadedScript, 'verify', '');
    assert.equal(verified.sentinelIntact, true);
    assert.equal(verified.sharedDirectoryClean, true);
    if (process.env.ELDER_SMB_TRANSFER_DRIVER) {
      const driver = process.env.ELDER_SMB_TRANSFER_DRIVER;
      await transferShare(driver, target.address, plain, user, false);
      await transferShare(driver, target.address, encrypted, user, false);
      await transferShare(driver, target.address, readOnly, user, true);
      const afterTransfer = await remoteCommand(
        agent,
        uploadedScript,
        'verify',
        ''
      );
      assert.equal(afterTransfer.sentinelIntact, true);
      assert.equal(afterTransfer.sharedDirectoryClean, true);
    }
    const seeded = await remoteCommand(agent, uploadedScript, 'ui-seed', '');
    assert.equal(seeded.seeded, true);
    try {
      const child = spawn(
        'npx',
        ['vitest', 'run', 'tests/smb-window.test.ts', '--reporter=dot'],
        {
          cwd: terminalDirectory,
          env: {
            ...process.env,
            ELDER_TERMS_TEST_FTP_APP:
              process.env.ELDER_TERMS_TEST_FTP_APP ??
              resolve(
                terminalDirectory,
                '../.build/elder-terms-terminal/elder-terms-file-transfer'
              ),
            ELDER_SMB_UI_ADDRESS: target.address,
            ELDER_SMB_UI_PORT: '445',
            ELDER_SMB_UI_SHARE: plain,
            ELDER_SMB_UI_USER: user,
            ELDER_SMB_UI_PASSWORD: password,
            ELDER_SMB_UI_EMPTY_SHARE: String(fixture.emptyShare),
            ELDER_SMB_UI_UNICODE_SHARE: String(fixture.unicodeShare),
            ELDER_SMB_UI_DENIED_SHARE: String(fixture.deniedShare),
          },
          stdio: ['ignore', 'pipe', 'pipe'],
          signal: AbortSignal.timeout(180_000),
        }
      );
      let output = '';
      child.stdout.setEncoding('utf8').on('data', (text: string) => {
        output += text;
      });
      child.stderr.setEncoding('utf8').on('data', (text: string) => {
        output += text;
      });
      const [code] = await once(child, 'exit');
      assert.equal(
        code,
        0,
        `Windows SMB UI test failed: ${diagnostic(output)}`
      );
      const uiVerified = await remoteCommand(
        agent,
        uploadedScript,
        'ui-verify',
        ''
      );
      assert.equal(uiVerified.verified, true);
      console.log(
        'PASS win11 SMB UI bidirectional transfer and file management'
      );
    } finally {
      const cleared = await remoteCommand(
        agent,
        uploadedScript,
        'ui-clear',
        ''
      );
      assert.equal(cleared.cleared, true);
    }
    const afterUi = await remoteCommand(agent, uploadedScript, 'verify', '');
    assert.equal(afterUi.sentinelIntact, true);
    assert.equal(afterUi.sharedDirectoryClean, true);
    console.log('PASS win11 SMB probe and outside-junction sentinel');
  }
} catch (error) {
  failure = error;
} finally {
  try {
    if (setupStarted && script) {
      const result = await remoteCommand(agent, script, 'cleanup', '');
      assert.equal(result.cleaned, true);
    }
    cleanupSucceeded = true;
  } catch (error) {
    console.error(`Windows fixture cleanup needs manual review: run ${runId}`);
    failure =
      failure === undefined
        ? error
        : new Error(
            `SMB verification and fixture cleanup both failed: ${String(failure)}; ${String(error)}`
          );
  } finally {
    try {
      if (cleanupSucceeded && remoteDirectory) {
        await agent.files.remove(remoteDirectory, { recursive: true });
      }
    } catch (error) {
      failure =
        failure === undefined
          ? error
          : new Error(
              `SMB verification and remote script cleanup both failed: ${String(failure)}; ${String(error)}`
            );
    } finally {
      agent.release();
    }
  }
}
if (failure !== undefined) throw failure;
console.log('PASS win11 fixture cleanup and agent release');
