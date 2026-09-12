import { createHash } from 'node:crypto';
import { createServer, type RequestListener } from 'node:http';
import { createServer as createSecureServer } from 'node:https';

/** Authentication mechanisms supported by the independent HTTP fixture. */
export type WebdavTestAuthentication = 'none' | 'basic' | 'digest';

/**
 * Starts a real DAV endpoint with namespace-qualified multistatus responses.
 * @param authentication Required mechanism; credentials are alice/secret.
 * @param tls Optional PEM certificate and key for HTTPS.
 * @returns Ephemeral port, request observations, a held-request event and cleanup.
 */
export const createWebdavTestServer = async (
  authentication: WebdavTestAuthentication,
  tls?: { cert: string; key: string }
) => {
  const requests: {
    method: string;
    url: string;
    authenticated: boolean;
    destination: string | undefined;
    overwrite: string | undefined;
    contentLength: string | undefined;
    receivedBytes: number;
  }[] = [];
  const md5 = (value: string) => createHash('md5').update(value).digest('hex');
  const realm = 'elder-terms DAV tests';
  let nonce = '0123456789abcdef';
  const renewedUploads = new Set<string>();
  let announceHeld: () => void = () => {};
  const held = new Promise<void>((resolve) => {
    announceHeld = resolve;
  });
  let announceUploadHeld: () => void = () => {};
  const uploadHeld = new Promise<void>((resolve) => {
    announceUploadHeld = resolve;
  });
  let announceUploadBodyHeld: () => void = () => {};
  const uploadBodyHeld = new Promise<void>((resolve) => {
    announceUploadBodyHeld = resolve;
  });
  const files = new Map<
    string,
    { content: Buffer; reportedSize: number | undefined }
  >([
    [
      '/dav/hello.txt',
      { content: Buffer.from('Hello DAV!\r\n'), reportedSize: 12 },
    ],
    ['/dav/資料 #+%.txt', { content: Buffer.alloc(0), reportedSize: 0 }],
    [
      '/dav/unknown.txt',
      { content: Buffer.from('unknown length\n'), reportedSize: undefined },
    ],
    [
      '/dav/nested/child.txt',
      { content: Buffer.from('child\n'), reportedSize: 6 },
    ],
  ]);
  const directories = new Set(['/dav', '/dav/nested']);
  const largeSize = 40 * 1024 * 1024 + 13;
  const listener: RequestListener = (request, response) => {
    const method = request.method ?? '';
    const url = request.url ?? '';
    const authorization = request.headers.authorization ?? '';
    let authenticated = authentication === 'none' && authorization === '';
    let staleDigest = false;
    if (authentication === 'basic') {
      authenticated =
        authorization ===
        `Basic ${Buffer.from('alice:secret').toString('base64')}`;
    } else if (
      authentication === 'digest' &&
      authorization.startsWith('Digest ')
    ) {
      const fields = Object.fromEntries(
        [
          ...authorization.slice(7).matchAll(/(\w+)=(?:"([^"]*)"|([^,\s]+))/g),
        ].map((match) => [match[1], match[2] ?? match[3]])
      );
      const validCredentials =
        fields.username === 'alice' &&
        fields.realm === realm &&
        typeof fields.nonce === 'string' &&
        fields.uri === url &&
        fields.qop === 'auth' &&
        fields.response ===
          md5(
            `${md5(`alice:${realm}:secret`)}:${fields.nonce}:${fields.nc}:${fields.cnonce}:auth:${md5(`${method}:${url}`)}`
          );
      authenticated = validCredentials && fields.nonce === nonce;
      // RFC 7616: a valid response to an expired nonce is not a bad password.
      staleDigest = validCredentials && fields.nonce !== nonce;
    }
    const observation = {
      method,
      url,
      authenticated,
      destination:
        typeof request.headers.destination === 'string'
          ? request.headers.destination
          : undefined,
      overwrite:
        typeof request.headers.overwrite === 'string'
          ? request.headers.overwrite
          : undefined,
      contentLength: request.headers['content-length'],
      receivedBytes: 0,
    };
    requests.push(observation);
    request.resume();
    if (!authenticated) {
      response.writeHead(401, {
        'WWW-Authenticate':
          authentication === 'digest'
            ? `Digest realm="${realm}", nonce="${nonce}", algorithm=MD5, qop="auth"${staleDigest ? ', stale=true' : ''}`
            : `Basic realm="${realm}"`,
      });
      response.end();
      return;
    }
    if (url === '/dav/unsupported-auth') {
      response.writeHead(401, {
        'WWW-Authenticate': ['Negotiate', 'NTLM', 'Bearer realm="Basic login"'],
      });
      response.end();
      return;
    }
    if (url === '/dav/idle') return;
    if (url === '/dav/hold') {
      announceHeld();
      return;
    }
    if (url === '/dav/denied') {
      response.writeHead(403);
      response.end();
      return;
    }
    if (url === '/dav/not-dav' || url === '/dav/malformed') {
      response.writeHead(url === '/dav/not-dav' ? 200 : 207);
      response.end('<html>Not a DAV collection</html>');
      return;
    }
    if (
      ['/dav/redirect', '/dav/outside', '/dav/cross', '/dav/loop'].includes(url)
    ) {
      const bound = server.address();
      response.writeHead(307, {
        Location:
          url === '/dav/redirect'
            ? '/dav/'
            : url === '/dav/outside'
              ? '/outside/'
              : url === '/dav/loop'
                ? url
                : `http://localhost:${bound !== null && typeof bound !== 'string' ? bound.port : 0}/dav/`,
      });
      response.end();
      return;
    }
    let decodedPath: string;
    try {
      decodedPath = decodeURIComponent(url);
    } catch {
      response.writeHead(400);
      response.end();
      return;
    }
    const resourcePath = decodedPath.replace(/\/+$/, '');
    const parentPath = resourcePath.slice(0, resourcePath.lastIndexOf('/'));
    const status = (code: number) => {
      response.writeHead(code);
      response.end();
    };
    if (['PUT', 'MKCOL', 'MOVE', 'DELETE'].includes(method)) {
      if (!resourcePath.startsWith('/dav/') || resourcePath === '/dav') {
        status(403);
        return;
      }
      if (method === 'PUT') {
        if (resourcePath === '/dav/body-held.bin') {
          request.pause();
          announceUploadBodyHeld();
          return;
        }
        if (resourcePath.includes('/redirect-write.txt.')) {
          response.writeHead(307, { Location: '/dav/redirected.txt' });
          response.end();
          return;
        }
        if (resourcePath.includes('/put-collision.txt.'))
          files.set(resourcePath, {
            content: Buffer.from('foreign temporary file'),
            reportedSize: 22,
          });
        if (
          request.headers['if-none-match'] === '*' &&
          (files.has(resourcePath) || directories.has(resourcePath))
        ) {
          status(412);
          return;
        }
        if (!directories.has(parentPath)) {
          status(409);
          return;
        }
        if (directories.has(resourcePath)) {
          status(405);
          return;
        }
        const chunks: Buffer[] = [];
        request.on('data', (chunk: Buffer) => {
          observation.receivedBytes += chunk.length;
          chunks.push(Buffer.from(chunk));
        });
        request.on('end', () => {
          const renewal = resourcePath.includes('/rewind-small.txt.')
            ? 'small'
            : resourcePath.includes('/rewind.txt.')
              ? 'large'
              : undefined;
          if (
            renewal !== undefined &&
            authentication === 'digest' &&
            !renewedUploads.has(renewal)
          ) {
            renewedUploads.add(renewal);
            nonce = md5('renewed-' + renewal);
            response.writeHead(401, {
              'WWW-Authenticate': `Digest realm="${realm}", nonce="${nonce}", algorithm=MD5, qop="auth", stale=true`,
            });
            response.end();
            return;
          }
          const content = Buffer.concat(chunks);
          const exists = files.has(resourcePath);
          files.set(resourcePath, { content, reportedSize: content.length });
          if (resourcePath.includes('/cancel-upload.txt.')) {
            announceUploadHeld();
            return;
          }
          if (resourcePath.includes('/put-drop.txt.')) {
            response.destroy();
            return;
          }
          status(exists ? 204 : 201);
        });
      } else if (method === 'MKCOL') {
        if (files.has(resourcePath) || directories.has(resourcePath)) {
          status(405);
          return;
        }
        if (!directories.has(parentPath)) {
          status(409);
          return;
        }
        directories.add(resourcePath);
        status(201);
      } else if (method === 'MOVE') {
        let destination: URL;
        try {
          destination = new URL(observation.destination ?? '');
        } catch {
          status(400);
          return;
        }
        const target = decodeURIComponent(destination.pathname).replace(
          /\/+$/,
          ''
        );
        if (
          destination.host !== request.headers.host ||
          !target.startsWith('/dav/')
        ) {
          status(400);
          return;
        }
        if (target.endsWith('/cleanup-denied.txt')) {
          status(403);
          return;
        }
        if (!files.has(resourcePath) && !directories.has(resourcePath)) {
          status(404);
          return;
        }
        const exists = files.has(target) || directories.has(target);
        if (exists && observation.overwrite !== 'T') {
          status(412);
          return;
        }
        if (!directories.has(target.slice(0, target.lastIndexOf('/')))) {
          status(409);
          return;
        }
        if (
          directories.has(target) !== directories.has(resourcePath) &&
          exists
        ) {
          status(409);
          return;
        }
        for (const path of [...files.keys()])
          if (path === target || path.startsWith(target + '/'))
            files.delete(path);
        for (const path of [...directories])
          if (path === target || path.startsWith(target + '/'))
            directories.delete(path);
        for (const [path, value] of [...files]) {
          if (path === resourcePath || path.startsWith(resourcePath + '/')) {
            files.delete(path);
            files.set(target + path.slice(resourcePath.length), value);
          }
        }
        for (const path of [...directories]) {
          if (path === resourcePath || path.startsWith(resourcePath + '/')) {
            directories.delete(path);
            directories.add(target + path.slice(resourcePath.length));
          }
        }
        if (target.endsWith('/move-drop.txt')) {
          response.destroy();
          return;
        }
        status(exists ? 204 : 201);
      } else {
        if (resourcePath.includes('/cleanup-denied.txt.')) {
          status(403);
          return;
        }
        if (resourcePath.endsWith('/partial')) {
          response.writeHead(207, { 'Content-Type': 'application/xml' });
          response.end(
            '<d:multistatus xmlns:d="DAV:"><d:response><d:href>/dav/uploaded/partial/blocked.txt</d:href><d:status>HTTP/1.1 403 Forbidden</d:status></d:response></d:multistatus>'
          );
          return;
        }
        if (!files.has(resourcePath) && !directories.has(resourcePath)) {
          status(404);
          return;
        }
        for (const path of [...files.keys()])
          if (path === resourcePath || path.startsWith(resourcePath + '/'))
            files.delete(path);
        for (const path of [...directories])
          if (path === resourcePath || path.startsWith(resourcePath + '/'))
            directories.delete(path);
        if (resourcePath.endsWith('/delete-drop.txt')) {
          response.destroy();
          return;
        }
        status(204);
      }
      return;
    }
    const file = files.get(decodedPath);
    if (method === 'GET') {
      if (decodedPath === '/dav/get-redirect') {
        response.writeHead(307, { Location: '/dav/hello.txt' });
        response.end('redirect body');
        return;
      }
      if (decodedPath === '/dav/get-outside') {
        response.writeHead(302, { Location: '/outside/hello.txt' });
        response.end();
        return;
      }
      if (decodedPath === '/dav/partial.bin') {
        response.writeHead(206, {
          'Content-Range': 'bytes 0-3/16',
          'Content-Length': 4,
        });
        response.end('part');
        return;
      }
      if (file !== undefined) {
        response.writeHead(
          200,
          file.reportedSize === undefined
            ? {}
            : { 'Content-Length': file.content.length }
        );
        if (file.reportedSize === undefined) response.write(file.content);
        response.end(
          file.reportedSize === undefined ? undefined : file.content
        );
      } else if (decodedPath === '/dav/large.bin') {
        response.writeHead(200, { 'Content-Length': largeSize });
        const chunk = Buffer.alloc(64 * 1024);
        for (let index = 0; index < chunk.length; index += 1)
          chunk[index] = (index * 31 + 7) & 255;
        let remaining = largeSize;
        const pump = () => {
          while (!response.destroyed && remaining > 0) {
            const count = Math.min(remaining, chunk.length);
            remaining -= count;
            if (!response.write(chunk.subarray(0, count))) {
              response.once('drain', pump);
              return;
            }
          }
          if (!response.destroyed) response.end();
        };
        pump();
      } else if (decodedPath === '/dav/truncated.bin') {
        response.writeHead(200, { 'Content-Length': 262144 });
        response.write(Buffer.alloc(1024, 23), () => response.destroy());
      } else if (decodedPath === '/dav/held.bin') {
        response.writeHead(200, { 'Content-Length': 1048576 });
        response.write(Buffer.alloc(1024, 42));
      } else {
        response.writeHead(404);
        response.end();
      }
      return;
    }
    const collectionPath = url.replace(/\/+$/, '');
    if (
      method !== 'PROPFIND' ||
      (!directories.has(resourcePath) &&
        file === undefined &&
        !['/dav/large.bin', '/dav/truncated.bin', '/dav/held.bin'].includes(
          decodedPath
        ))
    ) {
      response.writeHead(404);
      response.end();
      return;
    }
    const entry = (
      href: string,
      collection: boolean,
      size: number | undefined
    ) =>
      `<d:response><d:href>${href}</d:href><d:propstat><d:prop>` +
      `<d:displayname>Not the resource identity</d:displayname>` +
      `<d:resourcetype>${collection ? '<d:collection/>' : ''}</d:resourcetype>` +
      (size === undefined
        ? ''
        : `<d:getcontentlength>${size}</d:getcontentlength>`) +
      `<d:getlastmodified>Wed, 01 Jan 2025 00:00:00 GMT</d:getlastmodified>` +
      `</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>`;
    if (
      file !== undefined ||
      ['/dav/large.bin', '/dav/truncated.bin', '/dav/held.bin'].includes(
        decodedPath
      )
    ) {
      const size =
        file !== undefined
          ? file.reportedSize
          : decodedPath === '/dav/large.bin'
            ? largeSize
            : decodedPath === '/dav/held.bin'
              ? 1048576
              : 262144;
      response.writeHead(207, {
        'Content-Type': 'application/xml; charset=utf-8',
      });
      response.end(
        '<?xml version="1.0"?><d:multistatus xmlns:d="DAV:">' +
          entry(url, false, size) +
          '</d:multistatus>'
      );
      return;
    }
    let children = '';
    if (request.headers.depth !== '0') {
      const href = (path: string) =>
        path
          .split('/')
          .map((part) => encodeURIComponent(part))
          .join('/');
      for (const [path, value] of files)
        if (path.slice(0, path.lastIndexOf('/')) === resourcePath)
          children += entry(href(path), false, value.reportedSize);
      for (const path of directories)
        if (
          path !== resourcePath &&
          path.slice(0, path.lastIndexOf('/')) === resourcePath
        )
          children += entry(href(path) + '/', true, undefined);
    }
    response.writeHead(207, {
      'Content-Type': 'application/xml; charset=utf-8',
    });
    response.end(
      `<?xml version="1.0"?><d:multistatus xmlns:d="DAV:">${entry(`${collectionPath}/`, true, undefined)}${children}</d:multistatus>`
    );
  };
  const server =
    tls === undefined
      ? createServer(listener)
      : createSecureServer(tls, listener);
  await new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      server.removeListener('error', reject);
      resolve();
    });
  });
  const address = server.address();
  if (address === null || typeof address === 'string') {
    throw new Error('DAV server did not bind a TCP port');
  }
  return {
    port: address.port,
    requests,
    files,
    directories,
    held,
    uploadHeld,
    uploadBodyHeld,
    close: async () => {
      await new Promise<void>((resolve, reject) => {
        server.close((error) =>
          error === undefined ? resolve() : reject(error)
        );
        server.closeAllConnections();
      });
    },
  };
};
