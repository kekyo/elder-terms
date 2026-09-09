import { execFile } from 'node:child_process';
import { mkdir, readFile, symlink, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { createServer, type Socket } from 'node:net';
import { join } from 'node:path';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

const execute = promisify(execFile);
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

it('renders missing glyphs with the third ordered family and tolerates unavailable candidates', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const families = [
      'Noto Sans Mono',
      'DejaVu Sans Mono',
      'IPAGothic',
      'IBM Plex Sans JP',
    ];
    const fonts = join(directory, 'fonts');
    await mkdir(fonts);
    for (const [index, family] of families.entries()) {
      const match = await execute('fc-match', [
        '-f',
        '%{family}\n%{file}\n',
        family,
      ]);
      const [actual, path] = match.stdout.trim().split('\n');
      expect(
        actual.split(','),
        `Install the ${family} font to run this rendering test`
      ).toContain(family);
      await symlink(path, join(fonts, `${index}.ttf`));
      const query = await execute('fc-query', ['-f', '%{charset}', path]);
      const ranges = query.stdout
        .trim()
        .split(/\s+/)
        .map((range) =>
          range.split('-').map((part) => Number.parseInt(part, 16))
        );
      for (const glyph of '漢語永あ') {
        const code = glyph.codePointAt(0)!;
        const present = ranges.some(
          ([first, last]) => code >= first && code <= (last ?? first)
        );
        expect(present, `${family} coverage of ${glyph}`).toBe(index >= 2);
      }
    }
    // Only these four fonts participate; neither the host's configuration nor
    // its font cache is changed by this test.
    const fontConfig = join(directory, 'fonts.conf');
    await writeFile(
      fontConfig,
      `<?xml version="1.0"?><fontconfig><dir>${fonts}</dir><cachedir>${directory}/cache</cachedir></fontconfig>`
    );
    const sockets: Socket[] = [];
    const server = createServer((socket) => {
      sockets.push(socket);
      socket.on('error', () => {});
      socket.write('\x1b[?25l\x1b[2J\x1b[H漢語永あ\r\n');
    });
    try {
      await new Promise<void>((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', resolve);
      });
      const endpoint = server.address();
      if (endpoint === null || typeof endpoint === 'string')
        throw new Error('Missing TCP endpoint');
      const captures: Buffer[] = [];
      const lists = [
        [families[0], families[2]],
        [families[0], families[1], families[2], families[3]],
        [families[0], families[3]],
        [families[0], families[1], families[3], families[2]],
        [
          'Definitely Uninstalled Font Candidate',
          families[0],
          families[1],
          families[3],
          families[2],
        ],
      ];
      for (const [index, list] of lists.entries()) {
        const config = join(directory, `fonts-${index}.ini`);
        await writeFile(
          config,
          `[general]\ntype=telnet\nbackground=#000000\n[telnet]\naddress=127.0.0.1\nport=${endpoint.port}\n[terminal]\nauto_close=false\nfont_families=${list.join(';')};\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const terminal = await app.getById('terminal_view');
            let previous: Buffer | undefined;
            const capture = await waitForResult(async () => {
              const current = await terminal.capture();
              expect(current.clipped).toBe(false);
              const pixels = PNG.sync.read(current.image).data;
              const old = previous;
              previous = pixels;
              expect(
                pixels.filter(
                  (value, position) => position % 4 !== 3 && value > 40
                ).length
              ).toBeGreaterThan(100);
              expect(old).toBeDefined();
              expect(pixels.equals(old!)).toBe(true);
              return current;
            });
            await evidence.captureEvidence(
              `font-fallback-${index}`,
              async () => capture
            );
            const image = PNG.sync.read(capture.image);
            // All cases retain the same Latin primary face and terminal grid.
            captures.push(
              Buffer.concat([
                Buffer.from(`${image.width}x${image.height}:`),
                image.data,
              ])
            );
            if (index === 3) {
              const saved = await readFile(config, 'utf8');
              await expectElementKind(
                await app.getById('application_menu_button'),
                'toggleButton'
              ).click();
              await expectElementKind(
                await app.getById('settings_menu_item'),
                'menuItem'
              ).click();
              const notebook = expectElementKind(
                await app.getById('settings_notebook'),
                'tabList'
              );
              for (let tab = 0; tab < (await notebook.getChildCount()); tab++) {
                if (
                  (await (await notebook.childAt(tab))?.info())?.name ===
                  'Terminal'
                ) {
                  await notebook.selectChildAt(tab);
                  break;
                }
              }
              for (const id of ['settings_terminal_page_scrollbar']) {
                const scrollbar = expectElementKind(
                  await app.getById(id),
                  'scrollbar'
                );
                await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
              }
              await expectElementKind(
                await app.getById('settings_terminal_font_up_3'),
                'button'
              ).click();
              await expectElementKind(
                await app.getById('settings_apply_button'),
                'button'
              ).click();
              await waitForResult(async () => {
                expect(await app.findById('settings_dialog')).toBeUndefined();
              });
              const applied = await waitForResult(async () => {
                const current = await terminal.capture();
                const pixels = PNG.sync.read(current.image);
                const value = Buffer.concat([
                  Buffer.from(`${pixels.width}x${pixels.height}:`),
                  pixels.data,
                ]);
                expect(
                  value.equals(captures[0]),
                  'Runtime reordering must repaint existing glyphs with the new third family'
                ).toBe(true);
                return current;
              });
              await evidence.captureEvidence(
                'font-fallback-runtime-apply',
                async () => applied
              );
              expect(await readFile(config, 'utf8')).toBe(saved);
            }
          },
          { env: { FONTCONFIG_FILE: fontConfig } }
        );
      }
      expect(
        captures[0].equals(captures[2]),
        'The two CJK faces must render differently'
      ).toBe(false);
      expect(
        captures[1].equals(captures[0]),
        'Third family should supply the glyphs'
      ).toBe(true);
      expect(
        captures[3].equals(captures[2]),
        'Reordering CJK candidates must change the glyphs'
      ).toBe(true);
      expect(
        captures[4].equals(captures[3]),
        'Unavailable candidates must not hide later fonts'
      ).toBe(true);
    } finally {
      for (const socket of sockets) socket.destroy();
      if (server.listening)
        await new Promise<void>((resolve, reject) =>
          server.close((error) =>
            error === undefined ? resolve() : reject(error)
          )
        );
    }
  });
}, 120_000);
