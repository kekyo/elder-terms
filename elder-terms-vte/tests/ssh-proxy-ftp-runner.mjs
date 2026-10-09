import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withSshGateway } from './ssh-proxy-test-helper.mjs';

const run = async (command, args, env) => {
  const child = spawn(command, args, {
    env: { ...process.env, ...env },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  child.stdout.on('data', (data) => {
    output += data;
  });
  child.stderr.on('data', (data) => {
    output += data;
  });
  const [code] = await once(child, 'close');
  assert.equal(code, 0, output);
  return output;
};

const root = await mkdtemp(join(tmpdir(), 'elder-ssh-ftp-'));
try {
  const cert = join(root, 'cert.pem');
  const key = join(root, 'key.pem');
  const wrong = join(root, 'wrong.pem');
  const wrongKey = join(root, 'wrong.key');
  const alternate = join(root, 'alternate.pem');
  const alternateKey = join(root, 'alternate.key');
  for (const [certificate, privateKey, host] of [
    [cert, key, 'proxy-test.invalid'],
    [wrong, wrongKey, 'wrong.invalid'],
    [alternate, alternateKey, 'proxy-test.invalid'],
  ]) {
    await run(
      'openssl',
      [
        'req',
        '-x509',
        '-newkey',
        'rsa:2048',
        '-noenc',
        '-keyout',
        privateKey,
        '-out',
        certificate,
        '-days',
        '1',
        '-subj',
        '/CN=' + host,
        '-addext',
        'subjectAltName=DNS:' + host + ',IP:::1',
      ],
      {}
    );
  }
  const cases = [];
  for (const mode of ['none', 'explicit', 'implicit']) {
    for (const legacy of [false, true])
      cases.push({
        name: mode + (legacy ? '-pasv' : '-epsv'),
        mode,
        flags: legacy ? ['--legacy-data'] : [],
      });
    cases.push({
      name: mode + '-ipv6',
      mode,
      address: '::1',
      flags: ['--ipv6'],
    });
  }
  cases.push({ name: 'none-ipv4', mode: 'none', address: '127.0.0.1' });
  for (const mode of ['explicit', 'implicit']) {
    for (const version of [771, 772])
      cases.push({
        name: mode + '-reuse-' + version,
        mode,
        flags: ['--require-reuse', '--tls-version=' + version],
      });
    cases.push({
      name: mode + '-wrong-name',
      mode,
      cert: wrong,
      key: wrongKey,
      ca: wrong,
      result: 'failure',
    });
    cases.push({ name: mode + '-untrusted', mode, ca: '', result: 'failure' });
    cases.push({
      name: mode + '-approve-data',
      mode,
      result: 'approve-data',
      settings: 'certificate_error_action=prompt\n',
      flags: ['--data-cert=' + alternate, '--data-key=' + alternateKey],
    });
  }
  for (const scenario of cases) {
    const directory = join(root, scenario.name);
    await mkdir(join(directory, 'home'), { recursive: true });
    const server = spawn(
      process.argv[3],
      [
        directory,
        '--trace',
        ...(scenario.mode === 'none' ? [] : ['--tls=' + scenario.mode]),
        '--cert=' + (scenario.cert ?? cert),
        '--key=' + (scenario.key ?? key),
        ...(scenario.flags ?? []),
      ],
      { stdio: ['pipe', 'pipe', 'pipe'] }
    );
    const completion = once(server, 'close');
    let trace = '';
    server.stderr.on('data', (data) => {
      trace += data;
    });
    try {
      const [ready] = await Promise.race([
        once(server.stdout, 'data'),
        (async () => {
          const [code] = await completion;
          throw new Error(
            'FTP fixture exited before readiness: ' + code + ' ' + trace
          );
        })(),
      ]);
      const port = Number(/^READY (\d+)/.exec(String(ready))?.[1]);
      assert.ok(port, String(ready));
      const ini = join(directory, 'connection.ini');
      const host = scenario.address ?? 'proxy-test.invalid';
      await writeFile(
        ini,
        `[ftp]\naddress=${host}\nport=${port}\nusername=alice\nlocal_directory=${directory}\ntls_mode=${scenario.mode}\nca_file=${scenario.ca ?? cert}\n${scenario.settings ?? ''}`
      );
      await withSshGateway(process.argv[4], root, async (gateway) => {
        const output = await run(
          process.argv[2],
          [ini, scenario.result ?? 'success'],
          {
            ELDER_TERMS_TEST_PROXY_PORT: String(gateway.port),
            ELDER_TERMS_TEST_PROXY_KNOWN_HOSTS: gateway.knownHosts,
            NO_PROXY: '*',
            no_proxy: '*',
          }
        );
        assert.ok(
          gateway.requests.every((request) => request.host === host),
          'Control and data must preserve the configured hostname'
        );
        assert.ok(
          !trace.includes('COMMAND EPRT') && !trace.includes('COMMAND PORT'),
          trace
        );
        if (scenario.result === 'failure') {
          assert.ok(output.includes('curl 60'), output);
          assert.ok(
            !trace.includes('COMMAND USER'),
            'Invalid TLS identity must prevent login'
          );
        } else {
          assert.ok(
            gateway.requests.some((request) => request.port === port),
            'Control connection must traverse SSH'
          );
          assert.ok(
            gateway.requests.filter((request) => request.port !== port)
              .length >= 4,
            'Listing, upload and download data connections must traverse SSH'
          );
          assert.ok(
            trace.includes('COMMAND STOR') && trace.includes('COMMAND RETR'),
            trace
          );
          assert.ok(
            trace.includes(
              scenario.flags?.includes('--legacy-data')
                ? 'COMMAND PASV'
                : 'COMMAND EPSV'
            ),
            trace
          );
          if (scenario.mode !== 'none')
            assert.ok(
              trace.includes('TLS CONTROL') && trace.includes('TLS DATA'),
              trace
            );
          if (scenario.result === 'approve-data') {
            assert.equal(
              trace.split('\n').filter((line) => line === 'COMMAND STOR probe')
                .length,
              2,
              'Certificate approval must wait for explicit upload retry'
            );
          }
        }
      });
      console.log('PASS SSH proxy FTP ' + scenario.name);
    } catch (error) {
      console.error(trace);
      throw error;
    } finally {
      server.kill();
      await completion;
    }
  }
} finally {
  await rm(root, { recursive: true, force: true });
}
