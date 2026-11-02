// Native HTTP crawler integration tests: record actual requests, never mock URLs.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { once } from 'node:events';

const requests = [];
const server = createServer((request, response) => {
  requests.push(`${request.method} ${request.url}`);
  const origin = `http://${request.headers.host}`;
  const pages = {
    '/pub/': `<a href="child">child</a><a href="${origin}/pub/child#again">duplicate</a>
      <a href="//${request.headers.host}/pub/deep/file?a=1&amp;b=2#part">deep</a>
      <a href="../outside">outside</a><a href="/public/file">boundary</a>
      <a href="http://localhost:9/pub/file">host</a><a href="https://${request.headers.host}/pub/file">scheme</a>
      <a href="#self">self</a><a href="?order=2&amp;sort=1">query</a>
      <a href="./x/../safe%25%3f%23">encoded</a><a href="%2e%2e/outside">encoded parent</a>
      <a href="bad%2fpart">separator</a><a href="bad%5cpart">backslash</a><a href="bad%00part">nul</a>
      <a href="javascript:alert(1)">script</a><a href="mailto:a@b">mail</a>
      <a href="%82%A0.txt">old URL</a><a href="日本語.txt">Unicode</a>`,
    '/pub/child': '<a href="file.bin">one file</a><a href="child#self">self</a>',
    '/pub/base': '<base href="deep/"><a href="file.bin">one</a><a href="../../outside">outside</a>',
    '/pub/foreign-base': '<base href="http://localhost:9/"><a href="file.bin">outside</a>',
    '/pub/attachment': '<a href="file.bin">download HTML</a>',
    '/pub/no-type': '<a href="file.bin">no media type</a>',
    '/pub/xhtml': '<html xmlns="http://www.w3.org/1999/xhtml"><body><a href="file.bin">one</a></body></html>',
    '/pub/sjis': Buffer.from('<meta charset="utf-8"><a href="', 'ascii'),
    '/pub/meta': Buffer.concat([Buffer.from('<meta charset="Shift_JIS"><a href="'), Buffer.from([0x82, 0xa0]), Buffer.from('.txt">one</a>')]),
    '/pub/bom': Buffer.from('\ufeff<meta charset="Shift_JIS"><a href="あ.txt">one</a>'),
    '/pub/limit': 'x'.repeat(2 * 1024 * 1024 + 1),
    '/pub/too-many': '<a href="file">one</a>'.repeat(10001),
  };
  pages['/pub/sjis'] = Buffer.concat([pages['/pub/sjis'], Buffer.from([0x82, 0xa0]), Buffer.from('.txt">one</a><a href="%82%A0.txt">two</a>')]);
  if (request.url === '/pub/redirect') {
    response.writeHead(302, { Location: 'child' }); response.end();
  } else if (request.url === '/pub/escape') {
    response.writeHead(302, { Location: '../outside' }); response.end();
  } else if (request.url === '/pub/loop') {
    response.writeHead(302, { Location: 'loop#again' }); response.end();
  } else if (request.url === '/pub/huge.html' || request.url === '/pub/trailing/') {
    response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': String(1024 ** 3) });
    response.flushHeaders(); // No body: successful classification must finish at headers.
  } else if (request.url in pages) {
    const type = request.url === '/pub/xhtml' ? 'application/xhtml+xml'
      : request.url === '/pub/sjis' || request.url === '/pub/bom' ? 'text/html; charset=Shift_JIS' : 'TeXt/HtMl; charset=UTF-8';
    if (request.url !== '/pub/no-type' && request.url !== '/pub/meta') response.setHeader('Content-Type', type);
    if (request.url === '/pub/meta') response.setHeader('Content-Type', 'text/html');
    if (request.url === '/pub/attachment') response.setHeader('Content-Disposition', 'attachment; filename="../../unsafe.html"');
    response.end(pages[request.url]);
  } else {
    response.writeHead(404, { 'Content-Type': 'text/html' }); response.end('<a href="child">error</a>');
  }
});
server.listen(0, '127.0.0.1');
await once(server, 'listening');
const origin = `http://127.0.0.1:${server.address().port}`;
const run = async (pages, success = true, scope = `${origin}/pub/`) => {
  requests.length = 0;
  const child = spawn(process.argv[2], [scope, ...pages], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '', error = '';
  child.stdout.on('data', (data) => { output += data; });
  child.stderr.on('data', (data) => { error += data; });
  const [code] = await once(child, 'close');
  assert.equal(code === 0, success, error || output);
  return output.trim().split('\n').map((line) => line.split('\t'));
};
try {
  const rows = await run(['']);
  assert.equal(rows[0][0], 'html');
  assert.deepEqual(requests, ['GET /pub/']);
  assert.deepEqual(rows.slice(1).map((row) => row[1]), [
    '/pub/child', '/pub/deep/file?a=1&b=2', '/pub/?order=2&sort=1', '/pub/safe%25%3F%23', '/pub/%82%A0.txt', '/pub/%E6%97%A5%E6%9C%AC%E8%AA%9E.txt',
  ].map((path) => origin + path));
  const child = await run(['', 'child']);
  assert.deepEqual(requests, ['GET /pub/', 'GET /pub/child']);
  assert.equal(child.at(-1)[1], origin + '/pub/file.bin');
  assert.equal((await run(['base']))[1][1], origin + '/pub/deep/file.bin');
  assert.equal((await run(['foreign-base'])).length, 1);
  for (const path of ['huge.html', 'trailing/', 'attachment', 'no-type'])
    assert.equal((await run([path]))[0][0], 'content');
  assert.equal((await run(['xhtml']))[1][1], origin + '/pub/file.bin');
  for (const path of ['sjis', 'meta', 'bom']) {
    const encoded = await run([path]);
    assert.equal(encoded[1][1], origin + '/pub/%E3%81%82.txt');
    if (path === 'sjis') assert.equal(encoded[2][1], origin + '/pub/%82%A0.txt');
  }
  await run(['redirect']);
  assert.deepEqual(requests, ['GET /pub/redirect', 'GET /pub/child']);
  for (const path of ['escape', 'loop', 'missing', 'limit', 'too-many']) {
    await run([path], false);
    assert.deepEqual(requests, [`GET /pub/${path}`]);
  }
  for (const scope of [`${origin}/pub/index.html`, `${origin}/pub/?x=1`, `${origin.replace('://', '://user:pass@')}/pub/`]) {
    await run([''], false, scope);
    assert.deepEqual(requests, []);
  }
  console.log('HTTP crawler scope, lazy GET, classification and encoding: PASS');
} finally {
  server.closeAllConnections();
  server.close();
  await once(server, 'close');
}
