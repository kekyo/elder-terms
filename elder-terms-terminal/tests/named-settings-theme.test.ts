// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { expect, it } from 'vitest';
import { toPass } from 'gestament/testing';
import {
  withTemporaryDirectory,
  capturePixel,
} from '../../shared/tests/connection-test-helpers.ts';
import { runGtkTest } from './gtk-test-helpers.ts';

it('renders a named theme titlebar and background from separate configuration files', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const configDirectory = join(directory, 'elder-terms');
    await mkdir(configDirectory);
    await writeFile(
      join(configDirectory, 'global.ini'),
      '[general]\nbackground=#111111\n'
    );
    await writeFile(
      join(configDirectory, 'theme.ini'),
      '[theme.red]\nname=赤テーマ\n[theme.red.general]\nexterior_background=#7A2222\nbackground=#281818\n'
    );
    const connection = join(directory, 'connection.ini');
    await writeFile(connection, '[general]\ntheme=red\nname=Named red theme\n');
    await runGtkTest(
      context,
      ['-c', connection],
      async (app, evidence) => {
        await toPass(async () => {
          expect(
            capturePixel(
              await (await app.getById('header_bar')).capture(),
              0.5,
              0.1
            )
          ).toEqual([0x7a, 0x22, 0x22]);
          expect(
            capturePixel(
              await (await app.getById('terminal_view')).capture(),
              0.9,
              0.9
            )
          ).toEqual([0x28, 0x18, 0x18]);
        });
        await evidence.captureEvidence('named-red-theme', async () =>
          (await app.getById('main_window')).capture()
        );
      },
      { env: { XDG_CONFIG_HOME: directory } }
    );
  });
});
