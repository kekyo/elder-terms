// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import {
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

it('groups NFS exports by their common directory prefixes', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-nfs-tree-'));
  const config = join(directory, 'connection.ini');
  await writeFile(
    config,
    [
      '[general]',
      'type=nfs',
      'auto_close=false',
      '[nfs]',
      'address=127.0.0.1',
      'discover_exports=false',
      'known_exports=/storage0/music;/storage1/home;/storage1/images;/storage1/media;/storage1/public;/storage2/videos;/storage4/youtube;',
      `local_directory=${directory}`,
      '',
    ].join('\n')
  );
  const evidence = createTestEvidence(context);
  const launcher = createGtkAppLauncher({
    appPath: fileURLToPath(
      new URL(
        '../../.build/elder-terms-file-transfer/elder-terms-file-transfer',
        import.meta.url
      )
    ),
    env: {
      LANGUAGE: 'en',
      LC_ALL: 'C.UTF-8',
      XDG_CONFIG_HOME: join(directory, 'config'),
      XDG_DATA_HOME: join(directory, 'data'),
    },
    onSystemOutput: evidence.recordSystemOutputEvent,
    xvfbTrayHost: true,
  });
  try {
    const app = await launcher.launch(['-c', config], {
      onOutput: evidence.recordAppOutputEvent,
    });
    const tree = expectElementKind(
      await app.getById('file_transfer_remote_tree'),
      'table'
    );
    const path = expectElementKind(
      await app.getById('file_transfer_remote_path_entry'),
      'entry'
    );
    const rowNames = async (): Promise<string[]> => {
      const names: string[] = [];
      for (let row = 0; row < (await tree.getRowCount()); row++)
        names.push((await (await tree.cellAt(row, 0))!.info()).name);
      return names;
    };
    await waitForResult(async () => {
      expect((await tree.info()).states).toContain('sensitive');
      expect(await tree.getRowCount()).toBeGreaterThan(0);
    });
    expect(await rowNames()).toEqual([
      '/storage0',
      '/storage1',
      '/storage2',
      '/storage4',
    ]);
    await evidence.captureEvidence('nfs-grouped-roots', async () =>
      app.capture()
    );
    await tree.selectRow(1);
    const bounds = (await (await tree.cellAt(1, 0))!.capture()).bounds;
    await app.input.moveMouseTo(
      Math.round(bounds.x + bounds.width / 2),
      Math.round(bounds.y + bounds.height / 2)
    );
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    await waitForResult(async () =>
      expect((await tree.info()).states).toContain('focused')
    );
    const treeBounds = (await tree.capture()).bounds;
    await app.input.moveMouseTo(
      treeBounds.x + 10,
      Math.round(bounds.y + bounds.height / 2)
    );
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    await waitForResult(async () =>
      expect(await rowNames()).toEqual([
        '/storage0',
        '/storage1',
        'home',
        'images',
        'media',
        'public',
        '/storage2',
        '/storage4',
      ])
    );
    await evidence.captureEvidence('nfs-expanded-prefix', async () =>
      app.capture()
    );
    const expanded = (await (await tree.cellAt(1, 0))!.capture()).bounds;
    await app.input.moveMouseTo(
      Math.round(expanded.x + expanded.width / 2),
      Math.round(expanded.y + expanded.height / 2)
    );
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    await waitForResult(async () =>
      expect((await tree.info()).states).toContain('focused')
    );
    await app.input.pressKey('Return');
    await waitForResult(async () =>
      expect(await path.text()).toBe('/storage1')
    );
    expect(await rowNames()).toEqual(['home', 'images', 'media', 'public']);
    await expectElementKind(
      await app.getById('file_transfer_remote_up_button'),
      'button'
    ).click();
    await waitForResult(async () => expect(await path.text()).toBe('/'));
    expect(await rowNames()).toEqual([
      '/storage0',
      '/storage1',
      '/storage2',
      '/storage4',
    ]);
    await evidence.flushOutputs([app], launcher);
  } finally {
    await launcher.release();
    await evidence.release();
    await rm(directory, { recursive: true, force: true });
  }
});
