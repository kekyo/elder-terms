// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const requests = [];
const server = createServer((request, response) => {
  requests.push(`${request.method} ${request.url}`);
  const pages = {
    '/pub/': '<a href="tree/">selected</a><a href="untouched/">untouched</a>',
    '/pub/tree/':
      '<a href="unknown.bin">unknown</a><a href="nested/">nested</a><a href="same.bin?a=1">one</a><a href="same.bin?a=2">two</a><a href="%82%A0.txt">old name</a><a href="../">parent</a>',
    '/pub/tree/nested/':
      '<a href="file.bin">file</a><a href="../">cycle</a><a href="../unknown.bin">duplicate</a>',
    '/pub/document.html': '<a href="untouched/">original</a>',
  };
  if (request.url in pages) {
    response.setHeader('Content-Type', 'text/html');
    response.end(pages[request.url]);
  } else if (request.url.startsWith('/pub/infinite?')) {
    response.setHeader('Content-Type', 'text/html');
    response.end(
      `<a href="?n=${Number(new URL(request.url, 'http://localhost').searchParams.get('n')) + 1}">next</a>`
    );
  } else if (request.url === '/pub/misconfigured') {
    response.end('<a href="document.html">document</a>');
  } else if (request.url === '/pub/broken') {
    response.writeHead(200, { 'Content-Length': '100000' });
    response.write('partial');
    setImmediate(() => response.destroy());
  } else if (request.url === '/pub/slow') {
    response.writeHead(200, { 'Content-Type': 'application/octet-stream' });
    response.write(Buffer.alloc(65536, 42));
  } else if (request.url === '/pub/escape' || request.url === '/pub/loop') {
    response.writeHead(302, {
      Location: request.url.endsWith('escape') ? '/outside' : 'loop',
    });
    response.end();
  } else if (
    ['/pub/denied', '/pub/missing', '/pub/error'].includes(request.url)
  ) {
    response.writeHead(
      request.url.endsWith('denied')
        ? 403
        : request.url.endsWith('error')
          ? 500
          : 404
    );
    response.end();
  } else {
    response.writeHead(200, { 'Content-Type': 'application/octet-stream' });
    response.write(
      request.url.includes('unknown.bin')
        ? 'unknown bytes'
        : request.url.includes('file.bin')
          ? 'nested bytes'
          : request.url.includes('%82%A0')
            ? 'encoded bytes'
            : request.url
    );
    response.end();
  }
});
server.listen(0, '127.0.0.1');
await once(server, 'listening');
const directory = await mkdtemp(join(tmpdir(), 'elder-http-'));
try {
  const child = spawn(
    process.argv[2],
    [`http://127.0.0.1:${server.address().port}/pub/`, directory],
    { stdio: 'inherit' }
  );
  const [code] = await once(child, 'close');
  assert.equal(code, 0);
  assert.ok(
    requests.every(
      (request) =>
        request.startsWith('GET /pub/') && !request.includes('untouched')
    )
  );
  assert.equal(requests.filter((request) => request === 'GET /pub/').length, 3);
  assert.equal(
    requests.filter((request) => request.startsWith('GET /pub/infinite?'))
      .length,
    3
  );
  assert.equal(
    requests.filter((request) => request === 'GET /pub/document.html').length,
    1
  );
  for (const path of [
    'tree/',
    'tree/unknown.bin',
    'tree/nested/',
    'tree/nested/file.bin',
    'tree/same.bin?a=1',
    'tree/same.bin?a=2',
    'tree/%82%A0.txt',
  ])
    assert.equal(
      requests.filter((request) => request === 'GET /pub/' + path).length,
      1,
      path
    );
  assert.ok(
    requests.indexOf('GET /pub/tree/unknown.bin') <
      requests.indexOf('GET /pub/tree/nested/')
  );
  console.log('HTTP exact requests, no HEAD or unrelated branches: PASS');
} finally {
  server.closeAllConnections();
  server.close();
  await once(server, 'close');
  await rm(directory, { recursive: true, force: true });
}
