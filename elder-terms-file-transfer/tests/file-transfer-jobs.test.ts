// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import {
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { selectCurrentTransferDirectory } from './file-transfer-destination-helpers.ts';
import {
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';
import { createWebdavTestServer } from './webdav-test-server.ts';
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

for (const scale of [1, 2]) {
  it(`queues, folds and cancels individual jobs while browsing at scale ${scale}`, async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-plan15-jobs-'));
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
      await mkdir(join(local, 'received'));
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
        const destination = remote ? localTree : remoteTree;
        const target = remote ? 'received' : 'archive';
        const targetRow = await waitForResult(async () => {
          expect((await destination.info()).states).toContain('sensitive');
          for (
            let index = 0;
            index < (await destination.getRowCount());
            index++
          )
            if (
              (await (await destination.cellAt(index, 0))?.info())?.name ===
              target
            )
              return index;
          throw new Error(`Missing transfer destination: ${target}`);
        });
        for (const selected of await destination.selectedRows())
          await destination.deselectRow(selected);
        await destination.selectRow(targetRow);
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
      const jobs = await app.getById('file_transfer_jobs');
      const beforeResize = (await jobs.capture()).bounds;
      const dividerX = Math.round(
        (beforeResize.x + beforeResize.width / 2) * scale
      );
      const dividerY = (beforeResize.y - 1) * scale;
      await app.input.moveMouseTo(dividerX, dividerY);
      await app.input.setMouseButton('left', true);
      await app.input.moveMouseTo(dividerX, dividerY - 100 * scale);
      await app.input.setMouseButton('left', false);
      await waitForResult(async () =>
        expect((await jobs.capture()).bounds.height).toBeGreaterThan(
          beforeResize.height + 80
        )
      );
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
      expect(
        await readFile(join(local, 'received', 'readme.txt'), 'utf8')
      ).toBe('hello from remote\n');
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

it('retains unknown progress and disconnected jobs until explicit retry', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-plan15-retry-'));
  const local = join(directory, 'local');
  const evidence = createTestEvidence(context);
  const server = await createWebdavTestServer('none', undefined);
  server.files.set('/dav/unknown-held.bin', {
    content: Buffer.alloc(2048, 42),
    reportedSize: undefined,
  });
  let serverClosed = false;
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
    // Selecting this destination from its parent must browse only our fixture.
    await mkdir(local);
    const config = join(directory, 'connection.ini');
    await writeFile(
      config,
      `[general]\ntype=webdav\nauto_close=false\n[webdav]\nscheme=http\nauthentication=none\naddress=127.0.0.1\nport=${server.port}\nbase_path=/dav/\nremote_directory=/\nlocal_directory=${local}\n`
    );
    const app = await launcher.launch(['-c', config], {
      onOutput: evidence.recordAppOutputEvent,
    });
    const tree = expectElementKind(
      await app.getById('file_transfer_remote_tree'),
      'table'
    );
    const receive = async (name: string) => {
      const row = await waitForResult(async () => {
        for (let i = 0; i < (await tree.getRowCount()); i++)
          if ((await (await tree.cellAt(i, 0))?.info())?.name === name)
            return i;
        throw new Error(`Missing ${name}`);
      });
      for (const i of await tree.selectedRows()) await tree.deselectRow(i);
      await tree.selectRow(row);
      const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
      await app.input.moveMouseTo(
        Math.round(bounds.x + bounds.width / 2),
        Math.round(bounds.y + bounds.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);
      await expectElementKind(
        await app.getById('file_transfer_receive_item'),
        'menuItem'
      ).click();
    };
    const state = async (id: number) =>
      await expectElementKind(
        await app.getById(`file_transfer_job_${id}_state`),
        'label'
      ).text();
    await selectCurrentTransferDirectory(app, false);
    await receive('unknown-held.bin');
    await waitForResult(async () => {
      expect(await state(1)).toBe('Transferring');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_jobs_summary'),
          'label'
        ).text()
      ).toContain('1024 bytes');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_jobs_summary'),
          'label'
        ).text()
      ).not.toContain('%');
    });
    await receive('hello.txt');
    expect(await state(2)).toBe('Waiting');
    // The refresh must wait behind the held download, then observe that the
    // service has disconnected before the next transfer is allowed to start.
    await expectElementKind(
      await app.getById('file_transfer_remote_refresh_button'),
      'button'
    ).click();
    const summary = expectElementKind(
      await app.getById('file_transfer_jobs_summary'),
      'label'
    );
    const folding = expectElementKind(
      await app.getById('file_transfer_jobs_expander'),
      'expander'
    );
    await folding.collapse();
    expect(await summary.text()).toContain('1024 bytes');
    expect(await summary.text()).not.toContain('%');
    await evidence.captureEvidence('unknown-progress-collapsed', async () =>
      app.capture()
    );
    await server.close();
    serverClosed = true;
    await waitForResult(async () =>
      expect(['Awaiting decision', 'Failed']).toContain(await state(1))
    );
    if ((await state(1)) === 'Awaiting decision') {
      // An interrupted HTTP body is a request failure; the subsequent refresh
      // confirms that the endpoint itself is unavailable.
      await expectElementKind(
        await app.getById('file_transfer_prompt_cancel_button'),
        'button'
      ).click();
    }
    await waitForResult(async () => {
      expect(await state(1)).toBe('Failed');
      expect(await state(2)).toBe('Failed');
    });
    expect(
      (await (await app.getById('file_transfer_jobs_paned')).info()).states
    ).not.toContain('sensitive');
    const retry = expectElementKind(
      await app.getById('file_transfer_job_2_retry'),
      'button'
    );
    expect((await retry.info()).states).not.toContain('sensitive');
    await server.restart();
    serverClosed = false;
    const reconnect = expectElementKind(
      await app.getById('file_transfer_reconnect_button'),
      'button'
    );
    await waitForResult(async () =>
      expect((await reconnect.info()).states).toContain('sensitive')
    );
    await reconnect.click();
    await waitForResult(async () =>
      expect((await retry.info()).states).toContain('sensitive')
    );
    await folding.expand();
    await expectElementKind(
      await app.getById('file_transfer_job_2_row'),
      'expander'
    ).expand();
    expect(
      await expectElementKind(
        await app.getById('file_transfer_job_2_details'),
        'label'
      ).text()
    ).toContain('Disconnected before execution');
    expect(
      server.requests.filter(
        (r) => r.method === 'GET' && r.url === '/dav/hello.txt'
      )
    ).toHaveLength(0);
    expect(await state(2)).toBe('Failed');
    await expectElementKind(
      await app.getById('file_transfer_job_2_row'),
      'expander'
    ).collapse();
    await retry.click();
    await waitForResult(async () => expect(await state(3)).toBe('Completed'));
    expect(await readFile(join(local, 'hello.txt'), 'utf8')).toBe(
      'Hello DAV!\r\n'
    );
    expect(await state(2)).toBe('Failed');
    await retry.click();
    await waitForResult(async () =>
      expect(await state(4)).toBe('Awaiting decision')
    );
    await evidence.captureEvidence('overwrite-modal', async () =>
      app.capture()
    );
    expect(
      (await (await app.getById('file_transfer_dim_overlay')).info()).states
    ).toContain('showing');
    expect(
      (await (await app.getById('file_transfer_jobs_paned')).info()).states
    ).not.toContain('sensitive');
    await expectElementKind(
      await app.getById('file_transfer_prompt_alternative_button'),
      'button'
    ).click();
    await waitForResult(async () => expect(await state(4)).toBe('Completed'));
    await expectElementKind(
      await app.getById('file_transfer_job_4_row'),
      'expander'
    ).expand();
    expect(
      await expectElementKind(
        await app.getById('file_transfer_job_4_details'),
        'label'
      ).text()
    ).toContain('Copied: 0; skipped: 1; failed: 0; unprocessed: 0');
    expect(
      await expectElementKind(
        await app.getById('file_transfer_job_4_progress'),
        'progressBar'
      ).value()
    ).toBe(0);
    expect(
      server.requests.filter(
        (r) => r.method === 'GET' && r.url === '/dav/hello.txt'
      )
    ).toHaveLength(1);
    expect(
      (await readdir(local)).some((name) => name.includes('.elder-terms-part-'))
    ).toBe(false);
    const jobBounds = (
      await (await app.getById('file_transfer_jobs')).capture()
    ).bounds;
    await app.input.moveMouseTo(
      Math.round(jobBounds.x + jobBounds.width / 2),
      jobBounds.y - 1
    );
    await app.input.setMouseButton('left', true);
    await app.input.moveMouseTo(
      Math.round(jobBounds.x + jobBounds.width / 2),
      jobBounds.y - 251
    );
    await app.input.setMouseButton('left', false);
    let previous: Buffer | undefined;
    const finalCapture = await waitForResult(async () => {
      const capture = await app.capture();
      const stable = previous?.equals(capture.image);
      previous = capture.image;
      expect(stable).toBe(true);
      return capture;
    });
    await evidence.captureEvidence(
      'explicit-retry-and-skip',
      async () => finalCapture
    );
    await evidence.flushOutputs([app], launcher);
  } finally {
    await launcher.release();
    if (!serverClosed) await server.close();
    await evidence.release();
    await rm(directory, { recursive: true, force: true });
  }
}, 60_000);
