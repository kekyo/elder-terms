import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const run = async (command, args) => {
  const child = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', (data) => {
    output += data;
  });
  child.stderr.on('data', (data) => {
    output += data;
  });
  const [code] = await once(child, 'exit');
  return { code, output };
};
const root = await mkdtemp(join(tmpdir(), 'elder-ftps-'));
let failures = 0;
try {
  const cert = join(root, 'cert.pem');
  const key = join(root, 'key.pem');
  const generated = await run('openssl', [
    'req',
    '-x509',
    '-newkey',
    'rsa:2048',
    '-noenc',
    '-keyout',
    key,
    '-out',
    cert,
    '-days',
    '1',
    '-subj',
    '/CN=localhost',
    '-addext',
    'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1',
  ]);
  assert.equal(generated.code, 0, generated.output);
  const wrong = join(root, 'wrong.pem');
  const wrongKey = join(root, 'wrong-key.pem');
  const wrongResult = await run('openssl', [
    'req',
    '-x509',
    '-newkey',
    'rsa:2048',
    '-noenc',
    '-keyout',
    wrongKey,
    '-out',
    wrong,
    '-days',
    '1',
    '-subj',
    '/CN=wrong.invalid',
    '-addext',
    'subjectAltName=DNS:wrong.invalid',
  ]);
  assert.equal(wrongResult.code, 0, wrongResult.output);
  const expired = join(root, 'expired.pem');
  const expiredResult = await run('openssl', [
    'x509',
    '-in',
    cert,
    '-signkey',
    key,
    '-days',
    '-1',
    '-out',
    expired,
  ]);
  assert.equal(expiredResult.code, 0, expiredResult.output);
  const cases = [
    { name: 'explicit', success: true },
    {
      name: 'wrong-host',
      cert: wrong,
      key: wrongKey,
      ca: wrong,
      noLogin: true,
    },
    { name: 'expired', cert: expired, ca: expired, noLogin: true },
    { name: 'untrusted', ca: '', noLogin: true },
    { name: 'bad-ca', ca: join(root, 'absent.pem'), noLogin: true },
    { name: 'auth-refused', flags: ['--reject-auth'], noLogin: true },
    { name: 'protection-refused', flags: ['--reject-protection'] },
    { name: 'data-tls-failed', flags: ['--break-data-tls'] },
    { name: 'invalid-mode', mode: 'explict', noLogin: true },
  ];
  for (const mode of ['explicit', 'implicit']) {
    for (const active of [false, true]) {
      for (const ipv6 of [false, true]) {
        cases.push({
          name:
            mode +
            (active ? '-active' : '-passive') +
            (ipv6 ? '-ipv6' : '-ipv4'),
          mode,
          active,
          ipv6,
          success: true,
          flags: ipv6 ? ['--ipv6'] : [],
        });
      }
      cases.push({
        name: mode + '-legacy-' + active,
        mode,
        active,
        success: true,
        flags: ['--legacy-data'],
      });
    }
    for (const version of [771, 772]) {
      cases.push({
        name: mode + '-reuse-' + version,
        mode,
        success: true,
        flags: ['--require-reuse', '--tls-version=' + version],
      });
    }
    cases.push({
      name: mode + '-old-tls-rejected',
      mode,
      noLogin: true,
      flags: ['--tls-version=770'],
    });
  }
  for (const mode of ['explicit', 'implicit'])
    for (const listing of ['unix', 'dos']) {
      cases.push({
        name: mode + '-listing-' + listing,
        mode,
        success: true,
        flags: ['--' + listing],
      });
    }
  for (const mode of ['explicit', 'implicit']) {
    cases.push({
      name: mode + '-foreign-active',
      mode,
      active: true,
      flags: ['--foreign-active'],
    });
  }
  for (const mode of ['explicit', 'implicit']) {
    for (const version of ['1.0', '1.1', '1.2', '1.3']) {
      for (const active of [false, true]) {
        const legacy = version < '1.2';
        cases.push({
          name: mode + '-fixed-' + version + '-' + active,
          mode,
          active,
          success: true,
          tls: version,
          flags: [
            '--tls-version=' + (769 + Number(version[2])),
            ...(legacy ? ['--legacy-tls'] : []),
          ],
          settings:
            'tls_min_version=' +
            version +
            '\ntls_max_version=' +
            version +
            '\n' +
            (legacy ? 'tls_compatibility=openssl_legacy\n' : ''),
        });
      }
    }
    for (const version of ['1.0', '1.1']) {
      cases.push({
        name: mode + '-standard-old-' + version,
        mode,
        noLogin: true,
        flags: ['--tls-version=' + (769 + Number(version[2])), '--legacy-tls'],
        settings:
          'tls_min_version=' + version + '\ntls_max_version=' + version + '\n',
      });
      for (const issue of [
        { suffix: 'untrusted', ca: '' },
        { suffix: 'wrong-host', cert: wrong, key: wrongKey, ca: wrong },
        { suffix: 'expired', cert: expired, ca: expired },
      ]) {
        cases.push({
          ...issue,
          name: mode + '-legacy-cert-' + version + '-' + issue.suffix,
          mode,
          noLogin: true,
          error: 'curl 60',
          flags: [
            '--tls-version=' + (769 + Number(version[2])),
            '--legacy-tls',
          ],
          settings:
            'tls_min_version=' +
            version +
            '\ntls_max_version=' +
            version +
            '\ntls_compatibility=openssl_legacy\n',
        });
      }
    }
    cases.push({
      name: mode + '-range',
      mode,
      success: true,
      tls: '1.2',
      flags: ['--tls-version=771'],
      settings: 'tls_min_version=1.0\ntls_max_version=1.2\n',
    });
    cases.push({
      name: mode + '-above-range',
      mode,
      noLogin: true,
      flags: ['--tls-version=772'],
      settings: 'tls_min_version=1.0\ntls_max_version=1.2\n',
    });
    cases.push({
      name: mode + '-below-range',
      mode,
      noLogin: true,
      flags: ['--tls-version=771'],
      settings: 'tls_min_version=1.3\n',
    });
    cases.push({
      name: mode + '-cipher12',
      mode,
      success: true,
      cipher: 'AES128-SHA',
      flags: ['--tls-version=771', '--tls-ciphers=AES128-SHA'],
      settings:
        'tls_min_version=1.2\ntls_max_version=1.2\ntls_cipher_list=AES128-SHA\ntls13_cipher_list=TLS_AES_128_GCM_SHA256\n',
    });
    cases.push({
      name: mode + '-cipher13',
      mode,
      success: true,
      cipher: 'TLS_AES_128_GCM_SHA256',
      flags: ['--tls-version=772', '--tls13-ciphers=TLS_AES_128_GCM_SHA256'],
      settings:
        'tls_min_version=1.3\ntls_max_version=1.3\ntls_cipher_list=AES128-SHA\ntls13_cipher_list=TLS_AES_128_GCM_SHA256\n',
    });
    cases.push({
      name: mode + '-no-common-cipher',
      mode,
      noLogin: true,
      flags: ['--tls-version=771', '--tls-ciphers=AES256-SHA'],
      settings:
        'tls_min_version=1.2\ntls_max_version=1.2\ntls_cipher_list=AES128-SHA\n',
    });
    cases.push({
      name: mode + '-unknown-cipher12',
      mode,
      noLogin: true,
      error: 'curl 59',
      settings: 'tls_cipher_list=NO_SUCH_CIPHER\n',
    });
    cases.push({
      name: mode + '-unknown-cipher13',
      mode,
      noLogin: true,
      error: 'curl 59',
      settings: 'tls13_cipher_list=NO_SUCH_CIPHER\n',
    });
  }
  for (const command of ['TLS', 'SSL'])
    for (const order of ['tls', 'ssl', 'default']) {
      cases.push({
        name: 'auth-' + command + '-' + order,
        success: true,
        mode: 'explicit',
        flags: ['--auth-command=' + command],
        settings: 'tls_auth_order=' + order + '\n',
        authOrder: order,
      });
    }
  for (const settings of [
    'tls_min_version=SSLv3',
    'tls_max_version=SSLv2',
    'tls_min_version=1.3\ntls_max_version=1.2',
    'tls_auth_order=bad',
    'tls_compatibility=bad',
    'tls_cipher_list=DEFAULT:@SECLEVEL=0',
  ]) {
    cases.push({
      name: 'invalid-settings-' + cases.length,
      noLogin: true,
      settings: settings + '\n',
      error: '[ftp]',
    });
  }
  for (const mode of ['explicit', 'implicit']) {
    const legacy =
      'tls_min_version=1.0\ntls_max_version=1.0\ntls_compatibility=openssl_legacy\n';
    const standard = 'tls_min_version=1.0\ntls_max_version=1.0\n';
    cases.push({
      name: mode + '-legacy-isolation',
      mode,
      success: true,
      flags: ['--tls-version=769', '--legacy-tls'],
      settings: legacy,
      followup: { success: false, settings: standard },
    });
    cases.push({
      name: mode + '-standard-isolation',
      mode,
      success: false,
      flags: ['--tls-version=769', '--legacy-tls'],
      settings: standard,
      followup: { success: true, settings: legacy },
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
        `--tls=${scenario.mode === 'implicit' ? 'implicit' : 'explicit'}`,
        `--cert=${scenario.cert ?? cert}`,
        `--key=${scenario.key ?? key}`,
        ...(scenario.flags ?? []),
      ],
      { stdio: ['pipe', 'pipe', 'pipe'] }
    );
    const exited = once(server, 'exit');
    let trace = '';
    server.stderr.on('data', (data) => {
      trace += data;
    });
    try {
      const [ready] = await Promise.race([
        once(server.stdout, 'data'),
        (async () => {
          const [code] = await exited;
          throw new Error(
            'FTPS fixture exited before READY: ' + code + ' ' + trace
          );
        })(),
      ]);
      const match = /^READY (\d+)/.exec(String(ready));
      assert.ok(match, String(ready));
      const ini = join(directory, 'connection.ini');
      await writeFile(
        ini,
        `[ftp]\naddress=${scenario.ipv6 ? '::1' : '127.0.0.1'}\nport=${match[1]}\nusername=alice\ndata_connection_mode=${scenario.active ? 'active' : 'passive'}\ntls_mode=${scenario.mode ?? 'explicit'}\nca_file=${scenario.ca ?? cert}\n${scenario.settings ?? ''}`
      );
      const followupArgs = [];
      if (scenario.followup) {
        const followup = join(directory, 'followup.ini');
        await writeFile(
          followup,
          '[ftp]\naddress=127.0.0.1\nport=' +
            match[1] +
            '\nusername=alice\ntls_mode=' +
            scenario.mode +
            '\nca_file=' +
            cert +
            '\n' +
            scenario.followup.settings
        );
        followupArgs.push(
          followup,
          scenario.followup.success ? 'success' : 'failure'
        );
      }
      const result = await run(process.argv[2], [
        ini,
        scenario.success ? 'success' : 'failure',
        ...followupArgs,
      ]);
      assert.equal(result.code, 0, result.output + trace);
      if (['wrong-host', 'expired', 'untrusted'].includes(scenario.name))
        assert.ok(result.output.includes('curl 60'), result.output);
      if (scenario.name === 'invalid-mode')
        assert.ok(result.output.includes('[ftp] tls_mode'), result.output);
      if (scenario.name === 'protection-refused')
        assert.ok(trace.includes('COMMAND PROT P'), trace);
      if (scenario.name === 'data-tls-failed')
        assert.ok(
          trace.includes('COMMAND MLSD') || trace.includes('COMMAND LIST'),
          trace
        );
      if (scenario.error)
        assert.ok(result.output.includes(scenario.error), result.output);
      if (scenario.success && scenario.tls) {
        const version =
          scenario.tls === '1.0' ? 'TLSv1' : 'TLSv' + scenario.tls;
        const negotiated = trace
          .split('\n')
          .filter(
            (line) =>
              line.startsWith('TLS CONTROL') || line.startsWith('TLS DATA')
          );
        assert.ok(
          negotiated.length > 2 &&
            negotiated.every((line) => line.split(' ')[2] === version),
          trace
        );
      }
      if (scenario.success && scenario.cipher) {
        const channels = trace
          .split('\n')
          .filter((line) => line.startsWith('TLS DATA'));
        assert.ok(
          channels.length > 1 &&
            channels.every((line) => line.includes(scenario.cipher)),
          trace
        );
      }
      if (scenario.authOrder && scenario.authOrder !== 'default') {
        const first = trace
          .split('\n')
          .find((line) => line.startsWith('COMMAND AUTH '));
        assert.equal(
          first,
          'COMMAND AUTH ' + scenario.authOrder.toUpperCase(),
          trace
        );
      }
      if (scenario.noLogin) assert.ok(!trace.includes('COMMAND USER'), trace);
      if (scenario.success && scenario.mode) {
        assert.ok(
          trace.includes('TLS CONTROL') && trace.includes('TLS DATA'),
          trace
        );
        assert.equal(
          trace.includes('COMMAND AUTH'),
          scenario.mode === 'explicit',
          trace
        );
        if (scenario.flags?.includes('--legacy-data'))
          assert.ok(
            trace.includes(scenario.active ? 'COMMAND PORT' : 'COMMAND PASV'),
            trace
          );
      }
      if (scenario.success)
        assert.ok(
          trace.includes('COMMAND STOR') && trace.includes('COMMAND RETR'),
          trace
        );
      console.log(`PASS ${scenario.name}`);
    } catch (error) {
      ++failures;
      console.error(`FAIL ${scenario.name}: ${error.message}`);
    } finally {
      server.kill();
      await exited;
    }
  }
  for (const mode of ['explicit', 'implicit']) {
    for (const name of [
      'passive',
      'active',
      'legacy-passive',
      'legacy-active',
      'backpressure',
      'cancel-backpressure',
      'held-upload',
      'held-download',
      'cancel-final-upload',
      'cancel-final-download',
      'store-refused',
      'upload-error',
      'download-error',
      'upload-disconnect',
      'download-disconnect',
      'truncated',
      'large-size',
      'abandon',
      'stop-reader',
      'engine',
      'engine-failure',
      'cancel-control-tls',
      'cancel-data-tls',
      'stop-data-tls',
    ]) {
      const result = await run(process.argv[4], [
        process.argv[3],
        name,
        mode,
        cert,
        key,
      ]);
      if (result.code !== 0) {
        ++failures;
        console.error(
          'FAIL ' + mode + '-stream-' + name + ': ' + result.output
        );
      } else console.log('PASS ' + mode + '-stream-' + name);
    }
    for (const name of ['normal', 'login', 'cancel']) {
      const result = await run(process.argv[5], [
        process.argv[3],
        name,
        mode,
        cert,
        key,
      ]);
      if (result.code !== 0) {
        ++failures;
        console.error(
          'FAIL ' + mode + '-lifetime-' + name + ': ' + result.output
        );
      } else console.log('PASS ' + mode + '-lifetime-' + name);
    }
  }
} finally {
  await rm(root, { recursive: true, force: true });
}
process.exitCode = failures ? 1 : 0;
