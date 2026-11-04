// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn, type ChildProcess } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import {
  withTemporaryDirectory,
  createTestEvidence,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';
import { createWebdavTestServer } from './webdav-test-server.ts';

for (const language of ['en', 'ja']) {
  it(`records direct authentication retries and recovery in ${language}`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const server = await createWebdavTestServer('basic', undefined);
      const evidence = createTestEvidence(context);
      const launcher = createGtkAppLauncher({
        appPath: fileURLToPath(
          new URL(
            '../../.build/elder-terms-file-transfer/elder-terms-file-transfer',
            import.meta.url
          )
        ),
        env: {
          LANGUAGE: language,
          LC_ALL: language === 'ja' ? 'ja_JP.UTF-8' : 'C.UTF-8',
          XDG_CONFIG_HOME: join(directory, 'config'),
          XDG_DATA_HOME: join(directory, 'data'),
        },
        onSystemOutput: evidence.recordSystemOutputEvent,
        xvfbTrayHost: true,
      });
      let recorder: ChildProcess | undefined;
      let recorded: Promise<number | null> | undefined;
      try {
        const config = join(directory, 'webdav.ini');
        await writeFile(
          config,
          `[general]\ntype=webdav\nauto_close=true\n[webdav]\nscheme=http\nauthentication=basic\nusername=alice\naddress=127.0.0.1\nport=${server.port}\nbase_path=/dav/\nlocal_directory=${directory}\n`
        );
        const app = await launcher.launch(['-c', config], {
          onOutput: evidence.recordAppOutputEvent,
        });
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
            '12',
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
            join(evidence.directory, 'reconnect.mkv'),
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
            const count = Math.min(
              bytes.length - offset,
              frameBytes - buffered
            );
            bytes.copy(pending, buffered, offset, offset + count);
            buffered += count;
            offset += count;
            if (buffered === frameBytes) {
              latest = pending;
              pending = Buffer.allocUnsafe(frameBytes);
              buffered = 0;
              ++frames;
            }
          }
        });
        const observed: { name: string; frame: number }[] = [];
        const observe = async (name: string, widget: GtkWidgetElement) => {
          const before = frames;
          const capture = await waitForResult(async () => {
            expect(frames, recordingError).toBeGreaterThan(before);
            const area = (await widget.capture()).bounds;
            const capture = await app.capture();
            const expected = PNG.sync.read(capture.image);
            let matching = 0,
              samples = 0;
            for (let y = area.y + 2; y < area.y + area.height - 2; y += 2)
              for (let x = area.x + 2; x < area.x + area.width - 2; x += 2) {
                const index = (y * screen.width + x) * 4;
                if (
                  Math.abs(latest[index]! - expected.data[index + 2]!) < 8 &&
                  Math.abs(latest[index + 1]! - expected.data[index + 1]!) <
                    8 &&
                  Math.abs(latest[index + 2]! - expected.data[index]!) < 8
                )
                  ++matching;
                ++samples;
              }
            expect(matching / samples).toBeGreaterThan(0.995);
            return capture;
          });
          await evidence.captureEvidence(name, async () => capture);
          observed.push({ name, frame: frames });
        };
        const prompt = await app.getById('file_transfer_prompt_panel');
        const entry = expectElementKind(
          await app.getById('file_transfer_prompt_entry'),
          'entry'
        );
        const accept = expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        );
        const notice = await app.getById(
          'file_transfer_connection_notice_panel'
        );
        const reconnect = expectElementKind(
          await app.getById('file_transfer_reconnect_button'),
          'button'
        );
        for (let attempt = 0; attempt < 3; ++attempt) {
          await waitForResult(async () => {
            expect((await prompt.info()).states).toContain('showing');
            expect(await entry.text()).toBe('alice');
          });
          if (attempt !== 0) {
            expect((await notice.info()).states).not.toContain('showing');
            await observe(`authentication-${attempt}`, prompt);
          }
          await accept.click();
          await waitForResult(async () =>
            expect(
              await expectElementKind(
                await app.getById('file_transfer_prompt_message_label'),
                'label'
              ).text()
            ).toBe(language === 'ja' ? 'パスワード:' : 'Password:')
          );
          await entry.setText(attempt === 2 ? 'secret' : 'wrong-password');
          await accept.click();
          if (attempt === 2) break;
          await waitForResult(async () => {
            expect((await notice.info()).states).toContain('showing');
            expect((await reconnect.info()).states).toContain('sensitive');
            expect((await reconnect.info()).name).toBe(
              language === 'ja' ? '再接続' : 'Reconnect'
            );
            expect(
              await expectElementKind(
                await app.getById(
                  'file_transfer_connection_notice_title_label'
                ),
                'label'
              ).text()
            ).toBe(
              language === 'ja'
                ? 'WebDAV接続を開始できませんでした'
                : 'Failed to start WebDAV'
            );
            expect(
              await expectElementKind(
                await app.getById(
                  'file_transfer_connection_notice_message_label'
                ),
                'label'
              ).text()
            ).toContain('401');
            expect((await app.output()).exitCode).toBeNull();
          });
          const panelBounds = (await notice.capture()).bounds;
          const buttonBounds = (await reconnect.capture()).bounds;
          const localBounds = (
            await (await app.getById('file_transfer_local_tree')).capture()
          ).bounds;
          expect(panelBounds.x).toBeGreaterThanOrEqual(
            localBounds.x + localBounds.width
          );
          expect(buttonBounds.x).toBeGreaterThanOrEqual(panelBounds.x);
          expect(buttonBounds.y).toBeGreaterThanOrEqual(panelBounds.y);
          expect(buttonBounds.x + buttonBounds.width).toBeLessThanOrEqual(
            panelBounds.x + panelBounds.width
          );
          expect(buttonBounds.y + buttonBounds.height).toBeLessThanOrEqual(
            panelBounds.y + panelBounds.height
          );
          await observe(`failed-${attempt}`, notice);
          await reconnect.click();
        }
        const tree = expectElementKind(
          await app.getById('file_transfer_remote_tree'),
          'table'
        );
        await waitForResult(async () => {
          expect((await notice.info()).states).not.toContain('showing');
          expect((await tree.info()).states).toContain('sensitive');
          const names: string[] = [];
          for (let row = 0; row < (await tree.getRowCount()); ++row)
            names.push((await (await tree.cellAt(row, 0))?.info())?.name ?? '');
          expect(names).toContain('hello.txt');
        });
        await observe('reconnected', tree);
        expect(observed.map((item) => item.name)).toEqual([
          'failed-0',
          'authentication-1',
          'failed-1',
          'authentication-2',
          'reconnected',
        ]);
        expect(
          observed.every(
            (item, index) =>
              index === 0 || item.frame > observed[index - 1]!.frame
          )
        ).toBe(true);
        await evidence.log('Recorded reconnect transitions', observed);
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
        await server.close();
        await evidence.release();
      }
    });
  });
}
