import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { existsSync } from 'node:fs';
import {
  chmod,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from 'node:fs/promises';
import { createConnection, createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// System trust changes are confined to disposable Podman test containers.
if (process.env.ELDER_TERMS_TEST_VSFTPD !== '1') process.exit(77);
assert.ok(
  existsSync('/run/.containerenv'),
  'Run the vsftpd integration suite in an isolated Podman container'
);
assert.equal(
  process.getuid(),
  0,
  'Container root is required for vsftpd anonymous chroot and private test CA setup'
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
  return { code, output };
};
const runtime = await run(process.argv[2], ['--runtime-version']);
assert.equal(runtime.code, 0, runtime.output);
const activeFtpsSupported = Number(runtime.output.trim()) >= 0x080000;
const root = await mkdtemp(join(tmpdir(), 'elder-vsftpd-'));
const trusted = join(
  '/usr/local/share/ca-certificates',
  `elder-ftps-${process.pid}.crt`
);
try {
  await chmod(root, 0o755);
  const certificate = join(root, 'certificate.pem'),
    key = join(root, 'key.pem');
  const generated = await run('openssl', [
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
  assert.equal(generated.code, 0, generated.output);
  const versions = await run('sh', [
    '-c',
    'dpkg-query -W vsftpd libcurl4 libcurl4t64 libssl3 libssl3t64 2>/dev/null; curl --version',
  ]);
  console.log(versions.output);
  for (const mode of ['explicit', 'implicit'])
    for (const version of ['1.2', '1.3'])
      for (const dataMode of ['passive', 'active']) {
        const unsupported = dataMode === 'active' && !activeFtpsSupported;
        const name = `${mode}-${version}-${dataMode}`;
        const directory = join(root, name);
        await mkdir(join(directory, 'home'), { recursive: true });
        await chmod(join(directory, 'home'), 0o777);
        const reservation = createServer();
        reservation.listen(0, '127.0.0.1');
        await once(reservation, 'listening');
        const port = reservation.address().port;
        await new Promise((resolve, reject) =>
          reservation.close((error) => (error ? reject(error) : resolve()))
        );
        const log = join(root, `${name}.log`),
          config = join(root, `${name}.conf`);
        await writeFile(
          config,
          [
            'listen=YES',
            'listen_ipv6=NO',
            'listen_address=127.0.0.1',
            `listen_port=${port}`,
            'background=NO',
            'anonymous_enable=YES',
            'local_enable=NO',
            'write_enable=YES',
            'anon_upload_enable=YES',
            'anon_mkdir_write_enable=YES',
            'anon_other_write_enable=YES',
            'anon_umask=022',
            `anon_root=${directory}`,
            'ssl_enable=YES',
            'allow_anon_ssl=YES',
            'force_anon_logins_ssl=YES',
            'force_anon_data_ssl=YES',
            // Pin a TLS 1.2 cipher; TLS 1.3 retains OpenSSL's TLS 1.3 defaults.
            'ssl_ciphers=ECDHE-RSA-AES128-GCM-SHA256',
            'require_ssl_reuse=YES',
            `implicit_ssl=${mode === 'implicit' ? 'YES' : 'NO'}`,
            `rsa_cert_file=${certificate}`,
            `rsa_private_key_file=${key}`,
            'debug_ssl=YES',
            'xferlog_enable=YES',
            'log_ftp_protocol=YES',
            `vsftpd_log_file=${log}`,
            'connect_from_port_20=NO',
            '',
          ].join('\n')
        );
        const server = spawn('vsftpd', [config], {
          stdio: ['ignore', 'pipe', 'pipe'],
          detached: true,
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
          const ini = join(root, `${name}.ini`);
          const settings = [
            '[ftp]',
            'address=127.0.0.1',
            `port=${port}`,
            'username=anonymous',
            `tls_mode=${mode}`,
            `tls_min_version=${version}`,
            `tls_max_version=${version}`,
            `data_connection_mode=${dataMode}`,
          ];
          for (const trust of [
            'private-ca',
            'reject-untrusted',
            'approve-untrusted',
            'system-ca',
          ]) {
            if (trust === 'system-ca') {
              await copyFile(certificate, trusted);
              const update = await run('update-ca-certificates', []);
              assert.equal(update.code, 0, update.output);
            }
            await writeFile(
              ini,
              [
                ...settings,
                `ca_file=${trust === 'private-ca' ? certificate : ''}`,
                `certificate_error_action=${trust === 'approve-untrusted' ? 'prompt' : 'reject'}`,
                '',
              ].join('\n')
            );
            const result = await run(process.argv[2], [
              ini,
              unsupported || trust === 'reject-untrusted'
                ? 'failure'
                : trust === 'approve-untrusted'
                  ? 'approve'
                  : 'success',
            ]);
            assert.equal(
              result.code,
              0,
              `${name}/${trust}: ${result.output}\n${output}`
            );
            if (unsupported)
              assert.match(result.output, /Active FTPS requires libcurl 8.0.0/);
            if (!unsupported && trust === 'reject-untrusted')
              assert.match(result.output, /curl 60/);
            if (!unsupported && trust === 'approve-untrusted')
              assert.match(result.output, /CONFIRM control/);
            console.log(`PASS vsftpd ${name}/${trust}`);
            if (trust === 'system-ca') {
              await rm(trusted);
              const update = await run('update-ca-certificates', []);
              assert.equal(update.code, 0, update.output);
            }
          }
          const trace = await readFile(log, 'utf8');
          if (unsupported) {
            assert.doesNotMatch(trace, /FTP command: Client .*USER/);
            continue;
          }
          assert.match(
            trace,
            /, reused,/,
            'The server must observe data TLS session reuse'
          );
          assert.doesNotMatch(trace, /No SSL session reuse on data channel/);
          // vsftpd logs SSL_get_cipher_version, not SSL_get_version. With the
          // client version fixed above and this TLS 1.2-only cipher, these labels
          // also distinguish the requested TLS 1.2 and TLS 1.3 connections.
          const negotiated = [
            ...trace.matchAll(/SSL version: ([^, \r\n]+)/g),
          ].map((match) => match[1]);
          assert.ok(
            negotiated.length > 0 &&
              negotiated.every((value) => value === 'TLSv' + version),
            'Every control/data handshake must use the requested TLS version: ' +
              trace
          );
        } catch (error) {
          let trace = '';
          try {
            trace = await readFile(log, 'utf8');
          } catch {}
          throw new Error(
            name + ': ' + error.message + '\n' + output + '\n' + trace
          );
        } finally {
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
} finally {
  if (existsSync(trusted)) {
    await rm(trusted);
    const update = await run('update-ca-certificates', []);
    assert.equal(update.code, 0, update.output);
  }
  await rm(root, { recursive: true, force: true });
}
