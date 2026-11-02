// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { runSharedGtkTest, expectElementKind } from './test-helpers.ts';

it('validates an inherited HTTP scope, saves a start page override and renders Japanese controls', async (context) => {
  const temporary = await mkdtemp(join(tmpdir(), 'elder-http-settings-ui-'));
  const config = join(temporary, 'http.ini');
  try {
    await runSharedGtkTest(
      context,
      [
        '--page=http',
        '--type=http',
        `--save-file=${config}`,
        '--global=http.base_url=https://example.com/pub/',
        '--connection=http.start_url=index.html',
      ],
      async ({ app, directory }) => {
        const start = expectElementKind(
          await app.getById('settings_http_start_url_entry'),
          'entry'
        );
        const save = expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        );
        const preview = expectElementKind(
          await app.getById('settings_http_url_label'),
          'label'
        );
        expect(await preview.text()).toBe('https://example.com/pub/index.html');
        await start.setText('../outside.html');
        expect((await save.info()).states).not.toContain('sensitive');
        await start.setText('downloads.html');
        expect((await save.info()).states).toContain('sensitive');
        expect(await preview.text()).toBe(
          'https://example.com/pub/downloads.html'
        );
        await writeFile(
          join(directory, 'http-settings-ja.png'),
          (await app.capture()).image
        );
        await save.click();
        await waitForResult(async () => {
          expect(await readFile(config, 'utf8')).toContain(
            'start_url=downloads.html'
          );
        });
      },
      {
        env: {
          LANGUAGE: 'ja',
          LC_ALL: 'ja_JP.UTF-8',
          ELDER_TERMS_LOCALE_DIR: fileURLToPath(
            new URL('../../.build/po/', import.meta.url)
          ),
        },
      }
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
