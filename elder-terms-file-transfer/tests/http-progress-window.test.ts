// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { once } from 'node:events';
import { mkdtemp, mkdir, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { createGtkAppLauncher, type GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { selectCurrentTransferDirectory } from './file-transfer-destination-helpers.ts';
import {
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

it('records unknown HTTP progress, cancellation cleanup and closing an active exploration', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-http-progress-'));
  const local = join(directory, 'local');
  await mkdir(local);
  const requests: string[] = [];
  const closed: string[] = [];
  const server = createServer((request, response) => {
    const url = request.url ?? '';
    requests.push(url);
    response.on('close', () => closed.push(url));
    if (url === '/pub/') {
      response.setHeader('Content-Type', 'text/html');
      response.end('<a href="slow.bin">slow</a><a href="explore">explore</a>');
    } else if (url === '/pub/slow.bin') {
      response.setHeader('Content-Type', 'application/octet-stream');
      response.write(Buffer.alloc(1024, 42));
    } else if (url === '/pub/explore') {
      response.setHeader('Content-Type', 'text/html');
      response.write('<a href="slow.bin">still loading</a>');
    } else {
      response.statusCode = 404;
      response.end();
    }
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const address = server.address();
  if (!address || typeof address === 'string')
    throw new Error('Expected TCP endpoint');
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
  let recorder: ReturnType<typeof spawn> | undefined;
  let recorded: Promise<number | null> | undefined;
  try {
    const config = join(directory, 'http.ini');
    await writeFile(
      config,
      '[general]\ntype=http\nauto_close=false\n[http]\nbase_url=http://127.0.0.1:' +
        address.port +
        '/pub/\nlocal_directory=' +
        local +
        '\n'
    );
    const app = await launcher.launch(['-c', config], {
      onOutput: evidence.recordAppOutputEvent,
    });
    const tree = expectElementKind(
      await app.getById('file_transfer_remote_tree'),
      'table'
    );
    const select = async (name: string) => {
      const index = await waitForResult(async () => {
        for (let i = 0; i < (await tree.getRowCount()); i++)
          if ((await (await tree.cellAt(i, 0))?.info())?.name === name)
            return i;
        throw new Error('Missing ' + name);
      });
      for (const row of await tree.selectedRows()) await tree.deselectRow(row);
      await tree.selectRow(index);
      const area = (await (await tree.cellAt(index, 0))!.capture()).bounds;
      await app.input.moveMouseTo(
        Math.round(area.x + area.width / 2),
        Math.round(area.y + area.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);
    };
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
        join(evidence.directory, 'http-progress.mkv'),
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
        const area = (await (await app.getById('file_transfer_jobs')).capture())
          .bounds;
        const capture = await app.capture();
        const expected = PNG.sync.read(capture.image);
        let matching = 0,
          samples = 0;
        for (let y = 2; y < area.height - 2; y += 3)
          for (let x = 2; x < area.width - 2; x += 3) {
            const source = ((area.y + y) * screen.width + area.x + x) * 4;
            const target = source;
            if (
              Math.abs(latest[source]! - expected.data[target + 2]!) < 12 &&
              Math.abs(latest[source + 1]! - expected.data[target + 1]!) < 12 &&
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

    await selectCurrentTransferDirectory(app, false);
    await select('slow.bin');
    await expectElementKind(
      await app.getById('file_transfer_receive_item'),
      'menuItem'
    ).click();
    const state = expectElementKind(
      await app.getById('file_transfer_job_1_state'),
      'label'
    );
    const summary = expectElementKind(
      await app.getById('file_transfer_jobs_summary'),
      'label'
    );
    await waitForResult(async () => {
      expect(await state.text()).toBe('Transferring');
      expect(await summary.text()).toContain('1024 bytes');
      expect(await summary.text()).not.toContain('%');
    });
    await observeVideo('http-unknown-progress');
    await expectElementKind(
      await app.getById('file_transfer_jobs_expander'),
      'expander'
    ).collapse();
    expect(await summary.text()).toContain('1024 bytes');
    await observeVideo('http-unknown-progress-collapsed');
    await expectElementKind(
      await app.getById('file_transfer_jobs_expander'),
      'expander'
    ).expand();
    await expectElementKind(
      await app.getById('file_transfer_job_1_cancel'),
      'button'
    ).click();
    await waitForResult(async () => {
      expect(await state.text()).toBe('Cancelled');
      expect(closed).toContain('/pub/slow.bin');
      expect(await readdir(local)).toEqual([]);
    });
    await observeVideo('http-cancelled');
    expect(new Set(observed.map((item) => item.pixels)).size).toBe(3);
    expect(observed[2]!.frame).toBeGreaterThan(observed[0]!.frame);
    await evidence.log('Observed HTTP video states', observed);
    recorder!.stdin!.write('q\n');
    expect(await recorded, recordingError).toBe(0);
    recorder = undefined;
    await select('explore');
    await expectElementKind(
      await app.getById('file_transfer_expand_html_item'),
      'menuItem'
    ).click();
    await waitForResult(async () => {
      expect(requests).toContain('/pub/explore');
      expect(closed).not.toContain('/pub/explore');
    });
    const widgets: GtkWidgetElement[] = [
      await app.getById('file_transfer_header_bar'),
    ];
    let closing = false;
    while (widgets.length) {
      const widget = widgets.shift()!;
      if (widget.kind === 'button' && (await widget.info()).name === 'Close') {
        await widget.click();
        closing = true;
        break;
      }
      if ('getChildCount' in widget)
        for (let i = 0; i < (await widget.getChildCount()); i++) {
          const child = await widget.childAt(i);
          if (child) widgets.push(child);
        }
    }
    expect(closing).toBe(true);
    await waitForResult(async () => {
      expect((await app.output()).exitCode).toBe(0);
      expect(closed).toContain('/pub/explore');
    });
    expect(requests).toEqual(['/pub/', '/pub/slow.bin', '/pub/explore']);
    await evidence.flushOutputs([app], launcher);
  } finally {
    if (recorder) {
      recorder.stdin!.write('q\n');
      await recorded;
    }
    await launcher.release();
    await evidence.release();
    server.closeAllConnections();
    server.close();
    await once(server, 'close');
    await rm(directory, { recursive: true, force: true });
  }
}, 90_000);
