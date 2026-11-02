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
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import {
  createGtkAppLauncher,
  type GtkTableElement,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers.ts';
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

it.for([2, 3, 4])(
  'transfers encoded NFS v%i files through protected public roots',
  { timeout: 360000 },
  async (version, context) => {
    const prefix = version === 4 ? '' : '/srv';
    if (process.env.ELDER_NFS_VM_ACTIVE !== '1') {
      const child = spawn(
        process.execPath,
        [
          fileURLToPath(new URL('./nfs/run-vm.mts', import.meta.url)),
          '--ui-window-only',
          `--ui-version=${version}`,
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
    const content = Buffer.alloc(192 * 1024);
    for (let index = 0; index < content.length; index++)
      content[index] = index % 251;
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
        `version=${version === 4 ? '4.0' : version}`,
        'known_namespace_paths=/other/public;/public/deep;',
        'filename_encoding=CP932',
        `local_directory=${local}`,
        'request_timeout_seconds=30',
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
    let recorder: ReturnType<typeof spawn> | undefined;
    let recorded: Promise<number | null> | undefined;
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
      await rowFor(remote, `${prefix}/other/public`);
      await rowFor(remote, `${prefix}/public/deep`);
      await select(remote, `${prefix}/public`, 'right');
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
      await select(remote, `${prefix}/public`, 'left');
      await waitForResult(async () =>
        expect((await remote.info()).states).toContain('focused')
      );
      await app.input.pressKey('Return');
      await waitForResult(async () =>
        expect(await remotePath.text()).toBe(`${prefix}/public:/`)
      );
      await rowFor(remote, '日本語表');

      const screen = PNG.sync.read((await app.capture()).image);
      const environment = await app.environment();
      const frameBytes = screen.width * screen.height * 4;
      let pending = Buffer.allocUnsafe(frameBytes),
        buffered = 0,
        latest = Buffer.alloc(0),
        frames = 0;
      recorder = spawn(
        'ffmpeg',
        [
          '-hide_banner',
          '-loglevel',
          'error',
          '-f',
          'x11grab',
          '-framerate',
          '10',
          '-draw_mouse',
          '0',
          '-video_size',
          `${screen.width}x${screen.height}`,
          '-i',
          environment.DISPLAY!,
          '-map',
          '0:v',
          '-c:v',
          'ffv1',
          '-threads',
          '1',
          '-pix_fmt',
          'bgr0',
          join(evidence.directory, `nfs-v${version}-progress.mkv`),
          '-map',
          '0:v',
          '-c:v',
          'rawvideo',
          '-threads',
          '1',
          '-pix_fmt',
          'bgr0',
          '-f',
          'rawvideo',
          'pipe:1',
        ],
        { env: environment, stdio: ['pipe', 'pipe', 'pipe'] }
      );
      recorded = new Promise((resolve) => recorder!.once('close', resolve));
      let recordingError = '';
      recorder.stderr!.on('data', (data: Buffer) => {
        recordingError += data.toString();
      });
      recorder.stdout!.on('data', (data: Buffer) => {
        let offset = 0;
        while (offset < data.length) {
          const count = Math.min(data.length - offset, frameBytes - buffered);
          data.copy(pending, buffered, offset, offset + count);
          buffered += count;
          offset += count;
          if (buffered === frameBytes) {
            latest = pending;
            pending = Buffer.allocUnsafe(frameBytes);
            buffered = 0;
            frames++;
          }
        }
      });
      const observed: { state: string; frame: number; pixels: string }[] = [];
      const observeVideo = async (state: string): Promise<void> => {
        const before = frames;
        const capture = await waitForResult(async () => {
          expect(frames, recordingError).toBeGreaterThan(before);
          const area = (
            await (await app.getById('file_transfer_jobs')).capture()
          ).bounds;
          const capture = await app.capture();
          const expected = PNG.sync.read(capture.image);
          let matches = 0,
            samples = 0;
          for (let y = 2; y < area.height - 2; y += 3)
            for (let x = 2; x < area.width - 2; x += 3) {
              const pixel = ((area.y + y) * screen.width + area.x + x) * 4;
              if (
                Math.abs(latest[pixel]! - expected.data[pixel + 2]!) < 12 &&
                Math.abs(latest[pixel + 1]! - expected.data[pixel + 1]!) < 12 &&
                Math.abs(latest[pixel + 2]! - expected.data[pixel]!) < 12
              )
                matches++;
              samples++;
            }
          expect(matches / samples).toBeGreaterThan(0.96);
          return capture;
        });
        await evidence.captureEvidence(
          `nfs-v${version}-${state}`,
          async () => capture
        );
        observed.push({
          state,
          frame: frames,
          pixels: createHash('sha256').update(latest).digest('hex'),
        });
      };
      expect(
        (
          await fetch(`${process.env.ELDER_NFS_CONTROL}/pause`, {
            method: 'POST',
          })
        ).ok
      ).toBe(true);
      await select(localTree, '送信表', 'right');
      await menu('file_transfer_send_item');
      const state = expectElementKind(
        await app.getById('file_transfer_job_1_state'),
        'label'
      );
      await waitForResult(async () => {
        const gate = await (
          await fetch(`${process.env.ELDER_NFS_CONTROL}/status`)
        ).json();
        expect(gate).toEqual({ waiting: 1 });
        expect(await state.text()).toBe('Transferring');
        expect(
          await expectElementKind(
            await app.getById('file_transfer_job_1_progress'),
            'progressBar'
          ).value()
        ).toBeCloseTo(1 / 3, 2);
      });
      await observeVideo('transferring');
      expect(
        (
          await fetch(`${process.env.ELDER_NFS_CONTROL}/resume`, {
            method: 'POST',
          })
        ).ok
      ).toBe(true);
      await rowFor(remote, '送信表');
      await waitForResult(async () =>
        expect(await state.text()).toBe('Completed')
      );
      await observeVideo('completed');
      expect(observed[1]!.frame).toBeGreaterThan(observed[0]!.frame);
      expect(new Set(observed.map((item) => item.pixels)).size).toBe(2);
      await evidence.log('Observed NFS progress video states', observed);
      recorder.stdin!.write('q\n');
      expect(await recorded, recordingError).toBe(0);
      recorder = undefined;
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
        { timeoutMs: 45000 }
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
      await evidence.captureEvidence(`nfs-v${version}-transferred`, async () =>
        app.capture()
      );
      await expectElementKind(
        await app.getById('file_transfer_remote_up_button'),
        'button'
      ).click();
      await rowFor(remote, `${prefix}/public`);
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
        await select(remote, `${prefix}/public`, 'left');
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
        async () =>
          expect((await reconnect.info()).states).toContain('showing'),
        { timeoutMs: 15000 }
      );
      await ssh(
        version === 4
          ? 'rpc.nfsd -N 2 -N 3 -V 4 -N 4.1 -N 4.2 -U -G 10 -L 10 8; echo Y > /proc/fs/nfsd/v4_end_grace'
          : version === 2
            ? 'rpc.nfsd -V 2 -N 3 -N 4 -U 8'
            : 'rpc.nfsd -N 2 -V 3 -N 4 -U 8'
      );
      await reconnect.click();
      await enterPublic();
      await receiveJapanese('EUC-JP\n');
      expect(
        (await (await app.getById('file_transfer_job_2_row')).info()).name
      ).toContain('変更表');
      await evidence.captureEvidence(
        `nfs-v${version}-reconnected-encoding`,
        async () => app.capture()
      );
      // Close a real connection with an outstanding WRITE. The wire gate makes
      // this deterministic, independent of file size, host speed or fixed sleeps.
      expect(
        (
          await fetch(`${process.env.ELDER_NFS_CONTROL}/pause`, {
            method: 'POST',
          })
        ).ok
      ).toBe(true);
      await select(localTree, '変更表', 'right');
      await menu('file_transfer_send_item');
      await waitForResult(async () => {
        expect(
          await (await fetch(`${process.env.ELDER_NFS_CONTROL}/status`)).json()
        ).toEqual({ waiting: 1 });
      });
      const widgets: GtkWidgetElement[] = [
        await app.getById('file_transfer_header_bar'),
      ];
      let closed = false;
      while (widgets.length) {
        const widget = widgets.shift()!;
        if (
          widget.kind === 'button' &&
          (await widget.info()).name === 'Close'
        ) {
          await widget.click();
          closed = true;
          break;
        }
        if ('getChildCount' in widget)
          for (let index = 0; index < (await widget.getChildCount()); index++) {
            const child = await widget.childAt(index);
            if (child) widgets.push(child);
          }
      }
      expect(closed).toBe(true);
      await waitForResult(async () =>
        expect((await app.output()).exitCode).toBe(0)
      );
      await evidence.flushOutputs([app], launcher);
    } finally {
      if (recorder) {
        recorder.stdin!.write('q\n');
        await recorded;
      }
      await fetch(`${process.env.ELDER_NFS_CONTROL}/resume`, {
        method: 'POST',
      });
      await launcher.release();
      await evidence.release();
      await rm(directory, { recursive: true, force: true });
    }
  }
);
