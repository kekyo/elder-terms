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
  const requests: { method: string; url: string; authenticated: boolean }[] =
    [];
  const md5 = (value: string) => createHash('md5').update(value).digest('hex');
  const realm = 'elder-terms DAV tests';
  const nonce = '0123456789abcdef';
  let announceHeld: () => void = () => {};
  const held = new Promise<void>((resolve) => {
    announceHeld = resolve;
  });
  const listener: RequestListener = (request, response) => {
    const method = request.method ?? '';
    const url = request.url ?? '';
    const authorization = request.headers.authorization ?? '';
    let authenticated = authentication === 'none' && authorization === '';
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
      authenticated =
        fields.username === 'alice' &&
        fields.realm === realm &&
        fields.nonce === nonce &&
        fields.uri === url &&
        fields.qop === 'auth' &&
        fields.response ===
          md5(
            `${md5(`alice:${realm}:secret`)}:${nonce}:${fields.nc}:${fields.cnonce}:auth:${md5(`${method}:${url}`)}`
          );
    }
    requests.push({ method, url, authenticated });
    request.resume();
    if (!authenticated) {
      response.writeHead(401, {
        'WWW-Authenticate':
          authentication === 'digest'
            ? `Digest realm="${realm}", nonce="${nonce}", algorithm=MD5, qop="auth"`
            : `Basic realm="${realm}"`,
      });
      response.end();
      return;
    }
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
    const collectionPath = url.replace(/\/+$/, '');
    if (
      method !== 'PROPFIND' ||
      !['/dav', '/dav/nested'].includes(collectionPath)
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
    const children =
      request.headers.depth === '0'
        ? ''
        : collectionPath === '/dav/nested'
          ? entry('/dav/nested/child.txt', false, 6)
          : entry('/dav/hello.txt', false, 12) +
            entry(`/dav/${encodeURIComponent('資料 #+%.txt')}`, false, 0) +
            entry('/dav/unknown.txt', false, undefined) +
            entry('/dav/nested/', true, undefined);
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
    held,
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
