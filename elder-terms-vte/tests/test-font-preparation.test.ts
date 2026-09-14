import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';
import {
  prepareTestFonts,
  type TestFontSource,
} from '../../scripts/test-fonts.mjs';

const execute = promisify(execFile);

for (const mode of [
  'installed',
  'missing',
  'offline-cache',
  'damaged-cache',
  'http-error',
  'checksum-error',
  'wrong-family',
  'deb',
] as const) {
  it(`prepares fonts in a private Fontconfig environment: ${mode}`, async () => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-font-preparation-'));
    const base = join(directory, 'base & fonts');
    const output = join(directory, 'prepared & fonts');
    await mkdir(base);
    const baseConfig = join(directory, 'base.conf');
    await writeFile(
      baseConfig,
      `<fontconfig><dir>${base.replace(/&/g, '&amp;')}</dir><cachedir>${directory}/base-cache</cachedir></fontconfig>`
    );
    const available = await execute('fc-match', [
      '-f',
      '%{file}',
      'DejaVu Sans Mono',
    ]);
    const font = await readFile(available.stdout);
    const license = Buffer.from('Test server license fixture\n');
    let payload = font;
    let requests = 0;
    let offline = false;
    if (mode === 'installed')
      await symlink(available.stdout, join(base, 'installed.ttf'));
    if (mode === 'deb') {
      const packageRoot = join(directory, 'package');
      await mkdir(join(packageRoot, 'DEBIAN'), { recursive: true });
      await mkdir(join(packageRoot, 'usr/share/fonts'), { recursive: true });
      await mkdir(join(packageRoot, 'usr/share/doc/font-fixture'), {
        recursive: true,
      });
      await writeFile(
        join(packageRoot, 'DEBIAN/control'),
        'Package: font-fixture\nVersion: 1.0\nArchitecture: all\nMaintainer: Test <test@example.invalid>\nDescription: Font preparation fixture\n'
      );
      await writeFile(join(packageRoot, 'usr/share/fonts/test.ttf'), font);
      await writeFile(
        join(packageRoot, 'usr/share/doc/font-fixture/copyright'),
        license
      );
      await execute('dpkg-deb', [
        '--build',
        packageRoot,
        join(directory, 'font.deb'),
      ]);
      payload = await readFile(join(directory, 'font.deb'));
    }
    const server = createServer((request, response) => {
      requests++;
      if (offline || mode === 'http-error') {
        response.writeHead(503).end('Unavailable');
      } else if (mode === 'checksum-error') {
        response.end('Invalid font bytes');
      } else {
        response.end(request.url === '/license' ? license : payload);
      }
    });
    await new Promise<void>((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    try {
      const address = server.address();
      if (!address || typeof address === 'string')
        throw new Error('Missing server address');
      const url = `http://127.0.0.1:${address.port}`;
      const source: TestFontSource = {
        family: mode === 'wrong-family' ? 'Noto Sans Mono' : 'DejaVu Sans Mono',
        download: {
          url: `${url}/font`,
          sha256: createHash('sha256').update(payload).digest('hex'),
        },
        ...(mode === 'deb'
          ? {
              format: 'deb' as const,
              fontPath: 'usr/share/fonts/test.ttf',
              licensePath: 'usr/share/doc/font-fixture/copyright',
            }
          : {
              format: 'font' as const,
              filename: 'test.ttf',
              license: {
                url: `${url}/license`,
                sha256: createHash('sha256').update(license).digest('hex'),
              },
            }),
      };
      const options = { directory: output, baseConfig, sources: [source] };
      if (
        mode === 'http-error' ||
        mode === 'checksum-error' ||
        mode === 'wrong-family'
      ) {
        await expect(prepareTestFonts(options)).rejects.toThrow(
          mode === 'http-error'
            ? /DejaVu Sans Mono.*503/s
            : mode === 'checksum-error'
              ? /DejaVu Sans Mono.*SHA-256/s
              : /Noto Sans Mono.*family/s
        );
        return;
      }
      let config = await prepareTestFonts(options);
      const match = await execute(
        'fc-match',
        ['-f', '%{family}\n%{file}', source.family],
        {
          env: { ...process.env, FONTCONFIG_FILE: config },
        }
      );
      const [family, path] = match.stdout.split('\n');
      expect(family).toBe(source.family);
      expect(requests).toBe(mode === 'installed' ? 0 : mode === 'deb' ? 1 : 2);
      if (mode !== 'installed') expect(path.startsWith(output)).toBe(true);
      if (mode === 'offline-cache' || mode === 'damaged-cache') {
        const previousRequests = requests;
        if (mode === 'damaged-cache') await writeFile(path, 'Damaged cache');
        else offline = true;
        config = await prepareTestFonts(options);
        const restored = await execute(
          'fc-match',
          ['-f', '%{family}\n%{file}', source.family],
          {
            env: { ...process.env, FONTCONFIG_FILE: config },
          }
        );
        expect(restored.stdout.split('\n')[0]).toBe(source.family);
        expect(await readFile(restored.stdout.split('\n')[1])).toEqual(font);
        expect(requests).toBe(
          mode === 'offline-cache' ? previousRequests : previousRequests + 2
        );
      }
      expect(await readFile(baseConfig, 'utf8')).not.toContain(output);
    } finally {
      await new Promise<void>((resolve, reject) =>
        server.close((error) => (error ? reject(error) : resolve()))
      );
      await rm(directory, { recursive: true, force: true });
    }
  });
}
