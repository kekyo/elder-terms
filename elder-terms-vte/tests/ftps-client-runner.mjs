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
const runtime = await run(process.argv[2], ['--runtime-version']);
assert.equal(runtime.code, 0, runtime.output);
const activeFtpsSupported = Number(runtime.output.trim()) >= 0x080000;
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
  const alternate = join(root, 'alternate.pem');
  const alternateKey = join(root, 'alternate-key.pem');
  const alternateResult = await run('openssl', [
    'req',
    '-x509',
    '-newkey',
    'rsa:2048',
    '-noenc',
    '-keyout',
    alternateKey,
    '-out',
    alternate,
    '-days',
    '1',
    '-subj',
    '/CN=alternate',
    '-addext',
    'subjectAltName=IP:127.0.0.1',
  ]);
  assert.equal(alternateResult.code, 0, alternateResult.output);
  const future = join(root, 'future.pem');
  const request = join(root, 'future.csr');
  const caConfig = join(root, 'ca.cnf');
  await writeFile(join(root, 'index'), '');
  await writeFile(join(root, 'serial'), '01\n');
  await writeFile(
    caConfig,
    '[ca]\ndefault_ca=test\n[test]\ndatabase=' +
      join(root, 'index') +
      '\nserial=' +
      join(root, 'serial') +
      '\nnew_certs_dir=' +
      root +
      '\ncertificate=' +
      cert +
      '\nprivate_key=' +
      key +
      '\ndefault_md=sha256\npolicy=policy\nx509_extensions=server\n[policy]\ncommonName=supplied\n[server]\nbasicConstraints=critical,CA:TRUE\nsubjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1\n'
  );
  const csr = await run('openssl', [
    'req',
    '-new',
    '-key',
    key,
    '-subj',
    '/CN=localhost',
    '-out',
    request,
  ]);
  assert.equal(csr.code, 0, csr.output);
  // Keep notBefore earlier than notAfter even for an expired certificate.
  // OpenSSL 3.5 rejects negative -days values that reverse that ordering.
  for (const [output, start, end] of [
    [expired, '20000101000000Z', '20010101000000Z'],
    [future, '20990101000000Z', '21000101000000Z'],
  ]) {
    await writeFile(join(root, 'index'), '');
    const signed = await run('openssl', [
      'ca',
      '-batch',
      '-selfsign',
      '-config',
      caConfig,
      '-in',
      request,
      '-out',
      output,
      '-startdate',
      start,
      '-enddate',
      end,
    ]);
    assert.equal(signed.code, 0, signed.output);
  }
  const policyCases = [];
  for (const host of [
    '127.0.0.1',
    '::1',
    'localhost',
    'LOCALHOST',
    'localhost.',
  ])
    policyCases.push({ host, certificate: cert, success: true });
  for (const value of [
    {
      name: 'ip-san-blocks-dns-cn',
      subject: 'localhost',
      san: 'IP:127.0.0.1',
      host: 'localhost',
      success: false,
    },
    {
      name: 'multiple-cn-last-mismatch',
      subject: 'localhost/CN=wrong.invalid',
      san: '',
      host: 'localhost',
      success: false,
    },
    {
      name: 'multiple-cn-last-match',
      subject: 'wrong.invalid/CN=localhost',
      san: '',
      host: 'localhost',
      success: true,
    },
    {
      name: 'legacy-cn-ip',
      subject: '127.0.0.1',
      san: '',
      host: '127.0.0.1',
      success: true,
    },
    {
      name: 'san-trailing-dot',
      subject: 'localhost',
      san: 'DNS:localhost.',
      host: 'localhost',
      success: true,
    },
    {
      name: 'cn-trailing-dot',
      subject: 'localhost.',
      san: '',
      host: 'localhost',
      success: true,
    },
    {
      name: 'broad-before-valid',
      subject: 'wrong.invalid',
      san: 'DNS:*.test,DNS:ftp.test',
      host: 'ftp.test',
      success: true,
    },
    {
      name: 'cn-only',
      subject: 'localhost',
      san: '',
      host: 'localhost',
      success: true,
    },
    {
      name: 'san-priority',
      subject: 'localhost',
      san: 'DNS:wrong.invalid',
      host: 'localhost',
      success: false,
    },
    {
      name: 'dns-is-not-ip',
      subject: '127.0.0.1',
      san: 'DNS:127.0.0.1',
      host: '127.0.0.1',
      success: false,
    },
    {
      name: 'wildcard',
      subject: 'wildcard',
      san: 'DNS:*.example.test',
      host: 'ftp.example.test',
      success: true,
    },
    {
      name: 'wildcard-depth',
      subject: 'wildcard',
      san: 'DNS:*.example.test',
      host: 'deep.ftp.example.test',
      success: false,
    },
    {
      name: 'partial-wildcard',
      subject: 'wildcard',
      san: 'DNS:f*.example.test',
      host: 'ftp.example.test',
      success: false,
    },
    {
      name: 'broad-wildcard',
      subject: 'wildcard',
      san: 'DNS:*.test',
      host: 'ftp.test',
      success: false,
    },
    {
      name: 'idn',
      subject: 'idn',
      san: 'DNS:xn--bcher-kva.example',
      host: 'bücher.example',
      success: true,
    },
    {
      name: 'rotation',
      subject: 'replacement',
      san: 'IP:127.0.0.1',
      host: '127.0.0.1',
      success: true,
    },
  ]) {
    const certificate = join(root, value.name + '.pem');
    const generated = await run('openssl', [
      'req',
      '-x509',
      '-key',
      key,
      '-out',
      certificate,
      '-days',
      '1',
      '-subj',
      '/CN=' + value.subject,
      ...(value.san ? ['-addext', 'subjectAltName=' + value.san] : []),
    ]);
    assert.equal(generated.code, 0, generated.output);
    policyCases.push({ ...value, certificate });
  }
  for (const scenario of policyCases) {
    const result = await run(process.argv[6], [
      scenario.host,
      scenario.certificate,
      key,
      'trusted',
      scenario.success ? 'success' : 'failure',
    ]);
    assert.equal(
      result.code,
      0,
      scenario.host + ' ' + scenario.certificate + ' ' + result.output
    );
  }
  const scope = await run(process.argv[6], [
    '127.0.0.1',
    cert,
    key,
    'untrusted',
    join(root, 'rotation.pem'),
  ]);
  assert.equal(scope.code, 0, scope.output);
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
    { name: 'future-rejected', cert: future, ca: future, noLogin: true },
    { name: 'untrusted', ca: '', noLogin: true },
    { name: 'bad-ca', ca: join(root, 'absent.pem'), noLogin: true },
    { name: 'auth-refused', flags: ['--reject-auth'], noLogin: true },
    { name: 'protection-refused', flags: ['--reject-protection'] },
    { name: 'data-tls-failed', flags: ['--break-data-tls'] },
    { name: 'invalid-mode', mode: 'explict', noLogin: true },
  ];
  // Compare standard libcurl verification and the confirmation policy using
  // the same numeric host forms, CA and server. Dotted DNS names use a
  // test-only resolver mapping and encrypted directory listing.
  for (const mode of ['explicit', 'implicit']) {
    for (const action of ['reject', 'prompt']) {
      for (const address of ['127.1', '0x7f000001']) {
        cases.push({
          name: mode + '-numeric-' + address + '-' + action,
          mode,
          address,
          success: true,
          confirmations: 0,
          settings: 'certificate_error_action=' + action + '\n',
        });
      }
      cases.push({
        name: mode + '-dotted-ip-san-' + action,
        mode,
        address: '127.0.0.1.',
        certificateOnly: true,
        action,
        referenceKey: mode + '-ip',
        settings: 'certificate_error_action=' + action + '\n',
      });
      const numericDns = join(root, 'dns-is-not-ip.pem');
      cases.push({
        name: mode + '-dotted-dns-san-' + action,
        mode,
        address: '127.0.0.1.',
        certificateOnly: true,
        action,
        referenceKey: mode + '-dns',
        cert: numericDns,
        ca: numericDns,
        settings: 'certificate_error_action=' + action + '\n',
      });
    }
  }
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
  for (const mode of ['explicit', 'implicit']) {
    for (const issue of [
      { name: 'untrusted', ca: '', confirmations: 1 },
      { name: 'future', cert: future, ca: future, confirmations: 1 },
      { name: 'expired', cert: expired, ca: expired, confirmations: 1 },
      { name: 'name', cert: wrong, key: wrongKey, ca: wrong, confirmations: 1 },
      {
        name: 'untrusted-name',
        cert: wrong,
        key: wrongKey,
        ca: '',
        confirmations: 2,
      },
    ]) {
      cases.push({
        ...issue,
        name: mode + '-approve-' + issue.name,
        mode,
        success: true,
        result: 'approve',
        settings: 'certificate_error_action=prompt\n',
      });
      cases.push({
        ...issue,
        name: mode + '-deny-' + issue.name,
        mode,
        noLogin: true,
        result: 'deny',
        confirmations: 1,
        settings: 'certificate_error_action=prompt\n',
      });
    }
    cases.push({
      name: mode + '-prompt-version-refused',
      mode,
      noLogin: true,
      confirmations: 0,
      ca: '',
      flags: ['--tls-version=769', '--legacy-tls'],
      settings: 'certificate_error_action=prompt\ntls_min_version=1.2\n',
    });
    cases.push({
      name: mode + '-prompt-ca-unreadable',
      mode,
      noLogin: true,
      confirmations: 0,
      ca: join(root, 'missing-ca.pem'),
      settings: 'certificate_error_action=prompt\n',
    });
    cases.push({
      name: mode + '-prompt-cipher-refused',
      mode,
      noLogin: true,
      confirmations: 0,
      flags: ['--tls-version=771', '--tls-ciphers=AES256-SHA'],
      settings:
        'certificate_error_action=prompt\ntls_min_version=1.2\ntls_max_version=1.2\ntls_cipher_list=AES128-SHA\n',
    });
    cases.push({
      name: mode + '-prompt-valid',
      mode,
      success: true,
      result: 'approve',
      confirmations: 0,
      settings: 'certificate_error_action=prompt\n',
    });
    cases.push({
      name: mode + '-prompt-legacy',
      mode,
      success: true,
      result: 'approve',
      ca: '',
      confirmations: 1,
      flags: ['--tls-version=769', '--legacy-tls'],
      settings:
        'certificate_error_action=prompt\ntls_min_version=1.0\ntls_max_version=1.0\ntls_compatibility=openssl_legacy\n',
    });
    for (const active of [false, true])
      cases.push({
        name: mode + '-approve-data-' + active,
        mode,
        active,
        success: true,
        result: 'approve-data',
        confirmations: 1,
        flags: ['--data-cert=' + alternate, '--data-key=' + alternateKey],
        settings: 'certificate_error_action=prompt\n',
      });
    for (const legacy of [false, true])
      cases.push({
        name: mode + '-approve-data-before-preliminary-' + legacy,
        mode,
        active: false,
        success: true,
        result: 'approve-data',
        confirmations: 1,
        flags: [
          '--data-cert=' + alternate,
          '--data-key=' + alternateKey,
          '--upload-tls-before-preliminary',
          ...(legacy ? ['--legacy-data'] : []),
        ],
        settings: 'certificate_error_action=prompt\n',
      });
    cases.push({
      name: mode + '-new-session-asks-again',
      mode,
      success: true,
      result: 'approve',
      ca: '',
      confirmations: 2,
      settings: 'certificate_error_action=prompt\n',
      followup: {
        success: true,
        result: 'approve',
        ca: '',
        settings: 'certificate_error_action=prompt\n',
      },
    });
  }
  const hostnameReferences = new Map();
  for (const mode of ['explicit', 'implicit'])
    for (const cipher of ['TLS_SHA256_SHA256', 'TLS_SHA384_SHA384'])
      cases.push({
        name: mode + '-integrity-only-' + cipher,
        mode,
        success: false,
        settings:
          'tls_compatibility=openssl_legacy\ntls13_cipher_list=TLS_AES_128_GCM_SHA256:' +
          cipher +
          '\n',
        error: 'must encrypt',
        noLogin: true,
      });
  for (let scenario of cases) {
    if (scenario.active && !activeFtpsSupported)
      scenario = {
        ...scenario,
        success: false,
        result: 'failure',
        followup: undefined,
        error: 'Active FTPS requires libcurl 8.0.0',
        noLogin: true,
        confirmations: 0,
        authOrder: undefined,
      };
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
        `[ftp]\naddress=${scenario.address ?? (scenario.ipv6 ? '::1' : '127.0.0.1')}\nport=${match[1]}\nusername=alice\nlocal_directory=${directory}\ndata_connection_mode=${scenario.active ? 'active' : 'passive'}\ntls_mode=${scenario.mode ?? 'explicit'}\nca_file=${scenario.ca ?? cert}\n${scenario.settings ?? ''}`
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
            (scenario.followup.ca ?? cert) +
            '\n' +
            scenario.followup.settings
        );
        followupArgs.push(
          followup,
          scenario.followup.result ??
            (scenario.followup.success ? 'success' : 'failure')
        );
      }
      if (scenario.certificateOnly && scenario.action === 'prompt') {
        assert.ok(
          hostnameReferences.has(scenario.referenceKey),
          'Missing standard libcurl reference'
        );
        scenario.success = hostnameReferences.get(scenario.referenceKey);
      }
      const result = scenario.certificateOnly
        ? await run(process.argv[7], [
            scenario.mode,
            scenario.address,
            match[1],
            scenario.ca ?? cert,
            scenario.action,
            scenario.action === 'reject'
              ? 'reference'
              : scenario.success
                ? 'success'
                : 'failure',
          ])
        : await run(process.argv[2], [
            ini,
            scenario.result ?? (scenario.success ? 'success' : 'failure'),
            ...followupArgs,
          ]);
      assert.equal(result.code, 0, result.output + trace);
      if (scenario.certificateOnly) {
        const code = /^RESULT curl (\d+)$/m.exec(result.output);
        assert.ok(code, result.output);
        assert.ok(['0', '60'].includes(code[1]), result.output);
        scenario.success = code[1] === '0';
        scenario.noLogin = !scenario.success;
        scenario.error = scenario.success ? undefined : 'curl 60';
        scenario.confirmations =
          scenario.action === 'prompt' && !scenario.success ? 1 : 0;
        if (scenario.action === 'reject') {
          hostnameReferences.set(scenario.referenceKey, scenario.success);
          const other =
            scenario.mode +
            (scenario.referenceKey.endsWith('-ip') ? '-dns' : '-ip');
          if (hostnameReferences.has(other))
            assert.equal(
              Number(hostnameReferences.get(other)) + Number(scenario.success),
              1,
              'The numeric host must match exactly one of the IP and DNS certificate identities'
            );
        }
      }
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
      if (scenario.confirmations !== undefined)
        assert.equal(
          result.output
            .split('\n')
            .filter((line) => line.startsWith('CONFIRM ')).length,
          scenario.confirmations,
          result.output
        );
      if (scenario.result === 'approve-data') {
        assert.ok(result.output.includes('CONFIRM data '), result.output);
        assert.ok(
          result.output.includes('DATA FAILURE OBSERVED'),
          result.output
        );
        assert.equal(
          trace.split('\n').filter((line) => line === 'COMMAND STOR probe')
            .length,
          2,
          trace
        );
      }
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
      if (scenario.success && !scenario.certificateOnly)
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
      const unsupported =
        !activeFtpsSupported && ['active', 'legacy-active'].includes(name);
      const accepted = unsupported
        ? result.code === 1 &&
          result.output.includes('Active FTPS requires libcurl 8.0.0')
        : result.code === 0;
      if (!accepted) {
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
