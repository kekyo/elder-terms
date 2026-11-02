// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers.ts';
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

for (const scale of [1, 2]) {
  it(`queues, folds and cancels individual jobs while browsing at scale ${scale}`, async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-plan15-jobs-'));
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
        GDK_SCALE: String(scale),
        XDG_CONFIG_HOME: join(directory, 'config'),
        XDG_DATA_HOME: join(directory, 'data'),
      },
      onSystemOutput: evidence.recordSystemOutputEvent,
      xvfbTrayHost: true,
      xvfbScreen: scale === 1 ? '1280x900x24' : '2560x1600x24',
    });
    let recorder: ReturnType<typeof spawn> | undefined;
    let recorded: Promise<number | null> | undefined;
    try {
      const local = join(directory, 'local');
      await mkdir(local);
      await writeFile(join(local, 'source.txt'), 'upload\n');
      await writeFile(join(local, 'zero.txt'), '');
      const config = join(directory, 'connection.ini');
      await writeFile(
        config,
        `[general]\ntype=sftp\nname=Transfer jobs\nauto_close=false\nbackground=#183C58\n[ssh]\naddress=fixture.example\n[sftp]\nlocal_directory=${local}\nremote_directory=/remote\n`
      );
      const app = await launcher.launch(
        ['--test-fixture', '--test-sftp-pause-transfer', '-c', config],
        { onOutput: evidence.recordAppOutputEvent }
      );
      const localTree = expectElementKind(
        await app.getById('file_transfer_local_tree'),
        'table'
      ) as GtkTableElement;
      const remoteTree = expectElementKind(
        await app.getById('file_transfer_remote_tree'),
        'table'
      ) as GtkTableElement;
      const submit = async (remote: boolean, name: string) => {
        const tree = remote ? remoteTree : localTree;
        const row = await waitForResult(async () => {
          expect((await tree.info()).states).toContain('sensitive');
          for (let index = 0; index < (await tree.getRowCount()); index++)
            if ((await (await tree.cellAt(index, 0))?.info())?.name === name)
              return index;
          throw new Error(`Missing row: ${name}`);
        });
        for (const selected of await tree.selectedRows())
          await tree.deselectRow(selected);
        await tree.selectRow(row);
        const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
        // AT-SPI reports GTK logical bounds; X11 pointer input uses device pixels.
        await app.input.moveMouseTo(
          Math.round((bounds.x + bounds.width / 2) * scale),
          Math.round((bounds.y + bounds.height / 2) * scale)
        );
        await app.input.setMouseButton('right', true);
        await app.input.setMouseButton('right', false);
        if (name === 'zero.txt')
          expect(
            (
              await (
                await app.getById('file_transfer_local_delete_item')
              ).info()
            ).states
          ).not.toContain('sensitive');
        await expectElementKind(
          await app.getById(
            remote ? 'file_transfer_receive_item' : 'file_transfer_send_item'
          ),
          'menuItem'
        ).click();
      };
      const state = async (id: number) =>
        await expectElementKind(
          await app.getById(`file_transfer_job_${id}_state`),
          'label'
        ).text();
      const cancel = async (id: number) =>
        await expectElementKind(
          await app.getById(`file_transfer_job_${id}_cancel`),
          'button'
        ).click();
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
          join(evidence.directory, 'jobs.mkv'),
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
      recorder.stderr!.on('data', (bytes: Buffer) => {
        recordingError += bytes.toString();
      });
      recorder.stdout!.on('data', (bytes: Buffer) => {
        let offset = 0;
        while (offset < bytes.length) {
          const count = Math.min(bytes.length - offset, frameBytes - buffered);
          bytes.copy(pending, buffered, offset, offset + count);
          offset += count;
          buffered += count;
          if (buffered === frameBytes) {
            latest = pending;
            pending = Buffer.allocUnsafe(frameBytes);
            buffered = 0;
            frames++;
          }
        }
      });
      const observed: { name: string; frame: number; pixels: string }[] = [];
      const observeVideo = async (name: string) => {
        const before = frames;
        const capture = await waitForResult(async () => {
          expect(frames, recordingError).toBeGreaterThan(before);
          // Wait for layout and paint to agree with a recorded frame; a label
          // notification alone can arrive before GTK has painted the new row.
          const area = (
            await (await app.getById('file_transfer_jobs')).capture()
          ).bounds;
          const capture = await app.capture();
          const expected = PNG.sync.read(capture.image);
          let matching = 0,
            samples = 0;
          for (let y = 2; y < area.height * scale - 2; y += 3)
            for (let x = 2; x < area.width * scale - 2; x += 3) {
              const source =
                ((area.y * scale + y) * screen.width + area.x * scale + x) * 4;
              const target = source;
              if (
                Math.abs(latest[source]! - expected.data[target + 2]!) < 12 &&
                Math.abs(latest[source + 1]! - expected.data[target + 1]!) <
                  12 &&
                Math.abs(latest[source + 2]! - expected.data[target]!) < 12
              )
                matching++;
              samples++;
            }
          expect(matching / samples).toBeGreaterThan(0.96);
          return capture;
        });
        await evidence.captureEvidence(name, async () => capture);
        observed.push({
          name,
          frame: frames,
          pixels: createHash('sha256').update(latest).digest('hex'),
        });
      };
      await submit(false, 'source.txt');
      await waitForResult(async () =>
        expect(await state(1)).toBe('Transferring')
      );
      expect((await localTree.info()).states).toContain('sensitive');
      await submit(false, 'zero.txt');
      await submit(false, 'source.txt');
      await submit(true, 'readme.txt');
      expect(await state(2)).toBe('Waiting');
      expect(await state(3)).toBe('Waiting');
      expect(await state(4)).toBe('Waiting');
      await observeVideo('jobs-queued');
      const expander = expectElementKind(
        await app.getById('file_transfer_jobs_expander'),
        'expander'
      );
      await expander.collapse!();
      const summary = expectElementKind(
        await app.getById('file_transfer_jobs_summary'),
        'label'
      );
      expect(await summary.text()).toContain('1 active, 3 waiting');
      expect(await state(1)).toBe('Transferring');
      await observeVideo('jobs-collapsed');
      await expander.expand!();
      await cancel(3);
      await waitForResult(async () => expect(await state(3)).toBe('Cancelled'));
      expect(await state(1)).toBe('Transferring');
      expect(await state(2)).toBe('Waiting');
      await cancel(1);
      await waitForResult(async () => {
        expect(await state(1)).toBe('Cancelled');
        expect(await state(2)).toBe('Completed');
        expect(await state(4)).toBe('Completed');
      });
      expect(await readFile(join(local, 'readme.txt'), 'utf8')).toBe(
        'hello from remote\n'
      );
      expect(await summary.text()).toContain('0 active, 0 waiting, 4 finished');
      await expectElementKind(
        await app.getById('file_transfer_job_2_row'),
        'expander'
      ).expand();
      expect(
        await expectElementKind(
          await app.getById('file_transfer_job_2_details'),
          'label'
        ).text()
      ).toContain('Copied: 1; skipped: 0; failed: 0; unprocessed: 0');
      await observeVideo('jobs-completed');
      expect(new Set(observed.map((item) => item.pixels)).size).toBe(3);
      expect(observed[2]!.frame).toBeGreaterThan(observed[0]!.frame);
      await evidence.log('Observed job video states', observed);
      recorder.stdin!.write('q\n');
      expect(await recorded, recordingError).toBe(0);
      recorder = undefined;
      await expectElementKind(
        await app.getById('file_transfer_jobs_clear'),
        'button'
      ).click();
      expect(await summary.text()).toContain('0 finished');
      await evidence.captureEvidence('tree-icons-dark', async () =>
        app.capture()
      );
      await evidence.flushOutputs([app], launcher);
    } finally {
      if (recorder) {
        recorder.stdin!.write('q\n');
        await recorded;
      }
      await launcher.release();
      await evidence.release();
      await rm(directory, { recursive: true, force: true });
    }
  }, 90_000);
}
