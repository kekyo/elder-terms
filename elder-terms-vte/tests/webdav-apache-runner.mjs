import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { once } from 'node:events';
import { existsSync } from 'node:fs';
import {
  chmod,
  chown,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from 'node:fs/promises';
import { createConnection, createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// The optional host entry becomes mandatory in the isolated distribution runs.
if (process.env.ELDER_TERMS_TEST_APACHE_WEBDAV !== '1') process.exit(77);
const guest =
  process.env.ELDER_TERMS_TEST_DISPOSABLE_GUEST === '1' &&
  (await readFile('/proc/cmdline', 'utf8')).includes(
    'elder_terms_webdav_validation=1'
  );
assert.ok(
  existsSync('/run/.containerenv') || guest,
  'Run Apache and system-CA validation in a disposable container or validation guest'
);
assert.equal(
  process.getuid(),
  0,
  'Apache user isolation and private container CA setup require container root'
);
const { waitForResult } = await import('gestament/testing');
const run = async (command, args) => {
  const child = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', (bytes) => {
    output += bytes.toString();
  });
  child.stderr.on('data', (bytes) => {
    output += bytes.toString();
  });
  const [code] = await once(child, 'close');
  assert.equal(code, 0, `${command}: ${output}`);
  return output;
};
const root = await mkdtemp(join(tmpdir(), 'elder-apache-dav-'));
const trusted = `/usr/local/share/ca-certificates/elder-dav-${process.pid}.crt`;
const failures = [];
try {
  await chmod(root, 0o755);
  const certificate = join(root, 'certificate.pem'),
    key = join(root, 'key.pem');
  await run('openssl', [
    'req',
    '-x509',
    '-newkey',
    'rsa:2048',
    '-noenc',
    '-keyout',
    key,
    '-out',
    certificate,
    '-days',
    '1',
    '-subj',
    '/CN=localhost',
    '-addext',
    'subjectAltName=DNS:localhost,IP:127.0.0.1',
  ]);
  const uid = Number((await run('id', ['-u', 'www-data'])).trim());
  const gid = Number((await run('id', ['-g', 'www-data'])).trim());
  console.log(await run('apache2', ['-v']));
  console.log(
    await run('pkg-config', [
      '--modversion',
      'libcurl',
      'openssl',
      'libxml-2.0',
    ])
  );
  const basicFile = join(root, 'basic.passwd'),
    digestFile = join(root, 'digest.passwd');
  await run('htpasswd', ['-cbB', basicFile, 'alice', 'secret']);
  await writeFile(
    digestFile,
    'alice:DAV:' +
      createHash('md5').update('alice:DAV:secret').digest('hex') +
      '\n'
  );
  await chmod(basicFile, 0o644);
  for (const scheme of ['http', 'https'])
    for (const authentication of ['none', 'basic', 'digest'])
      for (const version of scheme === 'https' ? ['1.2', '1.3'] : ['plain']) {
        const name = `${scheme}-${authentication}-${version}`;
        const directory = join(root, name),
          documentRoot = join(directory, 'www');
        await mkdir(join(documentRoot, 'dav'), { recursive: true });
        await mkdir(join(directory, 'run'));
        await chown(join(documentRoot, 'dav'), uid, gid);
        await chown(join(directory, 'run'), uid, gid);
        const reservation = createServer();
        reservation.listen(0, '127.0.0.1');
        await once(reservation, 'listening');
        const port = reservation.address().port;
        await new Promise((resolve, reject) =>
          reservation.close((error) => (error ? reject(error) : resolve()))
        );
        const log = join(directory, 'requests.log'),
          errorLog = join(directory, 'errors.log');
        const config = join(directory, 'httpd.conf');
        const modules = [
          'mpm_event',
          'authn_core',
          'authn_file',
          'authz_core',
          'authz_user',
          'auth_basic',
          'auth_digest',
          'dav',
          'dav_fs',
          'socache_shmcb',
          'ssl',
        ];
        await writeFile(
          config,
          [
            `ServerRoot "${directory}"`,
            ...modules.map(
              (name) =>
                `LoadModule ${name}_module /usr/lib/apache2/modules/mod_${name}.so`
            ),
            `Listen 127.0.0.1:${port}`,
            'ServerName 127.0.0.1',
            'User www-data',
            'Group www-data',
            `PidFile "${directory}/run/httpd.pid"`,
            `DefaultRuntimeDir "${directory}/run"`,
            `ErrorLog "${errorLog}"`,
            'LogLevel warn',
            `CustomLog "${log}" "%m %U %>s %{SSL_PROTOCOL}x"`,
            `DocumentRoot "${documentRoot}"`,
            `DavLockDB "${directory}/run/DavLock"`,
            '<Directory />',
            'Require all denied',
            '</Directory>',
            `<Directory "${documentRoot}/dav">`,
            'Dav On',
            'Options None',
            ...(authentication === 'none'
              ? ['Require all granted']
              : [
                  `AuthType ${authentication === 'basic' ? 'Basic' : 'Digest'}`,
                  'AuthName DAV',
                  `AuthUserFile "${authentication === 'basic' ? basicFile : digestFile}"`,
                  ...(authentication === 'digest'
                    ? ['AuthDigestProvider file', 'AuthDigestDomain /dav/']
                    : ['AuthBasicProvider file']),
                  'Require user alice',
                ]),
            '</Directory>',
            ...(scheme === 'https'
              ? [
                  'SSLEngine on',
                  `SSLProtocol -all +TLSv${version}`,
                  `SSLCertificateFile "${certificate}"`,
                  `SSLCertificateKeyFile "${key}"`,
                ]
              : []),
            '',
          ].join('\n')
        );
        const server = spawn('apache2', ['-X', '-f', config], {
          detached: true,
          stdio: ['ignore', 'pipe', 'pipe'],
        });
        let output = '';
        server.stdout.on('data', (bytes) => {
          output += bytes.toString();
        });
        server.stderr.on('data', (bytes) => {
          output += bytes.toString();
        });
        const finished = once(server, 'close');
        try {
          await waitForResult(
            async () => {
              assert.equal(server.exitCode, null, output);
              const socket = createConnection({ host: '127.0.0.1', port });
              try {
                await once(socket, 'connect');
              } finally {
                socket.destroy();
              }
            },
            { timeoutMs: 30_000 }
          );
          const runCase = async (
            auth,
            password,
            ca,
            expected,
            label,
            certificateAction
          ) => {
            const result = await run(process.argv[2], [
              scheme,
              String(port),
              auth,
              password,
              ca,
              expected,
              certificateAction,
            ]);
            if (expected === 'failure')
              assert.match(result, /WebDAV request failed/);
            // mod_dav_fs keeps its private property database in .DAV; it is not a DAV resource.
            // https://github.com/apache/httpd/blob/2.4.x/modules/dav/fs/dbm.c
            const remaining = (await readdir(join(documentRoot, 'dav'))).filter(
              (name) => name !== '.DAV'
            );
            assert.deepEqual(
              remaining,
              [],
              'Each test must remove its files from the isolated Apache collection'
            );
            console.log(`PASS Apache ${name}/${label}`);
          };
          const ca = scheme === 'https' ? certificate : '';
          await runCase(
            authentication,
            'secret',
            ca,
            'success',
            'explicit-auth',
            'reject'
          );
          if (authentication !== 'none') {
            await runCase(
              'auto',
              'secret',
              ca,
              'success',
              'auto-auth',
              'reject'
            );
            await runCase(
              authentication,
              'incorrect',
              ca,
              'failure',
              'reject-password',
              'reject'
            );
          }
          if (scheme === 'https') {
            await runCase(
              authentication,
              'secret',
              '',
              'failure',
              'reject-untrusted',
              'reject'
            );
            await runCase(
              authentication,
              'secret',
              '',
              'success',
              'approve-private-ca',
              'prompt'
            );
            await copyFile(certificate, trusted);
            await run('update-ca-certificates', []);
            await runCase(
              authentication,
              'secret',
              '',
              'success',
              'system-ca',
              'reject'
            );
            await rm(trusted);
            await run('update-ca-certificates', []);
          }
          const trace = await readFile(log, 'utf8');
          for (const method of [
            'PROPFIND',
            'GET',
            'PUT',
            'MKCOL',
            'MOVE',
            'DELETE',
          ])
            assert.match(
              trace,
              new RegExp(`^${method} /dav/`, 'm'),
              `${method} must reach the independent server`
            );
          assert.doesNotMatch(trace, /^(LOCK|UNLOCK|COPY|PROPPATCH) /m);
          if (scheme === 'https') {
            const negotiated = [...trace.matchAll(/ (TLSv[^\s]+)$/gm)].map(
              (match) => match[1]
            );
            assert.ok(
              negotiated.length &&
                negotiated.every((value) => value === `TLSv${version}`),
              trace
            );
          }
        } catch (error) {
          let trace = '';
          try {
            trace = await readFile(errorLog, 'utf8');
          } catch {}
          failures.push(`${name}: ${error.message}\n${output}\n${trace}`);
        } finally {
          if (existsSync(trusted)) {
            await rm(trusted);
            await run('update-ca-certificates', []);
          }
          if (server.pid) {
            try {
              process.kill(-server.pid, 'SIGTERM');
            } catch (error) {
              if (error.code !== 'ESRCH') throw error;
            }
          }
          await finished;
        }
      }
  assert.equal(failures.length, 0, failures.join('\n'));
} finally {
  if (existsSync(trusted)) {
    await rm(trusted);
    await run('update-ca-certificates', []);
  }
  await rm(root, { recursive: true, force: true });
}
