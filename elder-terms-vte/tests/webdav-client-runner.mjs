import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createWebdavTestServer } from './webdav-test-server.ts';

const run = async (command, args) => {
  const child = spawn(command, args, { stdio: 'inherit' });
  const [code, signal] = await once(child, 'close');
  if (code !== 0) throw new Error(`${command} failed: ${code ?? signal}`);
};

const directory = await mkdtemp(join(tmpdir(), 'elder-dav-client-'));
try {
  const cert = join(directory, 'ca.pem');
  const key = join(directory, 'key.pem');
  await run('openssl', [
    'req',
    '-x509',
    '-newkey',
    'rsa:2048',
    '-nodes',
    '-days',
    '2',
    '-subj',
    '/CN=127.0.0.1',
    '-addext',
    'subjectAltName=IP:127.0.0.1',
    '-keyout',
    key,
    '-out',
    cert,
  ]);
  const tls = {
    cert: await readFile(cert, 'utf8'),
    key: await readFile(key, 'utf8'),
  };
  for (const scheme of ['http', 'https']) {
    for (const authentication of ['none', 'basic', 'digest']) {
      const server = await createWebdavTestServer(
        authentication,
        scheme === 'https' ? tls : undefined
      );
      try {
        const ca = scheme === 'https' ? cert : '';
        const runCase = async (auth, password, trust, expected) => {
          console.log('WebDAV:', scheme, auth, expected);
          await run(process.argv[2], [
            scheme,
            String(server.port),
            auth,
            password,
            trust,
            expected,
          ]);
        };
        await runCase(authentication, 'secret', ca, 'success');
        if (authentication !== 'none') {
          await runCase('auto', 'secret', ca, 'success');
          await runCase(authentication, 'incorrect', ca, 'failure');
        }
        if (scheme === 'https')
          await runCase(authentication, 'secret', '', 'failure');
        if (scheme === 'http' && authentication === 'none') {
          for (const path of [
            '/redirect',
            '/denied',
            '/not-dav',
            '/malformed',
            '/outside',
            '/cross',
            '/loop',
          ]) {
            console.log('WebDAV resource:', path);
            await run(process.argv[2], [
              scheme,
              String(server.port),
              authentication,
              '',
              '',
              path === '/redirect' ? 'success' : 'failure',
              path,
            ]);
          }
          if (
            server.requests.some((request) =>
              request.url.startsWith('/outside/')
            )
          ) {
            throw new Error(
              'WebDAV followed a redirect outside its virtual root'
            );
          }
          const child = spawn(
            process.argv[2],
            [
              scheme,
              String(server.port),
              authentication,
              '',
              '',
              'success',
              '/hold',
            ],
            { stdio: ['pipe', 'inherit', 'inherit'] }
          );
          const completion = once(child, 'close');
          await server.held;
          child.stdin.end('cancel\n');
          const [code] = await completion;
          if (code !== 0)
            throw new Error('Pending DAV connection did not cancel cleanly');
        }
      } finally {
        await server.close();
      }
    }
  }
} finally {
  await rm(directory, { recursive: true, force: true });
}
