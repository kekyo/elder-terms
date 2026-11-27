// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { createHash } from 'node:crypto';
import { expect, it } from 'vitest';
import { createWebdavTestServer } from './webdav-test-server.ts';

it('round-trips file and collection names containing URI delimiters in DAV XML', async () => {
  const server = await createWebdavTestServer('none');
  const base = `http://127.0.0.1:${server.port}/dav/`;
  try {
    expect((await fetch(base + 'R&D', { method: 'MKCOL' })).status).toBe(201);
    const file = base + 'R&D/notes&plans.txt';
    expect((await fetch(file, { method: 'PUT', body: 'notes' })).status).toBe(
      201
    );
    const listing = await fetch(base + 'R&D/', {
      method: 'PROPFIND',
      headers: { Depth: '1' },
    });
    expect(await listing.text()).toContain('<d:href>/dav/R%26D/</d:href>');
    const metadata = await fetch(file, { method: 'PROPFIND' });
    expect(await metadata.text()).toContain(
      '<d:href>/dav/R%26D/notes%26plans.txt</d:href>'
    );
    const content = await fetch(file);
    expect(content.headers.get('content-type')).toBe(
      'application/octet-stream'
    );
    expect(content.headers.get('x-content-type-options')).toBe('nosniff');
    expect(await content.text()).toBe('notes');
  } finally {
    await server.close();
  }
});

it('parses complete Digest parameters including whitespace and quoted commas', async () => {
  const server = await createWebdavTestServer('digest');
  const uri = '/dav/hello.txt';
  const url = `http://127.0.0.1:${server.port}${uri}`;
  try {
    const challenge = await fetch(url);
    expect(challenge.status).toBe(401);
    const nonce = /nonce="([^"]+)"/.exec(
      challenge.headers.get('www-authenticate') ?? ''
    )?.[1];
    expect(nonce).toBeTruthy();
    const realm = 'elder-terms DAV tests';
    const nc = '00000001';
    const cnonce = 'client,one';
    const md5 = (value: string) =>
      createHash('md5').update(value).digest('hex');
    const digest = md5(
      `${md5('alice:' + realm + ':secret')}:${nonce}:${nc}:${cnonce}:auth:${md5('GET:' + uri)}`
    );
    const authorization = `Digest username = "alice", realm="${realm}", nonce="${nonce}", uri="${uri}", qop=auth, nc=${nc}, cnonce="${cnonce}", response="${digest}"`;
    expect((await fetch(url, { headers: { authorization } })).status).toBe(200);
    for (const suffix of [', nc=00000001', ', unfinished']) {
      expect(
        (
          await fetch(url, {
            headers: { authorization: authorization + suffix },
          })
        ).status
      ).toBe(401);
    }
  } finally {
    await server.close();
  }
});
