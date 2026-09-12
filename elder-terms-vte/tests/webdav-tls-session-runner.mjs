import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import { createWebdavTestServer } from './webdav-test-server.ts';

const directory = await mkdtemp(join(tmpdir(), 'elder-dav-tls-session-'));
let server;
let child;
let completion;
try {
  const certificates = [];
  for (const name of ['first', 'replacement']) {
    const key = join(directory, name + '.key'),
      cert = join(directory, name + '.pem');
    const openssl = spawn(
      'openssl',
      [
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
        '/CN=' + name,
        '-addext',
        'subjectAltName=IP:127.0.0.1',
      ],
      { stdio: 'ignore' }
    );
    assert.equal(
      (await once(openssl, 'close'))[0],
      0,
      'TLS certificate generation must succeed'
    );
    certificates.push({
      cert: await readFile(cert, 'utf8'),
      key: await readFile(key, 'utf8'),
    });
  }
  server = await createWebdavTestServer('basic', certificates[0]);
  child = spawn(process.argv[2], [String(server.port)], {
    stdio: ['pipe', 'pipe', 'inherit'],
  });
  completion = once(child, 'close');
  const lines = createInterface({ input: child.stdout });
  const iterator = lines[Symbol.asyncIterator]();
  const waitForMarker = async (marker) => {
    for (;;) {
      const next = await iterator.next();
      assert.equal(next.done, false, 'Client exited before ' + marker);
      console.log(next.value);
      if (next.value === marker) return;
    }
  };
  await waitForMarker('READY');
  assert.ok(
    server.requests.length > 0,
    'Initial read must reach the server after approval'
  );
  const before = server.requests.length;
  server.replaceTls(certificates[1]);
  child.stdin.write('r');
  await waitForMarker('FAILED_MUTATION');
  assert.equal(
    server.requests.length,
    before,
    'Certificate approval during MOVE must not send HTTP requests before explicit retry'
  );
  child.stdin.end('t');
  for (;;) {
    const next = await iterator.next();
    if (next.done) break;
    console.log(next.value);
  }
  const [code, signal] = await completion;
  assert.equal(code, 0, 'Certificate mutation driver failed: ' + signal);
  const mutations = server.requests.filter(
    (request) => request.method === 'MOVE'
  );
  assert.equal(
    mutations.length,
    1,
    'Only the distinct explicit retry may perform MOVE'
  );
  assert.equal(mutations[0].authenticated, true);
  assert.equal(mutations[0].url, '/dav/hello.txt');
  assert.equal(mutations[0].overwrite, 'F');
  assert.ok(mutations[0].destination.endsWith('/dav/renamed.txt'));
} finally {
  if (child && child.exitCode === null && child.signalCode === null)
    child.kill();
  if (completion) await completion;
  if (server) await server.close();
  await rm(directory, { recursive: true, force: true });
}
