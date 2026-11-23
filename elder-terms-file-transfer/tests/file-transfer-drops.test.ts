// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { expectElementKind } from '../../shared/tests/connection-test-helpers.ts';

it('copies a local file to the remote pane and drops it back into a folder', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'elder-pane-drop-'));
  const launcher = createGtkAppLauncher({
    appPath: fileURLToPath(
      new URL(
        '../../.build/elder-terms-file-transfer/elder-terms-file-transfer',
        import.meta.url
      )
    ),
    env: { LANGUAGE: 'en', LC_ALL: 'C.UTF-8', XDG_CONFIG_HOME: dir },
    xvfbTrayHost: false,
  });
  try {
    const file = 'drag 日本.txt';
    await mkdir(join(dir, 'received'));
    await writeFile(join(dir, file), 'round trip\n');
    const config = join(dir, 'connection.ini');
    await writeFile(
      config,
      `[general]\ntype=sftp\n[ssh]\naddress=fixture.example\n[sftp]\nremote_directory=/remote\nlocal_directory=${dir}\n`
    );
    const app = await launcher.launch(['--test-fixture', '-c', config]);
    const local = expectElementKind(
      await app.getById('file_transfer_local_tree'),
      'table'
    );
    const remote = expectElementKind(
      await app.getById('file_transfer_remote_tree'),
      'table'
    );
    const row = async (tree: GtkTableElement, name: string) =>
      await waitForResult(async () => {
        expect((await tree.info()).states).toContain('sensitive');
        for (let i = 0; i < (await tree.getRowCount()); i++) {
          const cell = await tree.cellAt(i, 0);
          if (cell && (await cell.info()).name === name)
            return (await cell.capture()).bounds;
        }
        throw Error('Missing file ' + name);
      });
    const drag = async (
      tree: GtkTableElement,
      dest: GtkTableElement,
      target: string
    ) => {
      const from = await row(tree, file);
      const to = await row(dest, target);
      const x = from.x + Math.floor(from.width / 2),
        y = from.y + Math.floor(from.height / 2);
      const tx = to.x + Math.floor(to.width / 2),
        ty = to.y + Math.floor(to.height / 2);
      await app.input.moveMouseTo(x, y);
      await app.input.setMouseButton('left', true);
      try {
        await app.input.moveMouseTo(x + 16, y);
        await app.input.moveMouseTo(tx - 12, ty);
        await app.input.moveMouseTo(tx, ty);
      } finally {
        await app.input.setMouseButton('left', false);
      }
    };
    await drag(local, remote, 'readme.txt');
    await drag(remote, local, 'received');
    await waitForResult(async () =>
      expect(await readFile(join(dir, 'received', file), 'utf8')).toBe(
        'round trip\n'
      )
    );
    expect(await readFile(join(dir, file), 'utf8')).toBe('round trip\n');
  } finally {
    await launcher.release();
    await rm(dir, { recursive: true, force: true });
  }
});
