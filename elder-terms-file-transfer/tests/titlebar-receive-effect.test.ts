// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { createGtkAppLauncher, type GtkApp } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import {
  createTestEvidence,
  expectElementKind,
  withTemporaryDirectory,
} from '../../shared/tests/connection-test-helpers.ts';

it('records the default medium receive gradient for real FTP replies and returns to idle during local browsing', async (context) => {
  const evidence = createTestEvidence(context);
  await withTemporaryDirectory(async (directory) => {
    const remote = join(directory, 'remote');
    const local = join(directory, 'local');
    await mkdir(remote);
    await mkdir(local);
    await writeFile(join(remote, 'received.txt'), 'Remote data\n');
    const server = spawn(
      resolve('../.build/elder-terms-file-transfer/ftp-test-server'),
      [remote, '--trace']
    );
    const serverFinished = once(server, 'close');
    let serverOutput = '';
    server.stdout.on('data', (chunk: Buffer) => {
      serverOutput += chunk.toString();
    });
    server.stderr.on('data', (chunk: Buffer) => {
      serverOutput += chunk.toString();
    });
    let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
    const apps: GtkApp[] = [];
    try {
      const port = await waitForResult(async () => {
        expect(server.exitCode, serverOutput).toBeNull();
        const match = /READY (\d+)/.exec(serverOutput);
        expect(match).not.toBeNull();
        return match![1];
      });
      const config = join(directory, 'ftp.ini');
      await writeFile(
        config,
        `[general]\nname=Receive effect\ntype=ftp\nexterior_background=#202020\n[ftp]\naddress=127.0.0.1\nport=${port}\nusername=alice\nlocal_directory=${local}\nremote_directory=/\n`
      );
      launcher = createGtkAppLauncher({
        appPath: resolve(
          '../.build/elder-terms-file-transfer/elder-terms-file-transfer'
        ),
        env: {
          LANGUAGE: 'en',
          LC_ALL: 'C.UTF-8',
          GDK_SCALE: '1',
          XDG_CONFIG_HOME: join(directory, 'config'),
          XDG_DATA_HOME: join(directory, 'data'),
        },
        onSystemOutput: evidence.recordSystemOutputEvent,
        xvfbTrayHost: true,
      });
      const app = await launcher.launch(['-c', config], {
        onOutput: evidence.recordAppOutputEvent,
      });
      apps.push(app);
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      await waitForResult(async () =>
        expect(
          await expectElementKind(
            await app.getById('file_transfer_prompt_message_label'),
            'label'
          ).text()
        ).toBe('Password:')
      );
      await expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      ).setText('secret');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(
          (await (await app.getById('file_transfer_remote_tree')).info()).states
        ).toContain('sensitive');
        expect(serverOutput).toContain('MLSD');
      });
      const header = await app.getById('file_transfer_header_bar');
      const label = expectElementKind(
        await app.getById('file_transfer_title_label'),
        'label'
      );
      const title = await label.text();
      const capture = await waitForResult(async () => {
        const result = await header.capture();
        const image = PNG.sync.read(result.image);
        for (const x of [0.05, 0.2, 0.35, 0.5, 0.65, 0.8, 0.95])
          expect(
            image.data[(3 * image.width + Math.round(image.width * x)) * 4]
          ).toBe(32);
        return result;
      });
      const png = PNG.sync.read(capture.image);
      const env = await app.environment();
      const recorder = spawn(
        'ffmpeg',
        [
          '-y',
          '-loglevel',
          'error',
          '-f',
          'x11grab',
          '-framerate',
          '60',
          '-draw_mouse',
          '0',
          '-video_size',
          `${png.width}x${png.height}`,
          '-i',
          `${env.DISPLAY}+${capture.bounds.x},${capture.bounds.y}`,
          '-t',
          '5',
          '-c:v',
          'ffv1',
          '-threads',
          '1',
          join(evidence.directory, 'ftp-receive.mkv'),
          '-t',
          '5',
          '-f',
          'rawvideo',
          '-pix_fmt',
          'rgb24',
          '-threads',
          '1',
          'pipe:1',
        ],
        { env }
      );
      const finished = once(recorder, 'close');
      let pending = Buffer.alloc(0);
      const samples: number[][] = [];
      const behindTitle: number[][] = [];
      let stderr = '';
      recorder.stderr.on('data', (chunk: Buffer) => {
        stderr += chunk.toString();
      });
      recorder.stdout.on('data', (chunk: Buffer) => {
        pending = Buffer.concat([pending, chunk]);
        const size = png.width * png.height * 3;
        while (pending.length >= size) {
          samples.push(
            [0.2, 0.4, 0.6].map(
              (x) => pending[(3 * png.width + Math.round(png.width * x)) * 3]
            )
          );
          behindTitle.push(
            [0.2, 0.6].map(
              (x) =>
                pending[
                  (Math.floor(png.height / 2) * png.width +
                    Math.round(png.width * x)) *
                    3
                ]
            )
          );
          pending = pending.subarray(size);
        }
      });
      let receiveFrame = 0;
      let localFrame = 0;
      try {
        await waitForResult(async () =>
          expect(samples.length).toBeGreaterThan(3)
        );
        receiveFrame = samples.length;
        await expectElementKind(
          await app.getById('file_transfer_remote_refresh_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(samples.length).toBeGreaterThan(180);
          expect(samples.slice(-5)).toEqual(
            Array.from({ length: 5 }, () => [32, 32, 32])
          );
        });
        localFrame = samples.length;
        await expectElementKind(
          await app.getById('file_transfer_local_refresh_button'),
          'button'
        ).click();
        const [status] = await finished;
        expect(status, stderr).toBe(0);
      } finally {
        if (recorder.exitCode === null && recorder.signalCode === null) {
          recorder.kill('SIGTERM');
          await finished;
        }
      }
      await evidence.log('FTP receive video', {
        samples,
        receiveFrame,
        localFrame,
      });
      const reception = samples.slice(receiveFrame, localFrame);
      // Empty space beside the title must share the moving header background.
      // A solid custom-title box would leave a rectangular stripe in the video.
      for (let frame = receiveFrame; frame < localFrame; ++frame) {
        expect(
          Math.abs(behindTitle[frame][0] - samples[frame][0])
        ).toBeLessThanOrEqual(1);
        expect(
          Math.abs(behindTitle[frame][1] - samples[frame][2])
        ).toBeLessThanOrEqual(1);
      }
      // Medium uses 15% opacity: the dark background rises by about 33 levels.
      const peak = Math.max(...reception.flat()) - 32;
      expect(peak).toBeGreaterThan(26);
      expect(peak).toBeLessThanOrEqual(35);
      expect(reception.some((frame) => frame[0] > frame[1] + 5)).toBe(true);
      expect(reception.some((frame) => frame[2] > frame[1] + 5)).toBe(true);
      expect(samples.slice(localFrame)).toEqual(
        Array.from({ length: samples.length - localFrame }, () => [32, 32, 32])
      );
      expect(await label.text()).toBe(title);
      expect(
        (serverOutput.match(/COMMAND MLSD/g) ?? []).length
      ).toBeGreaterThanOrEqual(2);
      await evidence.captureEvidence('ftp-receive-settled', () =>
        app.capture()
      );
    } catch (error) {
      if (apps[0])
        await evidence.captureEvidence('ftp-receive-failure', () =>
          apps[0].capture()
        );
      throw error;
    } finally {
      try {
        await evidence.log('FTP server', { output: serverOutput });
        await evidence.flushOutputs(apps, launcher);
      } finally {
        if (launcher) await launcher.release();
        server.kill('SIGTERM');
        await serverFinished;
        await evidence.release();
      }
    }
  });
}, 60_000);
