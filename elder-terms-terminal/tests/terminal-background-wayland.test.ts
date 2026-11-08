// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, stat, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';

import {
  withTemporaryDirectory,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

for (const backend of ['cairo', 'opengl']) {
  for (const mode of ['transparent', 'blur']) {
    for (const cellMode of ['transparent', 'opaque']) {
      it(`composites the actual Wayland surface and reports its renderer (${backend}, ${mode}, ${cellMode})`, async () => {
        await withTemporaryDirectory(async (directory) => {
          const runtime = join(directory, 'runtime');
          await mkdir(runtime, { mode: 0o700 });
          const config = join(directory, 'terminal.ini');
          const westonConfig = join(directory, 'weston.ini');
          const peer = join(directory, 'peer.mjs');
          await writeFile(
            peer,
            "process.stdin.setRawMode(true); process.stdin.resume(); process.stdout.write('\\x1b[?25l\\x1b]11;#000000\\x07\\x1b[49m\\x1b[38;2;255;255;255m\\x1b[2J\\x1b[HWHITE\\x1b[12;1H\\x1b[48;2;0;0;0m\\x1b[J');"
          );
          await writeFile(
            config,
            `[terminal]\nbackground_mode=${mode}\nbackground_opacity=50\ncell_background_mode=${cellMode}\n[local]\ncommand_line=${process.execPath} ${peer}\n`
          );
          await writeFile(
            westonConfig,
            '[shell]\nbackground-image=\nbackground-color=0xffe03020\npanel-position=none\nlocking=false\nanimation=none\nclose-animation=none\n'
          );
          const launcher = createGtkAppLauncher({
            // Keep the automation driver on Xvfb and put only the child application
            // on the nested Wayland display; both captures and input remain isolated.
            appPath: '/usr/bin/env',
            args: [
              'GDK_BACKEND=wayland',
              'WAYLAND_DISPLAY=appearance-wayland',
              `XDG_RUNTIME_DIR=${runtime}`,
              fileURLToPath(
                new URL(
                  '../../.build/elder-terms-terminal/elder-terms-terminal',
                  import.meta.url
                )
              ),
              '-c',
              config,
            ],
            accessibilitySession: 'minimal',
            env: {
              XDG_CONFIG_HOME: join(directory, 'config'),
              XDG_DATA_HOME: join(directory, 'data'),
              LIBGL_ALWAYS_SOFTWARE: '1',
              GDK_GL: backend === 'cairo' ? 'disable' : undefined,
            },
          });
          const env = {
            ...(await launcher.environment()),
            XDG_RUNTIME_DIR: runtime,
          };
          const weston = spawn(
            'weston',
            [
              '--backend=x11-backend.so',
              '--width=1280',
              '--height=720',
              '--socket=appearance-wayland',
              '--idle-time=0',
              `--config=${westonConfig}`,
            ],
            { env, stdio: ['ignore', 'pipe', 'pipe'] }
          );
          const closed = once(weston, 'close');
          let compositorLog = '';
          weston.stderr.on('data', (bytes: Buffer) => {
            compositorLog += bytes.toString();
          });
          const evidence = fileURLToPath(
            new URL(
              `../../test-results/boxrig/background-wayland-${backend}-${mode}-${cellMode}/`,
              import.meta.url
            )
          );
          await mkdir(evidence, { recursive: true });
          try {
            await waitForResult(async () =>
              expect(
                (await stat(join(runtime, 'appearance-wayland'))).isSocket()
              ).toBe(true)
            );
            const app = await launcher.launch();
            // Capture the outer Xvfb screen: AT-SPI window coordinates are not global
            // coordinates on Wayland. The nested compositor supplies the real pixels.
            try {
              await waitForResult(async () => {
                const frame = PNG.sync.read((await app.capture()).image);
                let blended = 0;
                let opaque = 0;
                let desktop = 0;
                for (let i = 0; i < frame.data.length; i += 4) {
                  if (
                    frame.data[i] === 0 &&
                    frame.data[i + 1] === 0 &&
                    frame.data[i + 2] === 0
                  )
                    opaque++;
                  if (
                    Math.abs(frame.data[i] - 112) <= 2 &&
                    Math.abs(frame.data[i + 1] - 24) <= 2 &&
                    Math.abs(frame.data[i + 2] - 16) <= 2
                  )
                    blended++;
                  if (
                    frame.data[i] === 224 &&
                    frame.data[i + 1] === 48 &&
                    frame.data[i + 2] === 32
                  )
                    desktop++;
                }
                expect(blended).toBeGreaterThan(50000);
                if (cellMode === 'opaque')
                  expect(opaque).toBeGreaterThan(50000);
                else expect(opaque).toBeLessThan(50000);
                expect(desktop).toBeGreaterThan(50000);
              });
            } finally {
              await writeFile(
                join(evidence, 'last-frame.png'),
                (await app.capture()).image
              );
              await writeFile(
                join(evidence, 'app.stderr.log'),
                (await app.output()).stderr
              );
            }
            await writeFile(
              join(evidence, 'transparent.png'),
              (await app.capture()).image
            );
            await expectElementKind(
              await app.getById('application_menu_button'),
              'toggleButton'
            ).click();
            await expectElementKind(
              await app.getById('settings_menu_item'),
              'menuItem'
            ).click();
            await waitForResult(async () =>
              expect(
                await expectElementKind(
                  await app.getById('settings_terminal_background_status'),
                  'label'
                ).text()
              ).toBe(
                mode === 'blur'
                  ? 'Transparent background: frosted glass is unavailable'
                  : 'Transparent background'
              )
            );
            const renderer = await expectElementKind(
              await app.getById('settings_runtime_status'),
              'label'
            ).text();
            expect(renderer).toContain('Wayland');
            expect(renderer).toContain(
              backend === 'cairo' ? 'Cairo' : 'OpenGL'
            );
            await writeFile(join(evidence, 'renderer.txt'), renderer + '\n');
            await expectElementKind(
              await app.getById('settings_cancel_button'),
              'button'
            ).click();
            await writeFile(
              join(evidence, 'app.stderr.log'),
              (await app.output()).stderr
            );
          } finally {
            await launcher.release();
            weston.kill('SIGTERM');
            await closed;
            await writeFile(join(evidence, 'weston.log'), compositorLog);
          }
        });
      });
    }
  }
}
