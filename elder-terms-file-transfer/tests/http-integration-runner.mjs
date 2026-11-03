// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer as http } from 'node:http';
import { createServer as https } from 'node:https';
import { once } from 'node:events';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createInterface } from 'node:readline';
import { withSshGateway } from '../../shared/tests/ssh-proxy-test-helper.mjs';
const directory = await mkdtemp(join(tmpdir(), 'elder-http-integration-'));
const run = async (command, args, env = {}) => {
  const child = spawn(command, args, {
    stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...process.env, ...env },
  });
  let output = '';
  child.stdout.on('data', (bytes) => {
    output += bytes;
  });
  child.stderr.on('data', (bytes) => {
    output += bytes;
  });
  assert.equal((await once(child, 'close'))[0], 0, output);
};
try {
  const certificate = join(directory, 'ca.pem'),
    key = join(directory, 'key.pem');
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
    '/CN=HTTP integration',
    '-addext',
    'subjectAltName=IP:127.0.0.1,DNS:proxy-test.invalid',
  ]);
  let index = 0;
  const check = async (scheme, policy, trusted, password, env, host) => {
    const requests = [];
    const server =
      scheme === 'https'
        ? https({ key: await readFile(key), cert: await readFile(certificate) })
        : http();
    server.on('request', (req, res) => {
      requests.push(`${req.method} ${req.url}`);
      if (
        password !== 'none' &&
        req.headers.authorization !==
          'Basic ' + Buffer.from('alice:secret').toString('base64')
      ) {
        res.writeHead(401, {
          'WWW-Authenticate': 'Basic realm="HTTP"',
          'Content-Type': 'text/html',
        });
        res.end('<a href="poison">authentication challenge</a>');
      } else if (req.url === '/pub/') {
        res.setHeader('Content-Type', 'text/html');
        res.end(
          '<a href="tree/">tree</a>' +
            Array.from(
              { length: 150 },
              (_, n) => `<a href="untouched/${n}">untouched</a>`
            ).join('')
        );
      } else if (req.url === '/pub/tree/') {
        res.setHeader('Content-Type', 'text/html');
        res.end(
          '<a href="file.txt">file</a><a href="alias">alias</a><a href="again">again</a><a href="../">parent</a>'
        );
      } else if (req.url === '/pub/tree/again') {
        res.setHeader('Content-Type', 'text/html');
        res.end('<a href="alias">same alias from another page</a>');
      } else if (req.url === '/pub/tree/alias') {
        res.writeHead(302, { Location: 'file.txt' });
        res.end();
      } else if (req.url === '/pub/tree/file.txt') {
        res.setHeader('Content-Type', 'text/plain');
        res.end('HTTP integration bytes\n');
      } else {
        res.statusCode = 404;
        res.end();
      }
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const output = join(directory, `download-${index++}`);
    await mkdir(output);
    const failed =
      password === 'wrong' ||
      (scheme === 'https' && !trusted && policy !== 'allow');
    try {
      await run(
        process.argv[2],
        [
          `${scheme}://${host}:${server.address().port}/pub/`,
          output,
          trusted ? certificate : '',
          policy,
          failed ? 'failure' : 'success',
          password,
        ],
        env
      );
      assert.ok(
        requests.every((value) =>
          /^GET \/pub\/(?:tree\/(?:file.txt|alias|again)?|)$/.test(value)
        ),
        requests.join('\n')
      );
      if (scheme === 'https' && failed && password !== 'wrong')
        assert.equal(requests.length, 0);
      if (!failed)
        assert.equal(
          requests.filter((value) => value === 'GET /pub/tree/file.txt').length,
          1
        );
      if (!failed)
        assert.equal(
          requests.filter((value) => value === 'GET /pub/tree/alias').length,
          1,
          'A redirect alias shared by multiple pages must be visited only once per job'
        );
      console.log(
        `PASS ${scheme} ${policy} CA=${trusted} authentication=${password} host=${host}`
      );
    } finally {
      server.closeAllConnections();
      server.close();
      await once(server, 'close');
    }
  };
  for (const scheme of ['http', 'https']) {
    await check(scheme, 'reject', scheme === 'https', 'none', {}, '127.0.0.1');
    await check(
      scheme,
      'reject',
      scheme === 'https',
      'secret',
      {},
      '127.0.0.1'
    );
    await check(scheme, 'reject', scheme === 'https', 'wrong', {}, '127.0.0.1');
  }
  for (const policy of ['reject', 'allow', 'decline'])
    await check('https', policy, false, 'none', {}, '127.0.0.1');
  await withSshGateway(process.argv[3], directory, async (gateway) => {
    const env = {
      ELDER_TERMS_TEST_PROXY_PORT: String(gateway.port),
      ELDER_TERMS_TEST_PROXY_KNOWN_HOSTS: gateway.knownHosts,
      NO_PROXY: '*',
      no_proxy: '*',
    };
    await check('https', 'allow', false, 'secret', env, 'proxy-test.invalid');
    assert.ok(
      gateway.requests.length > 0 &&
        gateway.requests.every(
          (request) => request.host === 'proxy-test.invalid'
        )
    );
  });
  // Use the published static server CLI, including its own directory-listing HTML.
  const root = join(directory, 'www');
  await mkdir(join(root, 'pub', 'tree'), { recursive: true });
  await mkdir(join(root, 'pub', 'untouched'));
  await writeFile(
    join(root, 'pub', 'tree', 'file.txt'),
    'HTTP integration bytes\n'
  );
  const output = join(directory, 'static-download');
  await mkdir(output);
  const server = spawn(
    process.execPath,
    [
      fileURLToPath(import.meta.resolve('http-server/bin/http-server')),
      root,
      '-a',
      '127.0.0.1',
      '-p',
      '0',
      '-c-1',
    ],
    { stdio: ['ignore', 'pipe', 'pipe'] }
  );
  const completion = once(server, 'close');
  const lines = createInterface({ input: server.stdout });
  let trace = '';
  server.stderr.on('data', (bytes) => {
    trace += bytes;
  });
  server.stdout.on('data', (bytes) => {
    trace += bytes;
  });
  try {
    let base;
    for await (const line of lines) {
      const match = line.match(/http:\/\/127\.0\.0\.1:\d+/);
      if (match) {
        base = match[0] + '/pub/';
        break;
      }
    }
    assert.ok(base, trace);
    await run(process.argv[2], [base, output, '', 'reject', 'success', 'none']);
    assert.doesNotMatch(trace, /GET \/pub\/untouched/);
    console.log('PASS published http-server directory listing');
  } finally {
    server.kill();
    await completion;
    lines.close();
  }
} finally {
  await rm(directory, { recursive: true, force: true });
}
