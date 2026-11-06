// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { join } from 'node:path';
import { expect, it } from 'vitest';
import { toPass, waitForResult } from 'gestament/testing';
import {
  withTemporaryDirectory,
  capturePixel,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';
import { runGtkTest } from './gtk-test-helpers.ts';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const execute = promisify(execFile);

it('renders a named theme titlebar and background from separate configuration files', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const configDirectory = join(directory, 'elder-terms');
    await mkdir(configDirectory);
    await writeFile(
      join(configDirectory, 'global.ini'),
      '[general]\nbackground=#111111\n'
    );
    await writeFile(
      join(configDirectory, 'theme.ini'),
      '[theme.red]\nname=赤テーマ\n[theme.red.general]\nexterior_background=#7A2222\nbackground=#281818\n'
    );
    const connection = join(directory, 'connection.ini');
    await writeFile(connection, '[general]\ntheme=red\nname=Named red theme\n');
    await runGtkTest(
      context,
      ['-c', connection],
      async (app, evidence) => {
        await toPass(async () => {
          expect(
            capturePixel(
              await (await app.getById('header_bar')).capture(),
              0.5,
              0.1
            )
          ).toEqual([0x7a, 0x22, 0x22]);
          expect(
            capturePixel(
              await (await app.getById('terminal_view')).capture(),
              0.9,
              0.9
            )
          ).toEqual([0x28, 0x18, 0x18]);
        });
        await evidence.captureEvidence('named-red-theme', async () =>
          (await app.getById('main_window')).capture()
        );
      },
      { env: { XDG_CONFIG_HOME: directory } }
    );
  });
});

it('records unchanged preview and cancel, explicit runtime Apply, and refreshed theme definitions', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const configDirectory = join(directory, 'elder-terms');
    await mkdir(configDirectory);
    const themes = join(configDirectory, 'theme.ini');
    const definitions =
      '[theme.red]\nname=Red\n[theme.red.general]\nbackground=#281818\nexterior_background=#7A2222\n' +
      '[theme.green]\nname=Green\n[theme.green.general]\nbackground=#182818\nexterior_background=#227A22\n';
    await writeFile(themes, definitions);
    const connection = join(directory, 'runtime.ini');
    await writeFile(connection, '[general]\ntheme=red\n');
    await runGtkTest(
      context,
      ['-c', connection],
      async (app, evidence) => {
        const screen = PNG.sync.read((await app.capture()).image);
        const window = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        const bounds = await window.bounds();
        await window.moveTo(screen.width - bounds.width, 0);
        const area = (await (await app.getById('terminal_view')).capture())
          .bounds;
        const x = area.x + Math.floor(area.width * 0.95);
        const y = area.y + Math.floor(area.height * 0.9);
        const environment = await app.environment();
        const video = join(evidence.directory, 'named-theme-runtime.mkv');
        const frameSize = screen.width * screen.height * 4;
        let pending = Buffer.alloc(0);
        let frame = Buffer.alloc(0);
        let frameNumber = 0;
        let recordingError: Error | undefined;
        let stderr = '';
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
            '-threads',
            '1',
            '-pix_fmt',
            'bgr0',
            '-fps_mode',
            'passthrough',
            video,
            '-map',
            '0:v',
            '-c:v',
            'rawvideo',
            '-threads',
            '1',
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
        const states: { frame: number; color: number[]; name: string }[] = [];
        const observe = async (
          name: string,
          color: number[]
        ): Promise<void> => {
          const previous = frameNumber;
          const matched = await waitForResult(async () => {
            if (recordingError) throw recordingError;
            expect(recorder.exitCode, stderr).toBeNull();
            expect(frameNumber).toBeGreaterThan(previous);
            const offset = (y * screen.width + x) * 4;
            expect(
              [frame[offset + 2], frame[offset + 1], frame[offset]],
              name
            ).toEqual(color);
            return frameNumber;
          });
          states.push({ frame: matched, color, name });
        };
        const open = async (): Promise<void> => {
          await expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          ).click();
          await expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          ).click();
          const dialog = expectElementKind(
            await app.getById('settings_dialog'),
            'window'
          );
          await dialog.moveTo(0, 0);
          expect((await dialog.bounds()).width).toBeLessThan(x);
        };
        const cancel = async (): Promise<void> => {
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
        };
        try {
          await observe('initial red', [40, 24, 24]);
          await open();
          await expectElementKind(
            await app.getById('settings_general_theme_combo'),
            'comboBox'
          ).selectChildAt(2);
          // The existing modal overlay dims the owner to 65% while editing.
          await observe('green selection remains a draft', [26, 16, 16]);
          await cancel();
          await observe('cancel keeps red', [40, 24, 24]);
          await open();
          await expectElementKind(
            await app.getById('settings_general_theme_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await cancel();
          await observe('Apply changes to green', [24, 40, 24]);
          await evidence.captureEvidence('applied-green-theme', async () =>
            window.capture()
          );
          await writeFile(
            themes,
            definitions.replace('background=#182818', 'background=#181828')
          );
          await open();
          await observe('reloaded definition waits for Apply', [16, 26, 16]);
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await cancel();
          await observe('Apply uses the latest definition', [24, 24, 40]);
          expect(await readFile(connection, 'utf8')).toContain('theme=red');
        } finally {
          recorder.stdin.write('q\n');
          expect(await finished, stderr).toBe(0);
          await evidence.log('theme runtime video states', states);
        }
        await execute('ffmpeg', [
          '-v',
          'error',
          '-i',
          video,
          '-vf',
          `select=${states.map(({ frame }) => `eq(n\\,${frame - 1})`).join('+')}`,
          '-fps_mode',
          'passthrough',
          join(evidence.directory, 'theme-frame-%d.png'),
        ]);
        for (let index = 0; index < states.length; index++) {
          const saved = PNG.sync.read(
            await readFile(
              join(evidence.directory, `theme-frame-${index + 1}.png`)
            )
          );
          const offset = (y * saved.width + x) * 4;
          expect(
            Array.from(saved.data.subarray(offset, offset + 3)),
            states[index].name
          ).toEqual(states[index].color);
        }
      },
      { env: { XDG_CONFIG_HOME: directory } }
    );
  });
});
