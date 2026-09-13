import { execFile, spawn } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const execute = promisify(execFile);

describe('Modal owner clicks', () => {
  it('raises the dialog on owner clicks without requiring a focus change', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[terminal]\nwidth=88\n'
        );
      },
      async ({ app, x11MapRecorder }) => {
        if (!x11MapRecorder) throw new Error('Focus recorder is required');
        const owner = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        await owner.moveTo(20, 20);
        const ownerId = (await owner.x11Info()).windowId;
        const button = await app.getById('global_defaults_button');
        const buttonBounds = (await button.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round(buttonBounds.x + buttonBounds.width / 2),
          Math.round(buttonBounds.y + buttonBounds.height / 2)
        );
        // First prove the same native input opens a normal, sensitive button.
        await x11MapRecorder.clickWindowWithoutFocus(ownerId);
        const dialog = expectElementKind(
          await app.getById('global_defaults_dialog'),
          'window'
        );
        await dialog.moveTo(420, 100);
        const dialogId = String(
          Number.parseInt((await dialog.x11Info()).windowId, 16)
        );
        await waitForResult(async () =>
          expect(await x11MapRecorder.focusedWindow()).toBe(dialogId)
        );
        const environment = { ...process.env, ...(await app.environment()) };
        const bounds = await dialog.bounds();
        const area = {
          x: bounds.x + 30,
          y: bounds.y + 10,
          width: 240,
          height: 15,
        };
        const screen = PNG.sync.read((await app.capture()).image);
        const reference = Buffer.alloc(area.width * area.height * 3);
        for (let y = 0; y < area.height; y++) {
          for (let x = 0; x < area.width; x++) {
            const offset = ((area.y + y) * screen.width + area.x + x) * 4;
            for (let channel = 0; channel < 3; channel++)
              reference[(y * area.width + x) * 3 + channel] =
                screen.data[offset + channel];
          }
        }
        const directory = fileURLToPath(
          new URL(
            `../../test-results/modal-click-${Date.now()}/`,
            import.meta.url
          )
        );
        await mkdir(directory, { recursive: true });
        const recorder = spawn(
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
            environment.DISPLAY ?? '',
            '-map',
            '0:v',
            '-c:v',
            'ffv1',
            '-pix_fmt',
            'bgr0',
            '-fps_mode',
            'passthrough',
            join(directory, 'owner-clicks.mkv'),
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
        let stderr = '';
        let recordingError: Error | undefined;
        let pending = Buffer.alloc(0);
        let frame = Buffer.alloc(0);
        let frameNumber = 0;
        const frameSize = screen.width * screen.height * 4;
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
        const states: { label: string; frame: number }[] = [];
        const expectVideo = async (label: string, visible: boolean) => {
          const previous = frameNumber;
          await waitForResult(async () => {
            if (recordingError) throw recordingError;
            expect(recorder.exitCode, stderr).toBeNull();
            expect(frameNumber).toBeGreaterThan(previous);
            let different = 0;
            for (let y = 0; y < area.height; y++) {
              for (let x = 0; x < area.width; x++) {
                const offset = ((area.y + y) * screen.width + area.x + x) * 4;
                if (
                  [0, 1, 2].some(
                    (channel) =>
                      Math.abs(
                        frame[offset + 2 - channel] -
                          reference[(y * area.width + x) * 3 + channel]
                      ) > 8
                  )
                )
                  different++;
              }
            }
            const ratio = different / (area.width * area.height);
            if (visible) expect(ratio).toBeLessThan(0.02);
            else expect(ratio).toBeGreaterThan(0.1);
          });
          states.push({ label, frame: frameNumber });
        };
        try {
          await expectVideo('dialog initially visible', true);
          const ownerBounds = await owner.bounds();
          await app.input.moveMouseTo(ownerBounds.x + 20, ownerBounds.y + 250);
          const competitor = await x11MapRecorder.focusCompetitor();
          await execute(
            'xdotool',
            [
              'windowmove',
              competitor,
              String(bounds.x + 10),
              String(bounds.y - 20),
            ],
            { env: environment }
          );
          await expectVideo('dialog covered by independent window', false);
          // Deliver a native button event without the WM first focusing the owner.
          // This isolates the click path from the existing focus-in fallback.
          expect(await x11MapRecorder.focusedWindow()).toBe(competitor);
          await x11MapRecorder.clickWindowWithoutFocus(ownerId);
          await waitForResult(async () =>
            expect(await x11MapRecorder.focusedWindow()).toBe(dialogId)
          );
          await expectVideo(
            'dialog raised by owner click without focus-in',
            true
          );
          for (const y of [20, 250, 250]) {
            await x11MapRecorder.focusCompetitor();
            await app.input.moveMouseTo(ownerBounds.x + 20, ownerBounds.y + y);
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await waitForResult(async () =>
              expect(await x11MapRecorder.focusedWindow()).toBe(dialogId)
            );
            expect((await owner.info()).states).not.toContain('enabled');
            expect(await app.getWindowCount()).toBe(2);
            await expectVideo(`dialog raised after native click at ${y}`, true);
          }
          await expectElementKind(
            await app.getById('global_defaults_cancel_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(await app.getWindowCount()).toBe(1);
            expect((await owner.info()).states).toContain('enabled');
          });
        } finally {
          recorder.stdin.write('q\n');
          const code = await finished;
          await writeFile(
            join(directory, 'states.json'),
            JSON.stringify({ states, code, stderr }, null, 2)
          );
          expect(code, stderr).toBe(0);
        }
      },
      {
        args: [],
        env: { LANGUAGE: 'en', LC_ALL: 'C.UTF-8' },
        recordX11Maps: true,
      }
    );
  });
});
