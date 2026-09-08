import { execFile, spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { readFile, writeFile } from 'node:fs/promises';
import { createServer, type Socket } from 'node:net';
import { join } from 'node:path';
import { promisify } from 'node:util';
import type {
  GtkApp,
  GtkCaptureBounds,
  GtkWidgetElement,
  GtkWidgetKind,
} from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { capturePixel, expectElementKind } from './test-helpers';
import {
  expectActivityIndicatorImageState,
  serialActivityIndicatorIds,
  waitForActivityIndicatorImageState,
} from './activity-indicator-test-helpers';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const execute = promisify(execFile);

const findNamedChild = async (
  root: GtkWidgetElement,
  kind: GtkWidgetKind,
  name: string
): Promise<GtkWidgetElement | undefined> => {
  if (root.kind === kind && (await root.info()).name === name) return root;
  if ('getChildCount' in root && 'childAt' in root) {
    for (let index = 0; index < (await root.getChildCount()); index += 1) {
      const child = await root.childAt(index);
      if (child === undefined) continue;
      const found = await findNamedChild(child, kind, name);
      if (found !== undefined) return found;
    }
  }
  return undefined;
};

const selectBlueIndicatorColor = async (app: GtkApp): Promise<void> => {
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
  // The video fixture places the terminal's right-aligned indicators to the right.
  await dialog.moveTo(0, 0);
  const notebook = expectElementKind(
    await app.getById('settings_notebook'),
    'tabList'
  );
  for (let index = 0; index < (await notebook.getChildCount()); index += 1) {
    if ((await (await notebook.childAt(index))?.info())?.name === 'Terminal') {
      await notebook.selectChildAt(index);
      break;
    }
  }
  const scrollbar = expectElementKind(
    await app.getById('settings_terminal_page_scrollbar'),
    'scrollbar'
  );
  await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
  await expectElementKind(
    await app.getById('settings_terminal_indicator_color_button'),
    'button'
  ).click();
  const chooser = await waitForResult(async () => {
    for (let index = 0; index < (await app.getWindowCount()); index += 1) {
      const window = await app.windowAt(index);
      if (window !== undefined && (await window.info()).name === 'Pick a Color')
        return window;
    }
    throw new Error('Color chooser has not opened');
  });
  await expectElementKind(
    await findNamedChild(chooser, 'radio', 'Blue'),
    'radio'
  ).click();
  await expectElementKind(
    await findNamedChild(chooser, 'button', 'Select'),
    'button'
  ).click();
};

type VideoIndicatorState = 'red-on' | 'red-off' | 'blue-on' | 'blue-off';

const expectVideoPixel = (
  pixel: readonly number[],
  state: VideoIndicatorState,
  litReference: readonly number[],
  darkReference: readonly number[]
): void => {
  const channel = state.startsWith('red') ? 0 : 2;
  const other = channel === 0 ? 2 : 0;
  expect(pixel[channel] - pixel[other], state).toBeGreaterThan(30);
  expect(pixel[channel] - pixel[1], state).toBeGreaterThan(20);
  // GTK fades the parent while a modal picker is open. Compare lamps within
  // the same frame so that a faded dark lamp cannot be mistaken for a lit one.
  const contrast = litReference[channel] - darkReference[channel];
  expect(contrast, state).toBeGreaterThan(20);
  const level = (pixel[channel] - darkReference[channel]) / contrast;
  if (state.endsWith('-on')) expect(level, state).toBeGreaterThan(0.75);
  else expect(level, state).toBeLessThan(0.25);
};

describe.concurrent('terminal indicator color', () => {
  for (const [name, color, channel, rgb] of [
    ['red', '#FF0000', 0, [255, 0, 0]],
    ['blue', '#0000FF', 2, [0, 0, 255]],
    ['black', '#000000', 0, [0, 0, 0]],
    ['white', '#FFFFFF', 0, [255, 255, 255]],
  ] as const) {
    it(`uses ${name} for every serial indicator and distinguishes lit from dark`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const configPath = join(directory, 'indicators.ini');
        await writeFile(
          configPath,
          [
            '[general]',
            'type=serial',
            '[serial]',
            'device=/dev/null',
            '[terminal]',
            `indicator_color=${color}`,
            '',
          ].join('\n'),
          'utf8'
        );
        await runGtkTest(
          context,
          ['--test-fixture', '-c', configPath],
          async (app, evidence) => {
            const brightness = new Map<string, number>();
            for (const id of serialActivityIndicatorIds) {
              const widget = await app.getById(`${id}_indicator_image`);
              const capture = await waitForResult(
                async () => {
                  const image = await widget.capture();
                  expect(image.clipped).toBe(false);
                  const pixel = capturePixel(image, 0.5, 0.5);
                  if (name === 'red' || name === 'blue') {
                    for (const other of [0, 1, 2]) {
                      if (other !== channel)
                        expect(pixel[channel] - pixel[other]).toBeGreaterThan(
                          30
                        );
                    }
                  } else {
                    expect(
                      Math.max(...pixel) - Math.min(...pixel)
                    ).toBeLessThan(12);
                  }
                  brightness.set(id, pixel[channel]);
                  await expectActivityIndicatorImageState(
                    image,
                    id === 'conn' ? 'on' : 'off',
                    rgb
                  );
                  return image;
                },
                {
                  message: `${id} should use the configured ${name} color`,
                  timeoutMs: 5_000,
                }
              );
              await evidence.captureEvidence(
                `${id}-${name}`,
                async () => capture
              );
            }
            for (const id of serialActivityIndicatorIds.filter(
              (id) => id !== 'conn'
            )) {
              expect(
                brightness.get('conn')! - brightness.get(id)!
              ).toBeGreaterThan(name === 'black' ? 10 : 40);
            }
            const window = expectElementKind(await app.windowAt(0), 'window');
            await evidence.captureEvidence(`all-indicators-${name}`, async () =>
              window.capture()
            );
          }
        );
      });
    });
  }

  it('saves the chosen color, reloads it after restarting, and applies the original green without changing lamp state', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'saved-color.ini');
      await writeFile(
        configPath,
        '[terminal]\nauto_close=false\nindicator_color=#FF0000\n',
        'utf8'
      );
      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await waitForActivityIndicatorImageState(
            app,
            'conn',
            'on',
            5_000,
            [255, 0, 0]
          );
          await selectBlueIndicatorColor(app);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(await readFile(configPath, 'utf8')).toContain(
              'indicator_color=#3584E4\n'
            );
          });
        }
      );
      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app, evidence) => {
          for (const id of ['conn', 'log', 'sd', 'rd'] as const) {
            await waitForActivityIndicatorImageState(
              app,
              id,
              id === 'conn' ? 'on' : 'off',
              5_000,
              [53, 132, 228]
            );
          }
          const saved = await readFile(configPath, 'utf8');
          await expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          ).click();
          await expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          ).click();
          const notebook = expectElementKind(
            await app.getById('settings_notebook'),
            'tabList'
          );
          for (
            let index = 0;
            index < (await notebook.getChildCount());
            index += 1
          ) {
            if (
              (await (await notebook.childAt(index))?.info())?.name ===
              'Terminal'
            ) {
              await notebook.selectChildAt(index);
              break;
            }
          }
          const scrollbar = expectElementKind(
            await app.getById('settings_terminal_page_scrollbar'),
            'scrollbar'
          );
          await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
          await expectElementKind(
            await app.getById('settings_terminal_indicator_color_mode_combo'),
            'comboBox'
          ).selectChildAt(1);
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          for (const id of ['conn', 'log', 'sd', 'rd']) {
            const capture = await waitForResult(async () => {
              const image = await (
                await app.getById(`${id}_indicator_image`)
              ).capture();
              await expectActivityIndicatorImageState(
                image,
                id === 'conn' ? 'on' : 'off'
              );
              return image;
            });
            await evidence.captureEvidence(
              `${id}-restored-default`,
              async () => capture
            );
          }
          expect(await readFile(configPath, 'utf8')).toBe(saved);
        }
      );
    });
  });

  for (const latched of [false, true]) {
    it(`preserves ${latched ? 'latched activity without new traffic' : 'ongoing blink transitions'} when changing color in a recorded session`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const sockets: Socket[] = [];
        let received = '';
        let traffic: ReturnType<typeof setInterval> | undefined;
        const server = createServer((socket) => {
          sockets.push(socket);
          socket.on('data', (bytes) => {
            received += bytes.toString('utf8');
          });
        });
        try {
          await new Promise<void>((resolve, reject) => {
            server.once('error', reject);
            server.listen(0, '127.0.0.1', resolve);
          });
          const address = server.address();
          if (address === null || typeof address === 'string')
            throw new Error('No TCP endpoint');
          const configPath = join(directory, 'blink.ini');
          const config = [
            '[general]',
            'type=telnet',
            '[telnet]',
            'address=127.0.0.1',
            `port=${address.port}`,
            '[terminal]',
            'auto_close=false',
            'indicator_color=#FF0000',
            '[macro.reply]',
            'regex=^PING$',
            'send=ACK\\n',
            '',
          ].join('\n');
          await writeFile(configPath, config, 'utf8');
          await runGtkTest(
            context,
            [
              '-c',
              configPath,
              ...(latched ? ['--test-latch-activity-indicators'] : []),
            ],
            async (app, evidence) => {
              await waitForResult(async () => {
                expect(sockets.length).toBe(1);
              });
              const screen = PNG.sync.read((await app.capture()).image);
              const mainWindow = expectElementKind(
                await app.getById('main_window'),
                'window'
              );
              const mainBounds = await mainWindow.bounds();
              await mainWindow.moveTo(screen.width - mainBounds.width, 0);
              const environment = await app.environment();
              const bounds: GtkCaptureBounds[] = [];
              // CONN remains lit and logging remains disabled throughout.
              for (const id of ['sd', 'rd', 'conn', 'log']) {
                const captured = await (
                  await app.getById(`${id}_indicator_image`)
                ).capture();
                expect(
                  captured.bounds.x + captured.bounds.width
                ).toBeLessThanOrEqual(screen.width);
                bounds.push(captured.bounds);
              }
              const videoPath = join(evidence.directory, 'indicator-color.mkv');
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
                stderr += bytes.toString('utf8');
              });
              recorder.stdout.on('data', (bytes: Buffer) => {
                pending = Buffer.concat([pending, bytes]);
                while (pending.length >= frameSize) {
                  frame = Buffer.from(pending.subarray(0, frameSize));
                  pending = pending.subarray(frameSize);
                  frameNumber += 1;
                }
              });
              const states: {
                readonly frame: number;
                readonly state: VideoIndicatorState;
              }[] = [];
              const expectNextFrame = async (
                state: VideoIndicatorState
              ): Promise<void> => {
                const previous = frameNumber;
                const matched = await waitForResult(
                  async () => {
                    if (recordingError !== undefined) throw recordingError;
                    expect(recorder.exitCode, stderr).toBeNull();
                    expect(frameNumber).toBeGreaterThan(previous);
                    const pixels = bounds.map((area) => {
                      const offset =
                        ((area.y + 9) * screen.width + area.x + 9) * 4;
                      return [
                        frame[offset + 2],
                        frame[offset + 1],
                        frame[offset],
                      ];
                    });
                    for (const pixel of pixels.slice(0, 2)) {
                      expectVideoPixel(pixel, state, pixels[2], pixels[3]);
                    }
                    return frameNumber;
                  },
                  {
                    message: `video should show both SD and RD ${state}`,
                    timeoutMs: 10_000,
                    intervalMs: 10,
                  }
                );
                states.push({ frame: matched, state });
              };
              try {
                await waitForResult(async () => {
                  expect(frameNumber).toBeGreaterThan(0);
                });
                sockets[0].write('PING\r\n');
                if (!latched)
                  traffic = setInterval(() => {
                    sockets[0].write('PING\r\n');
                  }, 40);
                await waitForResult(async () => {
                  expect(received).toContain('ACK');
                });
                await expectNextFrame('red-on');
                if (!latched) await expectNextFrame('red-off');
                await selectBlueIndicatorColor(app);
                const dialogBounds = await expectElementKind(
                  await app.getById('settings_dialog'),
                  'window'
                ).bounds();
                for (const area of bounds) {
                  expect(area.x).toBeGreaterThanOrEqual(
                    dialogBounds.x + dialogBounds.width
                  );
                }
                // Activity is still running (or latched) before Apply. Opening a
                // picker alone must not change the terminal's current color.
                await expectNextFrame('red-on');
                await expectElementKind(
                  await app.getById('settings_apply_button'),
                  'button'
                ).click();
                await expectNextFrame('blue-on');
                if (!latched) {
                  await expectNextFrame('blue-off');
                  await expectNextFrame('blue-on');
                  clearInterval(traffic);
                  traffic = undefined;
                  await expectNextFrame('blue-off');
                }
                expect(await readFile(configPath, 'utf8')).toBe(config);
              } finally {
                clearInterval(traffic);
                traffic = undefined;
                recorder.stdin.write('q\n');
                const code = await finished;
                await evidence.log('indicator video states', {
                  states,
                  code,
                  stderr,
                });
                expect(code, stderr).toBe(0);
              }
              // Verify states from the saved lossless video as well as the live
              // stream; timestamp passthrough keeps their frame ordinals equal.
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
                join(evidence.directory, 'indicator-frame-%d.png'),
              ]);
              for (let index = 0; index < states.length; index += 1) {
                const saved = PNG.sync.read(
                  await readFile(
                    join(evidence.directory, `indicator-frame-${index + 1}.png`)
                  )
                );
                const pixels = bounds.map((area) => {
                  const offset = ((area.y + 9) * saved.width + area.x + 9) * 4;
                  return Array.from(saved.data.subarray(offset, offset + 3));
                });
                for (const pixel of pixels.slice(0, 2)) {
                  expectVideoPixel(
                    pixel,
                    states[index].state,
                    pixels[2],
                    pixels[3]
                  );
                }
                if (index > 0)
                  expect(states[index].frame).toBeGreaterThan(
                    states[index - 1].frame
                  );
              }
            }
          );
        } finally {
          clearInterval(traffic);
          for (const socket of sockets) socket.destroy();
          await new Promise<void>((resolve, reject) => {
            server.close((error) => {
              if (error === undefined) resolve();
              else reject(error);
            });
          });
        }
      });
    });
  }
});
