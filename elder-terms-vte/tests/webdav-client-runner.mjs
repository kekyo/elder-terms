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

const assertMutationRequests = (requests, authentication) => {
  const mutations = requests.filter(
    (request) =>
      request.authenticated &&
      ['PUT', 'MOVE', 'MKCOL', 'DELETE'].includes(request.method)
  );
  if (
    !mutations.some((request) => request.method === 'PUT') ||
    !mutations.some((request) => request.method === 'MOVE')
  )
    throw new Error('The client did not perform real uploads and commits');
  for (const request of mutations.filter((value) => value.method === 'PUT')) {
    if (
      request.contentLength === undefined ||
      !/^\d+$/.test(request.contentLength)
    )
      throw new Error('Known-size PUT must send an explicit Content-Length');
    if (request.receivedBytes > Number(request.contentLength))
      throw new Error('PUT sent more bytes than its declared size');
  }
  if (
    mutations.some(
      (request) =>
        request.method === 'DELETE' &&
        request.url === '/dav/uploaded/replace.txt'
    )
  )
    throw new Error(
      'Upload replacement deleted the completed destination before MOVE'
    );
  const replacements = mutations.filter(
    (request) =>
      request.method === 'MOVE' &&
      request.destination?.endsWith('/dav/uploaded/replace.txt')
  );
  if (
    replacements.length !== 2 ||
    replacements[0].overwrite !== 'F' ||
    replacements[1].overwrite !== 'T'
  )
    throw new Error(
      'New creation and approved replacement must have distinct MOVE overwrite policy'
    );
  if (
    mutations.some(
      (request) =>
        request.method === 'DELETE' &&
        request.url.includes('put-collision.txt.elder-terms-part-')
    )
  )
    throw new Error(
      'The client tried to delete a foreign conditional-PUT collision'
    );
  if (
    mutations.some(
      (request) =>
        request.method === 'DELETE' &&
        request.url.includes('put-drop.txt.elder-terms-part-')
    )
  )
    throw new Error(
      'The client tried to delete an upload whose ownership is uncertain'
    );
  for (const [method, matches] of [
    [
      'PUT',
      (request) => request.url.includes('put-drop.txt.elder-terms-part-'),
    ],
    [
      'MOVE',
      (request) => request.destination?.endsWith('/dav/uploaded/move-drop.txt'),
    ],
    ['DELETE', (request) => request.url === '/dav/uploaded/delete-drop.txt'],
  ]) {
    const attempts = mutations.filter(
      (request) => request.method === method && matches(request)
    );
    if (attempts.length !== 1)
      throw new Error(
        `${method} was replayed after a lost final response (${attempts.length} attempts)`
      );
  }
  const bundleDeletes = mutations.filter(
    (request) =>
      request.method === 'DELETE' &&
      request.url.startsWith('/dav/uploaded/bundle')
  );
  if (
    bundleDeletes.length !== 1 ||
    bundleDeletes[0].url !== '/dav/uploaded/bundle/'
  )
    throw new Error(
      'An explicit WebDAV tree deletion must target its collection once'
    );
  if (requests.some((request) => request.url === '/dav/redirected.txt'))
    throw new Error('A mutation redirect was followed automatically');
  for (const name of ['rewind.txt', 'rewind-small.txt']) {
    const replayedBodies = mutations.filter(
      (request) =>
        request.method === 'PUT' &&
        request.url.includes(name + '.elder-terms-part-') &&
        request.receivedBytes > 0
    );
    if (
      replayedBodies.length > 1 ||
      (authentication === 'digest' && replayedBodies.length !== 1)
    )
      throw new Error(
        'Digest renewal replayed a consumed forward-only PUT body: ' + name
      );
  }
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
  const failures = [];
  const withServer = async (scheme, authentication, label, check) => {
    const server = await createWebdavTestServer(
      authentication,
      scheme === 'https' ? tls : undefined
    );
    try {
      console.log('WebDAV:', scheme, authentication, label);
      await check(server);
    } catch (error) {
      const message =
        scheme + '/' + authentication + '/' + label + ': ' + error.message;
      failures.push(message);
      console.error('FAIL', message);
      console.error(
        'Recent WebDAV requests:',
        JSON.stringify(server.requests.slice(-12))
      );
    } finally {
      await server.close();
    }
  };
  const runCase = async (
    scheme,
    serverAuth,
    auth,
    password,
    trust,
    expected,
    path = '/'
  ) => {
    await withServer(
      scheme,
      serverAuth,
      auth + '/' + expected + '/' + path,
      async (server) => {
        await run(process.argv[2], [
          scheme,
          String(server.port),
          auth,
          password,
          trust,
          expected,
          path,
        ]);
        if (expected === 'success')
          assertMutationRequests(server.requests, auth);
        if (
          server.requests.some((request) => request.url.startsWith('/outside/'))
        )
          throw new Error(
            'WebDAV followed a redirect outside its virtual root'
          );
      }
    );
  };
  for (const scheme of ['http', 'https']) {
    for (const authentication of ['none', 'basic', 'digest']) {
      const ca = scheme === 'https' ? cert : '';
      await runCase(
        scheme,
        authentication,
        authentication,
        'secret',
        ca,
        'success'
      );
      if (authentication !== 'none') {
        await runCase(scheme, authentication, 'auto', 'secret', ca, 'success');
        await runCase(
          scheme,
          authentication,
          authentication,
          'incorrect',
          ca,
          'failure'
        );
      }
      if (scheme === 'https')
        await runCase(
          scheme,
          authentication,
          authentication,
          'secret',
          '',
          'failure'
        );
    }
  }
  for (const path of [
    '/redirect',
    '/denied',
    '/unsupported-auth',
    '/not-dav',
    '/malformed',
    '/outside',
    '/cross',
    '/loop',
  ])
    await runCase(
      'http',
      'none',
      'none',
      '',
      '',
      path === '/redirect' ? 'success' : 'failure',
      path
    );
  for (const mode of ['bounded-upload', 'abandon-upload'])
    await withServer('http', 'none', mode, async (server) => {
      const child = spawn(
        process.argv[2],
        ['http', String(server.port), 'none', '', '', 'success', '/', mode],
        { stdio: ['pipe', 'inherit', 'inherit'] }
      );
      const completion = once(child, 'close');
      try {
        await Promise.race([
          server.uploadBodyHeld,
          (async () => {
            const [code, signal] = await completion;
            throw new Error(
              'Upload exited before its paused request body: ' +
                (code ?? signal)
            );
          })(),
        ]);
        child.stdin.end('c');
        const [code] = await completion;
        if (code !== 0)
          throw new Error(
            'Bounded upload cancellation and queued session reuse failed'
          );
        const uploads = server.requests.filter(
          (request) =>
            request.authenticated &&
            request.method === 'PUT' &&
            request.url === '/dav/body-held.bin'
        );
        if (
          uploads.length !== 1 ||
          uploads[0].contentLength !== String(64 * 1024 * 1024)
        )
          throw new Error('The held upload must be one known-size request');
      } finally {
        if (child.exitCode === null && child.signalCode === null) child.kill();
        await completion;
      }
    });
  await withServer('http', 'none', 'cancel-upload', async (server) => {
    const child = spawn(
      process.argv[2],
      [
        'http',
        String(server.port),
        'none',
        '',
        '',
        'success',
        '/',
        'cancel-upload',
      ],
      { stdio: ['pipe', 'inherit', 'inherit'] }
    );
    const completion = once(child, 'close');
    try {
      await Promise.race([
        server.uploadHeld,
        (async () => {
          const [code, signal] = await completion;
          throw new Error(
            'Upload exited before its held final response: ' + (code ?? signal)
          );
        })(),
      ]);
      child.stdin.end('cancel\n');
      const [code] = await completion;
      if (code !== 0)
        throw new Error('Upload final-response cancellation failed');
      if (
        server.requests.some(
          (request) =>
            request.method === 'DELETE' &&
            request.url.includes('cancel-upload.txt.elder-terms-part-')
        )
      )
        throw new Error(
          'Cancellation deleted a temporary file whose ownership was not confirmed'
        );
    } finally {
      if (child.exitCode === null && child.signalCode === null) child.kill();
      await completion;
    }
  });
  await withServer('http', 'none', 'cancel-connection', async (server) => {
    const child = spawn(
      process.argv[2],
      ['http', String(server.port), 'none', '', '', 'success', '/hold'],
      { stdio: ['pipe', 'inherit', 'inherit'] }
    );
    const completion = once(child, 'close');
    try {
      await Promise.race([
        server.held,
        (async () => {
          const [code, signal] = await completion;
          throw new Error(
            'Connection exited before its held request: ' + (code ?? signal)
          );
        })(),
      ]);
      child.stdin.end('cancel\n');
      const [code] = await completion;
      if (code !== 0)
        throw new Error('Pending DAV connection did not cancel cleanly');
    } finally {
      if (child.exitCode === null && child.signalCode === null) child.kill();
      await completion;
    }
  });
  if (failures.length) throw new Error(failures.join('\n'));
} finally {
  await rm(directory, { recursive: true, force: true });
}
