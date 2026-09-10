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
  for (const scenario of cases) {
    const directory = join(root, scenario.name);
    await mkdir(join(directory, 'home'), { recursive: true });
    const server = spawn(
      process.argv[3],
      [
        directory,
        '--trace',
        '--tls=explicit',
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
      const [ready] = await once(server.stdout, 'data');
      const match = /^READY (\d+)/.exec(String(ready));
      assert.ok(match, String(ready));
      const ini = join(directory, 'connection.ini');
      await writeFile(
        ini,
        `[ftp]\naddress=127.0.0.1\nport=${match[1]}\nusername=alice\ntls_mode=${scenario.mode ?? 'explicit'}\nca_file=${scenario.ca ?? cert}\n`
      );
      const result = await run(process.argv[2], [
        ini,
        scenario.success ? 'success' : 'failure',
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
      if (scenario.noLogin) assert.ok(!trace.includes('COMMAND USER'), trace);
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
} finally {
  await rm(root, { recursive: true, force: true });
}
process.exitCode = failures ? 1 : 0;
