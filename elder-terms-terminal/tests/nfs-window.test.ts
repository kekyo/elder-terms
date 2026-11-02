// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import { once } from 'node:events';
import {
  mkdir,
  mkdtemp,
  readFile,
  rename,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers.ts';

it('transfers encoded NFS files through protected export roots', async (context) => {
  if (process.env.ELDER_NFS_VM_ACTIVE !== '1') {
    const child = spawn(
      process.execPath,
      [
        fileURLToPath(new URL('./nfs/run-vm.mts', import.meta.url)),
        '--ui-window-only',
      ],
      { stdio: ['ignore', 'pipe', 'pipe'] }
    );
    let output = '';
    child.stdout.on('data', (data) => {
      output += data;
    });
    child.stderr.on('data', (data) => {
      output += data;
    });
    const watchdog = setTimeout(() => child.kill('SIGKILL'), 300000);
    try {
      const [status] = await once(child, 'exit');
      expect(status, output).toBe(0);
    } finally {
      clearTimeout(watchdog);
    }
    return;
  }
  const directory = await mkdtemp(join(tmpdir(), 'elder-nfs-window-'));
  const local = join(directory, 'local');
  await mkdir(join(local, '送信表', '深い'), { recursive: true });
  const content = Buffer.from('NFS contents\0\xff\n', 'binary');
  await writeFile(join(local, '送信表', '深い', '内容.txt'), content);
  const config = join(directory, 'connection.ini');
  await writeFile(
    config,
    [
      '[general]',
      'type=nfs',
      'auto_close=false',
      'name=NFS files',
      '[nfs]',
      'address=127.0.0.1',
      'filename_encoding=CP932',
      `local_directory=${local}`,
      'request_timeout_seconds=3',
      '',
    ].join('\n')
  );
  const evidence = createTestEvidence(context);
  const launcher = createGtkAppLauncher({
    appPath: fileURLToPath(
      new URL(
        '../../.build/elder-terms-terminal/elder-terms-file-transfer',
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
    const remote = expectElementKind(
      await app.getById('file_transfer_remote_tree'),
      'table'
    );
    const localTree = expectElementKind(
      await app.getById('file_transfer_local_tree'),
      'table'
    );
    const remotePath = expectElementKind(
      await app.getById('file_transfer_remote_path_entry'),
      'entry'
    );
    const rowFor = async (
      tree: GtkTableElement,
      name: string
    ): Promise<number> =>
      await waitForResult(
        async () => {
          expect((await tree.info()).states).toContain('sensitive');
          for (let row = 0; row < (await tree.getRowCount()); row++)
            if ((await (await tree.cellAt(row, 0))?.info())?.name === name)
              return row;
          throw new Error(`Missing NFS browser row: ${name}`);
        },
        { timeoutMs: 15000 }
      );
    const select = async (
      tree: GtkTableElement,
      name: string,
      button: 'left' | 'right'
    ): Promise<void> => {
      const row = await rowFor(tree, name);
      for (const selected of await tree.selectedRows())
        await tree.deselectRow(selected);
      await tree.selectRow(row);
      const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
      await app.input.moveMouseTo(
        Math.round(bounds.x + bounds.width / 2),
        Math.round(bounds.y + bounds.height / 2)
      );
      await app.input.setMouseButton(button, true);
      await app.input.setMouseButton(button, false);
    };
    const menu = async (id: string): Promise<void> => {
      const item = expectElementKind(await app.getById(id), 'menuItem');
      await waitForResult(async () =>
        expect((await item.info()).states).toContain('showing')
      );
      await item.click();
    };
    await rowFor(remote, '/srv/other/public');
    await rowFor(remote, '/srv/public/deep');
    await select(remote, '/srv/public', 'right');
    for (const action of ['new_directory', 'rename', 'delete'])
      expect(
        (
          await (
            await app.getById(`file_transfer_remote_${action}_item`)
          ).info()
        ).states
      ).not.toContain('sensitive');
    await evidence.captureEvidence('nfs-export-roots', async () =>
      app.capture()
    );
    await app.input.pressKey('Escape');
    await select(remote, '/srv/public', 'left');
    await waitForResult(async () =>
      expect((await remote.info()).states).toContain('focused')
    );
    await app.input.pressKey('Return');
    await waitForResult(async () =>
      expect(await remotePath.text()).toBe('/srv/public:/')
    );
    await rowFor(remote, '日本語表');
    await select(localTree, '送信表', 'right');
    await menu('file_transfer_send_item');
    await rowFor(remote, '送信表');
    await select(remote, '送信表', 'right');
    await menu('file_transfer_remote_rename_item');
    const prompt = expectElementKind(
      await app.getById('file_transfer_prompt_entry'),
      'entry'
    );
    await waitForResult(async () =>
      expect((await prompt.info()).states).toContain('showing')
    );
    await prompt.setText('変更表');
    await expectElementKind(
      await app.getById('file_transfer_prompt_accept_button'),
      'button'
    ).click();
    await rowFor(remote, '変更表');
    await rename(join(local, '送信表'), join(directory, 'original'));
    await select(remote, '変更表', 'right');
    await menu('file_transfer_receive_item');
    await waitForResult(
      async () =>
        expect(
          await readFile(join(local, '変更表', '深い', '内容.txt'))
        ).toEqual(content),
      { timeoutMs: 15000 }
    );
    await select(remote, '日本語表', 'right');
    await menu('file_transfer_receive_item');
    await waitForResult(async () =>
      expect(await readFile(join(local, '日本語表'), 'utf8')).toBe('CP932\n')
    );
    await rowFor(localTree, '日本語表');
    expect(
      (await (await app.getById('file_transfer_job_2_row')).info()).name
    ).toContain('変更表');
    expect(
      (await (await app.getById('file_transfer_job_3_row')).info()).name
    ).toContain('日本語表');
    await evidence.captureEvidence('nfs-v3-transferred', async () =>
      app.capture()
    );
    await expectElementKind(
      await app.getById('file_transfer_remote_up_button'),
      'button'
    ).click();
    await rowFor(remote, '/srv/public');
    expect(await remotePath.text()).toBe('/');

    await expectElementKind(
      await app.getById('application_menu_button'),
      'toggleButton'
    ).click();
    await menu('settings_menu_item');
    const tabs = expectElementKind(
      await app.getById('settings_notebook'),
      'tabList'
    );
    for (let index = 0; index < (await tabs.getChildCount()); index++) {
      if ((await (await tabs.childAt(index))?.info())?.name === 'NFS')
        await tabs.selectChildAt(index);
    }
    const encoding = expectElementKind(
      await app.getById('settings_nfs_filename_encoding_entry'),
      'entry'
    );
    await encoding.setText('EUC-JP');
    await expectElementKind(
      await app.getById('settings_save_button'),
      'button'
    ).click();
    const enterPublic = async (): Promise<void> => {
      await select(remote, '/srv/public', 'left');
      await waitForResult(async () =>
        expect((await remote.info()).states).toContain('focused')
      );
      await app.input.pressKey('Return');
      await rowFor(remote, '日本語表');
    };
    let nextJapaneseJob = 4;
    const receiveJapanese = async (expected: string): Promise<void> => {
      await select(remote, '日本語表', 'right');
      await menu('file_transfer_receive_item');
      const overwrite = expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      );
      await waitForResult(async () =>
        expect((await overwrite.info()).states).toContain('showing')
      );
      await overwrite.click();
      const state = expectElementKind(
        await app.getById(`file_transfer_job_${nextJapaneseJob++}_state`),
        'label'
      );
      await waitForResult(async () =>
        expect(await state.text()).toBe('Completed')
      );
      await waitForResult(async () =>
        expect(await readFile(join(local, '日本語表'), 'utf8')).toBe(expected)
      );
      await rowFor(localTree, '日本語表');
    };
    await enterPublic();
    await receiveJapanese('CP932\n');
    const ssh = async (command: string): Promise<void> => {
      await promisify(execFile)('ssh', [
        '-F',
        '/dev/null',
        '-i',
        fileURLToPath(
          new URL('../../.build/nfs-vm/id_ed25519', import.meta.url)
        ),
        '-p',
        '22022',
        '-o',
        'StrictHostKeyChecking=no',
        '-o',
        'UserKnownHostsFile=/dev/null',
        '-o',
        'LogLevel=ERROR',
        'root@127.0.0.1',
        command,
      ]);
    };
    await ssh('rpc.nfsd 0');
    await expectElementKind(
      await app.getById('file_transfer_remote_refresh_button'),
      'button'
    ).click();
    const reconnect = expectElementKind(
      await app.getById('file_transfer_reconnect_button'),
      'button'
    );
    await waitForResult(
      async () => expect((await reconnect.info()).states).toContain('showing'),
      { timeoutMs: 15000 }
    );
    await ssh('rpc.nfsd -N 2 -V 3 -N 4 -U 8');
    await reconnect.click();
    await enterPublic();
    await receiveJapanese('EUC-JP\n');
    expect(
      (await (await app.getById('file_transfer_job_2_row')).info()).name
    ).toContain('変更表');
    await evidence.captureEvidence('nfs-reconnected-encoding', async () =>
      app.capture()
    );
  } finally {
    await launcher.release();
    await rm(directory, { recursive: true, force: true });
  }
}, 360000);
