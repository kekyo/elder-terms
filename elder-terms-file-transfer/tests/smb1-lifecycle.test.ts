// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createGtkAppLauncher, type GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { selectCurrentTransferDirectory } from './file-transfer-destination-helpers.ts';
import {
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

const execute = promisify(execFile);
it.for(['cancel', 'transfer', 'decision', 'disconnected'])(
  'closes SMB1 while %s without leaving a worker',
  { timeout: 120000 },
  async (scenario, context) => {
    const fixture = fileURLToPath(new URL('./smb/', import.meta.url));
    if (!process.env.ELDER_SMB1_NAMESPACE) {
      const child = spawn(
        process.execPath,
        [join(fixture, 'run-smb1-window.mts'), scenario],
        { stdio: 'inherit' }
      );
      const [status] = await once(child, 'exit');
      expect(status).toBe(0);
      return;
    }
    const directory = await mkdtemp(join(tmpdir(), 'elder-smb1-lifecycle-'));
    const local = join(directory, 'local');
    await mkdir(local);
    if (scenario === 'decision')
      await writeFile(join(local, 'single.txt'), 'keep this local content');
    const evidence = createTestEvidence(context);
    const relay = spawn(process.execPath, [join(fixture, 'smb1-relay.mts')], {
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    let relayOutput = '';
    relay.stdout.on('data', (bytes: Buffer) => {
      relayOutput += bytes;
    });
    relay.stderr.on('data', (bytes: Buffer) => {
      relayOutput += bytes;
    });
    const config = join(directory, 'connection.ini');
    await writeFile(
      config,
      `[general]\ntype=smb\nauto_close=false\n[smb]\nversion=1\naddress=127.0.0.1\nusername=elder\nlocal_directory=${local}\nremote_directory=/probe\n`
    );
    const executable =
      process.env.ELDER_TERMS_TEST_FTP_APP ??
      fileURLToPath(
        new URL(
          '../../.build/elder-terms-file-transfer/elder-terms-file-transfer',
          import.meta.url
        )
      );
    const pidFile = join(directory, 'application.pid');
    const wrapper = join(directory, 'run-app');
    const quote = (text: string) => "'" + text.replace(/'/g, "'\\''") + "'";
    await writeFile(
      wrapper,
      '#!/bin/sh\necho $$ > ' +
        quote(pidFile) +
        '\nexec ' +
        quote(executable) +
        ' "$@"\n',
      { mode: 0o700 }
    );
    const launcher = createGtkAppLauncher({
      appPath: wrapper,
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
      await waitForResult(async () =>
        expect(relayOutput).toContain('READY relay')
      );
      const app = await launcher.launch(['-c', config], {
        onOutput: evidence.recordAppOutputEvent,
      });
      const prompt = expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      );
      const accept = expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      );
      await waitForResult(async () =>
        expect(await prompt.text()).toBe('elder')
      );
      await accept.click();
      await waitForResult(async () =>
        expect(
          await expectElementKind(
            await app.getById('file_transfer_prompt_message_label'),
            'label'
          ).text()
        ).toBe('Password:')
      );
      await prompt.setText(process.env.ELDER_SMB_UI_PASSWORD!);
      await accept.click();
      await waitForResult(async () =>
        expect(
          await expectElementKind(
            await app.getById('file_transfer_status_label'),
            'label'
          ).text()
        ).toBe('Ready')
      );
      const parent = (await readFile(pidFile, 'utf8')).trim();
      const workers = (
        await readFile(`/proc/${parent}/task/${parent}/children`, 'utf8')
      )
        .trim()
        .split(/\s+/);
      expect(workers).toHaveLength(1);
      const worker = Number(workers[0]);
      if (scenario === 'disconnected') {
        process.kill(worker, 'SIGKILL');
        await expectElementKind(
          await app.getById('file_transfer_remote_refresh_button'),
          'button'
        ).click();
        await waitForResult(async () =>
          expect(
            (await (await app.getById('file_transfer_reconnect_button')).info())
              .states
          ).toContain('showing')
        );
      } else {
        if (scenario !== 'decision') {
          relay.stdin.write('cancel');
          await waitForResult(async () =>
            expect(relayOutput).toContain('ARMED')
          );
        }
        await selectCurrentTransferDirectory(app, false);
        const tree = expectElementKind(
          await app.getById('file_transfer_remote_tree'),
          'table'
        );
        const row = await waitForResult(async () => {
          for (let index = 0; index < (await tree.getRowCount()); ++index)
            if (
              (await (await tree.cellAt(index, 0))?.info())?.name ===
              'single.txt'
            )
              return index;
          throw new Error('Missing single.txt');
        });
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
        if (scenario === 'decision') {
          await waitForResult(async () =>
            expect(
              await expectElementKind(
                await app.getById('file_transfer_prompt_title_label'),
                'label'
              ).text()
            ).toBe('Destination already exists')
          );
        } else {
          await waitForResult(async () =>
            expect(relayOutput).toContain('HELD')
          );
          expect(
            await expectElementKind(
              await app.getById('file_transfer_job_1_state'),
              'label'
            ).text()
          ).toBe('Transferring');
        }
      }
      if (scenario === 'cancel') {
        const overlay = await app.getById('file_transfer_job_1_progress');
        const screen = PNG.sync.read((await app.capture()).image);
        const area = (await overlay.capture()).bounds;
        const environment = await app.environment();
        const videoPath = join(evidence.directory, 'smb1-cancellation.mkv');
        let pending = Buffer.alloc(0);
        let frame = Buffer.alloc(0);
        let frameNumber = 0;
        let recordingError: Error | undefined;
        let stderr = '';
        const frameSize = screen.width * screen.height * 4;
        const recorder = spawn(
          'ffmpeg',
          [
            '-hide_banner',
            '-loglevel',
            'error',
            '-f',
            'x11grab',
            '-framerate',
            '30',
            '-draw_mouse',
            '0',
            '-video_size',
            `${screen.width}x${screen.height}`,
            '-i',
            environment.DISPLAY ?? '',
            '-map',
            '0:v',
            '-c:v',
            'ffv1',
            '-pix_fmt',
            'bgr0',
            '-fps_mode',
            'passthrough',
            videoPath,
            '-map',
            '0:v',
            '-c:v',
            'rawvideo',
            '-pix_fmt',
            'bgr0',
            '-fps_mode',
            'passthrough',
            '-f',
            'rawvideo',
            'pipe:1',
          ],
          { env: environment, stdio: ['pipe', 'pipe', 'pipe'] }
        );
        const finished = new Promise<number | null>((resolve) => {
          recorder.once('error', (error) => {
            recordingError = error;
            resolve(null);
          });
          recorder.once('close', resolve);
        });
        recorder.stderr.on('data', (bytes: Buffer) => {
          stderr += bytes.toString();
        });
        recorder.stdout.on('data', (bytes: Buffer) => {
          pending = Buffer.concat([pending, bytes]);
          while (pending.length >= frameSize) {
            frame = Buffer.from(pending.subarray(0, frameSize));
            pending = pending.subarray(frameSize);
            frameNumber++;
          }
        });
        const states: {
          readonly frame: number;
          readonly label: string;
          readonly pixels: Buffer;
        }[] = [];
        const difference = (
          image: Buffer,
          width: number,
          bgr: boolean,
          reference: Buffer
        ) => {
          let different = 0;
          for (let y = 0; y < area.height; y++) {
            for (let x = 0; x < area.width; x++) {
              const actual = ((area.y + y) * width + area.x + x) * 4;
              const expected = (y * area.width + x) * 4;
              if (
                [0, 1, 2].some(
                  (channel) =>
                    Math.abs(
                      image[actual + (bgr ? 2 - channel : channel)] -
                        reference[expected + channel]
                    ) > 8
                )
              )
                different++;
            }
          }
          return different / (area.width * area.height);
        };
        const expectVideoState = async (label: string) => {
          let previousPixels: Buffer | undefined;
          const previousFrame = frameNumber;
          const state = await waitForResult(async () => {
            if (recordingError) throw recordingError;
            expect(recorder.exitCode, stderr).toBeNull();
            const captured = PNG.sync.read((await app.capture()).image);
            const current = Buffer.alloc(area.width * area.height * 4);
            for (let y = 0; y < area.height; y++) {
              const offset = ((area.y + y) * captured.width + area.x) * 4;
              captured.data.copy(
                current,
                y * area.width * 4,
                offset,
                offset + area.width * 4
              );
            }
            const previous = previousPixels;
            previousPixels = current;
            if (states.length > 0) {
              // Verify that the recorded progress changes to the terminal result.
              expect(
                difference(
                  captured.data,
                  captured.width,
                  false,
                  states[states.length - 1].pixels
                )
              ).toBeGreaterThan(0.02);
            }
            expect(previous).toBeDefined();
            expect(current.equals(previous!)).toBe(true);
            // Recapture when rendering advances before the video catches up.
            // A transient stable screenshot must not become a permanent target.
            expect(frameNumber).toBeGreaterThan(previousFrame);
            expect(difference(frame, screen.width, true, current)).toBeLessThan(
              0.01
            );
            return { frame: frameNumber, label, pixels: current };
          });
          states.push(state);
        };
        try {
          await expectVideoState('waiting for the SMB1 read response');
          await expectElementKind(
            await app.getById('file_transfer_job_1_cancel'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_job_1_state'),
                'label'
              ).text()
            ).toBe('Cancelled');
          });
          await expect(
            readFile(join(local, 'single.txt'))
          ).rejects.toMatchObject({ code: 'ENOENT' });
          await expectVideoState('canceled with no committed destination');
          expect(states[1].frame).toBeGreaterThan(states[0].frame);
          expect(states[1].pixels.equals(states[0].pixels)).toBe(false);
        } finally {
          recorder.stdin.write('q\n');
          const code = await finished;
          await evidence.log('SMB1 cancellation video states', {
            states: states.map(({ frame, label }) => ({ frame, label })),
            code,
            stderr,
          });
          expect(code, stderr).toBe(0);
        }
        const selection = states
          .map(({ frame }) => `eq(n\\,${frame - 1})`)
          .join('+');
        await execute('ffmpeg', [
          '-v',
          'error',
          '-i',
          videoPath,
          '-vf',
          `select=${selection}`,
          '-fps_mode',
          'passthrough',
          join(evidence.directory, 'smb1-frame-%d.png'),
        ]);
        for (let index = 0; index < states.length; index++) {
          const saved = PNG.sync.read(
            await readFile(
              join(evidence.directory, `smb1-frame-${index + 1}.png`)
            )
          );
          expect(
            difference(saved.data, saved.width, false, states[index].pixels)
          ).toBeLessThan(0.01);
        }
      }
      await evidence.captureEvidence('before-close', async () => app.capture());
      const window = await app.getById('file_transfer_window');
      const widgets: GtkWidgetElement[] = [window];
      let clicked = false;
      while (widgets.length > 0) {
        const widget = widgets.shift()!;
        if (
          widget.kind === 'button' &&
          (await widget.info()).name === 'Close'
        ) {
          await widget.click();
          clicked = true;
          break;
        }
        if ('getChildCount' in widget) {
          for (let index = 0; index < (await widget.getChildCount()); ++index) {
            const child = await widget.childAt(index);
            if (child) widgets.push(child);
          }
        }
      }
      expect(clicked).toBe(true);
      await waitForResult(async () => {
        for (const pid of [Number(parent), worker]) {
          let gone = false;
          try {
            process.kill(pid, 0);
          } catch (error) {
            gone = (error as NodeJS.ErrnoException).code === 'ESRCH';
          }
          expect(gone, `Process ${pid} must be reaped`).toBe(true);
        }
      });
      if (scenario === 'decision')
        expect(await readFile(join(local, 'single.txt'), 'utf8')).toBe(
          'keep this local content'
        );
    } finally {
      await evidence.flushOutputs([], launcher);
      await launcher.release();
      await evidence.release();
      relay.stdin.end();
      if (relay.exitCode === null && relay.signalCode === null)
        await once(relay, 'exit');
      await rm(directory, { recursive: true, force: true });
    }
  }
);
