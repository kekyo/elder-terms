import { execFile, spawn } from 'node:child_process';
import { createRequire } from 'node:module';
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
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkTableElement,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';

// Use the same complete window scenarios for an installed application tree.
const ftpAppPath =
  process.env.ELDER_TERMS_TEST_FTP_APP ??
  fileURLToPath(
    new URL(
      '../../.build/elder-terms-vte/elder-terms-file-transfer',
      import.meta.url
    )
  );
const serverPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/ftp-test-server', import.meta.url)
);
const execute = promisify(execFile);
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

for (const scenario of [
  'passive',
  'active',
  'login-refused',
  'cancel-final',
  'close-final',
  'close-final-download',
] as const) {
  it(`uses the real FTP service through its window: ${scenario}`, async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-ftp-live-'));
    const evidence = createTestEvidence(context);
    const apps: GtkApp[] = [];
    let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
    let server: ReturnType<typeof spawn> | undefined;
    let serverFinished: Promise<number | null> | undefined;
    const events: string[] = [];
    let serverError: Error | undefined;
    let serverLog = '';
    try {
      const configHome = join(directory, 'config');
      const local = join(directory, 'local');
      const remoteRoot = join(directory, 'remote');
      const remote = join(remoteRoot, 'home');
      const configPath = join(directory, 'ftp.ini');
      const closingDownload = scenario === 'close-final-download';
      const held =
        scenario === 'cancel-final' || scenario.startsWith('close-final');
      await Promise.all([
        mkdir(configHome),
        mkdir(local),
        mkdir(join(remote, 'archive'), { recursive: true }),
      ]);
      const upload = Buffer.alloc(2 * 1024 * 1024 + 17);
      for (let index = 0; index < upload.length; index++)
        upload[index] = index % 251;
      const download = Buffer.from('Remote binary\r\n\x00\xff\n', 'latin1');
      await Promise.all([
        writeFile(join(local, 'upload.bin'), upload),
        writeFile(join(remote, 'download.bin'), download),
        writeFile(
          join(remote, 'archive', 'nested.txt'),
          'nested remote file\n'
        ),
      ]);
      server = spawn(
        serverPath,
        [
          remoteRoot,
          '--trace',
          ...(held ? ['--hold-final'] : []),
          ...(scenario === 'login-refused' ? ['--reject-login'] : []),
        ],
        { stdio: ['pipe', 'pipe', 'pipe'] }
      );
      const lines = createInterface({ input: server.stdout! });
      lines.on('line', (line) => events.push(line));
      server.stderr!.on('data', (bytes: Buffer) => {
        serverLog += bytes.toString();
      });
      serverFinished = new Promise((resolve) => {
        server!.once('error', (error) => {
          serverError = error;
          resolve(null);
        });
        server!.once('close', resolve);
      });
      const ready = await waitForResult(async () => {
        if (serverError) throw serverError;
        expect(server!.exitCode, serverLog).toBeNull();
        const line = events.find((value) => value.startsWith('READY '));
        expect(line).toBeDefined();
        return line!;
      });
      await writeFile(
        configPath,
        [
          '[general]',
          'name=Live FTP',
          'type=ftp',
          'background=#183C58',
          '[ftp]',
          'address=127.0.0.1',
          `port=${ready.slice(6)}`,
          'username=alice',
          `data_connection_mode=${scenario === 'active' ? 'active' : 'passive'}`,
          `local_directory=${local}`,
          'remote_directory=/home',
          '',
        ].join('\n')
      );
      launcher = createGtkAppLauncher({
        appPath: ftpAppPath,
        env: { LANGUAGE: 'en', LC_ALL: 'C.UTF-8', XDG_CONFIG_HOME: configHome },
        onSystemOutput: evidence.recordSystemOutputEvent,
        xvfbTrayHost: true,
      });
      const app = await launcher.launch(['-c', configPath], {
        onOutput: evidence.recordAppOutputEvent,
      });
      apps.push(app);
      await expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      ).setText('alice');
      await expectElementKind(
        await app.getById('file_transfer_prompt_secondary_entry'),
        'entry'
      ).setText('secret');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();

      if (scenario === 'login-refused') {
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('file_transfer_prompt_title_label'),
              'label'
            ).text()
          ).toBe('Failed to start FTP');
          expect(
            await expectElementKind(
              await app.getById('file_transfer_prompt_message_label'),
              'label'
            ).text()
          ).toContain('530');
        });
      } else {
        const localTree = expectElementKind(
          await app.getById('file_transfer_local_tree'),
          'table'
        ) as GtkTableElement;
        const remoteTree = expectElementKind(
          await app.getById('file_transfer_remote_tree'),
          'table'
        ) as GtkTableElement;
        const rowFor = async (tree: GtkTableElement, name: string) =>
          await waitForResult(async () => {
            for (let index = 0; index < (await tree.getRowCount()); index++) {
              if ((await (await tree.cellAt(index, 0))?.info())?.name === name)
                return index;
            }
            throw new Error(`Missing FTP row: ${name}`);
          });
        const contextMenu = async (tree: GtkTableElement, name: string) => {
          const row = await rowFor(tree, name);
          await tree.selectRow(row);
          const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
          await app.input.moveMouseTo(
            Math.round(bounds.x + bounds.width / 2),
            Math.round(bounds.y + bounds.height / 2)
          );
          await app.input.setMouseButton('right', true);
          await app.input.setMouseButton('right', false);
        };
        await rowFor(remoteTree, 'download.bin');
        expect(
          await expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          ).text()
        ).toBe('/home');
        await contextMenu(
          closingDownload ? remoteTree : localTree,
          closingDownload ? 'download.bin' : 'upload.bin'
        );
        await expectElementKind(
          await app.getById(
            closingDownload
              ? 'file_transfer_receive_item'
              : 'file_transfer_send_item'
          ),
          'menuItem'
        ).click();
        if (held) {
          await waitForResult(async () => {
            expect(events, serverLog).toContain('FINAL_WAIT');
          });
          const overlay = await app.getById('file_transfer_overlay');
          expect((await overlay.info()).states).toContain('showing');
          const destination = closingDownload ? local : remote;
          expect(await readdir(destination)).not.toContain(
            closingDownload ? 'download.bin' : 'upload.bin'
          );
          await waitForResult(async () => {
            expect(
              (await readdir(destination)).some((name) =>
                name.includes('.elder-terms-part-')
              )
            ).toBe(true);
          });
          await evidence.captureEvidence('ftp-final-response-held', async () =>
            overlay.capture()
          );
          if (scenario === 'cancel-final') {
            const screen = PNG.sync.read((await app.capture()).image);
            const area = (await overlay.capture()).bounds;
            const environment = await app.environment();
            const videoPath = join(evidence.directory, 'ftp-cancellation.mkv');
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
              const pixels = await waitForResult(async () => {
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
                if (states.length > 0)
                  expect(current.equals(states[states.length - 1].pixels)).toBe(
                    false
                  );
                expect(previous).toBeDefined();
                expect(current.equals(previous!)).toBe(true);
                return current;
              });
              const previous = frameNumber;
              const ordinal = await waitForResult(async () => {
                if (recordingError) throw recordingError;
                expect(recorder.exitCode, stderr).toBeNull();
                expect(frameNumber).toBeGreaterThan(previous);
                expect(
                  difference(frame, screen.width, true, pixels)
                ).toBeLessThan(0.01);
                return frameNumber;
              });
              states.push({ frame: ordinal, label, pixels });
            };
            try {
              await expectVideoState('waiting for the final FTP response');
              await expectElementKind(
                await app.getById('file_transfer_cancel_button'),
                'button'
              ).click();
              // The server still withholds 226 and waits for the client's EOF.
              // Cancellation must reach GTK and terminate the operation itself.
              server.stdin!.write('cancel\n');
              await waitForResult(async () => {
                expect((await overlay.info()).states).not.toContain('showing');
              });
              expect(await readdir(remote)).not.toContain('upload.bin');
              await expectVideoState('canceled with no committed destination');
              expect(states[1].frame).toBeGreaterThan(states[0].frame);
              expect(states[1].pixels.equals(states[0].pixels)).toBe(false);
            } finally {
              recorder.stdin.write('q\n');
              const code = await finished;
              await evidence.log('FTP cancellation video states', {
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
              join(evidence.directory, 'ftp-frame-%d.png'),
            ]);
            for (let index = 0; index < states.length; index++) {
              const saved = PNG.sync.read(
                await readFile(
                  join(evidence.directory, `ftp-frame-${index + 1}.png`)
                )
              );
              expect(
                difference(saved.data, saved.width, false, states[index].pixels)
              ).toBeLessThan(0.01);
            }
          }
        } else {
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_status_label'),
                'label'
              ).text()
            ).toBe('Sent 1 item');
          });
          expect(await readFile(join(remote, 'upload.bin'))).toEqual(upload);
          await rowFor(remoteTree, 'upload.bin');
          await contextMenu(remoteTree, 'download.bin');
          await expectElementKind(
            await app.getById('file_transfer_receive_item'),
            'menuItem'
          ).click();
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_status_label'),
                'label'
              ).text()
            ).toBe('Received 1 item');
          });
          expect(await readFile(join(local, 'download.bin'))).toEqual(download);
          await contextMenu(remoteTree, 'upload.bin');
          await expectElementKind(
            await app.getById('file_transfer_remote_rename_item'),
            'menuItem'
          ).click();
          await expectElementKind(
            await app.getById('file_transfer_prompt_entry'),
            'entry'
          ).setText('renamed 日本語.bin');
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
          await rowFor(remoteTree, 'renamed 日本語.bin');
          expect(await readFile(join(remote, 'renamed 日本語.bin'))).toEqual(
            upload
          );
          await contextMenu(remoteTree, 'renamed 日本語.bin');
          await expectElementKind(
            await app.getById('file_transfer_remote_delete_item'),
            'menuItem'
          ).click();
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_status_label'),
                'label'
              ).text()
            ).toBe('Deleted 1 item');
          });
          expect(await readdir(remote)).not.toContain('renamed 日本語.bin');
          expect(
            (await readdir(remote)).some((name) =>
              name.includes('.elder-terms-part-')
            )
          ).toBe(false);
          // Navigate a real server directory using the existing path entry.
          const path = expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          );
          await path.setText('/home/archive');
          const pathBounds = (await path.capture()).bounds;
          await app.input.moveMouseTo(
            Math.round(pathBounds.x + pathBounds.width / 2),
            Math.round(pathBounds.y + pathBounds.height / 2)
          );
          await app.input.setMouseButton('left', true);
          await app.input.setMouseButton('left', false);
          await app.input.pressKey('Return');
          await rowFor(remoteTree, 'nested.txt');
          expect(await path.text()).toBe('/home/archive');
        }
      }
      const window = expectElementKind(
        await app.getById('file_transfer_window'),
        'window'
      );
      // Xvfb has no window manager to handle Alt+F4. Exercise the same
      // title-bar action as an ordinary close, including its GTK destroy path.
      const clickClose = async (root: GtkWidgetElement) => {
        const pending: GtkWidgetElement[] = [root];
        while (pending.length > 0) {
          const widget = pending.shift()!;
          if (
            widget.kind === 'button' &&
            (await widget.info()).name === 'Close'
          ) {
            await widget.click();
            return;
          }
          if ('getChildCount' in widget) {
            const count = await widget.getChildCount();
            for (let index = 0; index < count; index++) {
              const child = await widget.childAt(index);
              if (child !== undefined) pending.push(child);
            }
          }
        }
        throw new Error('The GTK Close button is missing');
      };
      if (scenario === 'cancel-final') {
        // Refresh reports the closed connection after an interrupted transfer.
        // Dismiss that modal notice before interacting with the title bar.
        await clickClose(
          await app.getById('file_transfer_operation_error_dialog')
        );
      }
      await evidence.captureEvidence('ftp-before-window-close', async () =>
        window.capture()
      );
      await clickClose(await app.getById('file_transfer_header_bar'));
      if (scenario.startsWith('close-final')) server.stdin!.write('cancel\n');
      await waitForResult(async () => {
        const output = await app.output();
        expect(output.exitCode, output.stderr).toBe(0);
        expect(output.exitSignal).toBeNull();
      });
      if (scenario === 'close-final-download') {
        const remaining = await readdir(local);
        expect(remaining).not.toContain('download.bin');
        expect(
          remaining.some((name) => name.includes('.elder-terms-part-'))
        ).toBe(false);
      }
    } finally {
      try {
        await evidence.log('FTP server events', { events, stderr: serverLog });
        await evidence.flushOutputs(apps, launcher);
      } finally {
        if (launcher) await launcher.release();
        if (server) {
          server.stdin?.end();
          server.kill('SIGTERM');
        }
        if (serverFinished) await serverFinished;
        await evidence.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  }, 120_000);
}
