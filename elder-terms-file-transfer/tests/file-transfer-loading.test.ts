// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { once } from 'node:events';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { createServer, type ServerResponse } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PNG } from 'pngjs';
import { createGtkAppLauncher, type GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import {
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

for (const scale of [1, 2]) {
  it(`animates remote node loading until completion, failure or close at scale ${scale}`, async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-tree-loading-'));
    const pending = new Map<string, ServerResponse>();
    const closed = new Set<string>();
    const server = createServer((request, response) => {
      const path = request.url ?? '';
      response.setHeader('Content-Type', 'text/html');
      if (path === '/pub/') {
        response.end(
          '<a href="complete/">complete</a><a href="empty/">empty</a><a href="failed/">failed</a><a href="closing/">closing</a>'
        );
      } else {
        pending.set(path, response);
        response.on('close', () => closed.add(path));
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
        GDK_SCALE: String(scale),
        XDG_CONFIG_HOME: join(directory, 'config'),
        XDG_DATA_HOME: join(directory, 'data'),
      },
      xvfbTrayHost: true,
      xvfbScreen: scale === 1 ? '1280x900x24' : '2560x1600x24',
      onSystemOutput: evidence.recordSystemOutputEvent,
    });
    let recorder: ReturnType<typeof spawn> | undefined;
    let recorded: Promise<number | null> | undefined;
    try {
      const config = join(directory, 'http.ini');
      await writeFile(
        config,
        `[general]\ntype=http\nauto_close=false\n[http]\nbase_url=http://127.0.0.1:${address.port}/pub/\nlocal_directory=${directory}\n`
      );
      const app = await launcher.launch(['-c', config], {
        onOutput: evidence.recordAppOutputEvent,
      });
      const tree = expectElementKind(
        await app.getById('file_transfer_remote_tree'),
        'table'
      );
      await waitForResult(async () => {
        expect((await tree.info()).states).toContain('sensitive');
        expect(await tree.getRowCount()).toBe(4);
      });
      const screen = PNG.sync.read((await app.capture()).image);
      const environment = await app.environment();
      const frameBytes = screen.width * screen.height * 4;
      let frame = Buffer.allocUnsafe(frameBytes),
        buffered = 0;
      let region:
        { x: number; y: number; width: number; height: number } | undefined;
      let hashes: string[] = [];
      recorder = spawn(
        'ffmpeg',
        [
          '-hide_banner',
          '-loglevel',
          'error',
          '-f',
          'x11grab',
          '-framerate',
          '20',
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
          join(evidence.directory, 'node-loading.mkv'),
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
          bytes.copy(frame, buffered, offset, offset + count);
          offset += count;
          buffered += count;
          if (buffered !== frameBytes) continue;
          if (region) {
            const hash = createHash('sha256');
            for (let y = region.y; y < region.y + region.height; ++y) {
              const start = (y * screen.width + region.x) * 4;
              hash.update(frame.subarray(start, start + region.width * 4));
            }
            hashes.push(hash.digest('hex'));
          }
          frame = Buffer.allocUnsafe(frameBytes);
          buffered = 0;
        }
      });
      const rowNamed = async (name: string) =>
        await waitForResult(async () => {
          for (let row = 0; row < (await tree.getRowCount()); ++row)
            if ((await (await tree.cellAt(row, 0))!.info()).name === name)
              return row;
          throw new Error('Missing row ' + name);
        });
      const sample = async (row: number, moving: boolean) => {
        const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
        const treeBounds = (await tree.capture()).bounds;
        // AT-SPI's text bounds start after the icon. Include the row's leading
        // indentation while excluding activity indicators outside the tree.
        region = {
          x: treeBounds.x * scale,
          y: bounds.y * scale,
          width: Math.min(120, treeBounds.width) * scale,
          height: bounds.height * scale,
        };
        hashes = [];
        await evidence.log('Sampled row', {
          region,
          bounds,
          screen: { width: screen.width, height: screen.height },
        });
        await waitForResult(async () =>
          expect(hashes.length, recordingError).toBeGreaterThanOrEqual(18)
        );
        const frames = hashes.slice(8, 18);
        if (moving)
          expect(
            new Set(frames).size,
            'Loading must visibly animate in the recorded row'
          ).toBeGreaterThan(2);
        else
          expect(
            new Set(frames).size,
            'Completed rows must stop animating'
          ).toBe(1);
        region = undefined;
        await evidence.log(
          moving ? 'Loading frames' : 'Completed frames',
          frames
        );
        return bounds.height;
      };
      const expand = async (name: string) => {
        await waitForResult(async () =>
          expect((await tree.info()).states).toContain('sensitive')
        );
        const row = await rowNamed(name);
        const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
        const area = (await tree.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round((area.x + 10) * scale),
          Math.round((bounds.y + bounds.height / 2) * scale)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await app.input.moveMouseTo(0, 0);
        await waitForResult(async () =>
          expect(pending.has(`/pub/${name}/`)).toBe(true)
        );
        const loading = await rowNamed('Loading…');
        expect(
          (await (await tree.cellAt(loading, 0))!.capture()).bounds.height
        ).toBe(bounds.height);
        await sample(loading, true);
        await evidence.captureEvidence(name + '-loading', async () =>
          app.capture()
        );
        return pending.get(`/pub/${name}/`)!;
      };
      const completed = await expand('complete');
      completed.end('<a href="done.bin">done</a>');
      await waitForResult(async () =>
        expect((await tree.info()).states).toContain('sensitive')
      );
      await sample(await rowNamed('done.bin'), false);
      await evidence.captureEvidence('completed', async () => app.capture());
      const empty = await expand('empty');
      empty.end('<p>No children</p>');
      await waitForResult(async () => {
        expect((await tree.info()).states).toContain('sensitive');
        expect(await tree.getRowCount()).toBe(5);
      });
      await sample(await rowNamed('empty'), false);
      const failed = await expand('failed');
      failed.writeHead(500);
      failed.end('Unable to list');
      const error = await app.getById('file_transfer_operation_error_dialog');
      await waitForResult(async () =>
        expect((await error.info()).states).toContain('showing')
      );
      await app.input.pressKey('Escape');
      await waitForResult(async () =>
        expect((await tree.info()).states).toContain('sensitive')
      );
      await sample(await rowNamed('failed'), false);
      pending.delete('/pub/failed/');
      const retry = await expand('failed');
      retry.end('<a href="recovered.bin">recovered</a>');
      await sample(await rowNamed('recovered.bin'), false);
      await expand('closing');
      const widgets: GtkWidgetElement[] = [
        await app.getById('file_transfer_header_bar'),
      ];
      let closing = false;
      while (widgets.length) {
        const widget = widgets.shift()!;
        if (
          widget.kind === 'button' &&
          (await widget.info()).name === 'Close'
        ) {
          await widget.click();
          closing = true;
          break;
        }
        if ('getChildCount' in widget)
          for (let i = 0; i < (await widget.getChildCount()); ++i) {
            const child = await widget.childAt(i);
            if (child) widgets.push(child);
          }
      }
      expect(closing).toBe(true);
      await waitForResult(async () => {
        expect((await app.output()).exitCode).toBe(0);
        expect(closed.has('/pub/closing/')).toBe(true);
      });
      expect((await app.output()).stderr).not.toMatch(/CRITICAL|WARNING/);
      recorder.stdin!.write('q\n');
      expect(await recorded, recordingError).toBe(0);
      recorder = undefined;
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
}
