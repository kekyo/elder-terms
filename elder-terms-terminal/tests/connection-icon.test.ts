// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { chmod, open, readFile, writeFile } from 'node:fs/promises';
import { constants } from 'node:fs';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import type { GtkApp, GtkWidgetElement } from 'gestament';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { capturePixel, expectElementKind } from './test-helpers';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';

const clickWidget = async (
  app: GtkApp,
  widget: GtkWidgetElement
): Promise<void> => {
  const { bounds } = await widget.capture();
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + bounds.width / 2),
    Math.trunc(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
};

const openSettings = async (app: GtkApp): Promise<void> => {
  await clickWidget(app, await app.getById('application_menu_button'));
  await expectElementKind(
    await app.getById('settings_menu_item'),
    'menuItem'
  ).click();
  await app.getById('settings_dialog');
};

const nativeCenterPixel = async (
  app: GtkApp,
  windowId: string
): Promise<number> => {
  const { stdout } = await promisify(execFile)(
    'xprop',
    ['-id', windowId, '-len', '4096', '32c', '_NET_WM_ICON'],
    { env: { ...process.env, ...(await app.environment()) } }
  );
  const pixels = stdout.split('=')[1]!.split(',').map(Number);
  expect(pixels[0]).toBe(16);
  expect(pixels[1]).toBe(16);
  return pixels[2 + 8 * 16 + 8]!;
};

it('keeps receiving local output while choosing icons and accepts input afterward', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const fifo = join(directory, 'output.fifo');
    const shell = join(directory, 'shell.sh');
    const received = join(directory, 'input.txt');
    const config = join(directory, 'connection.ini');
    const log = join(directory, 'output.log');
    await promisify(execFile)('mkfifo', [fifo]);
    // Hold the FIFO open so each write is released by an explicit UI action.
    // The shell blocks on input rather than relying on timed output or sleeps.
    const output = await open(fifo, constants.O_RDWR);
    try {
      await writeFile(
        shell,
        `#!/bin/sh
IFS= read -r first < "$ELDER_TERMS_ICON_TEST_FIFO"
printf '%s\\n' "$first"
IFS= read -r second < "$ELDER_TERMS_ICON_TEST_FIFO"
printf '%s\\n' "$second"
IFS= read -r input
printf '%s' "$input" > "$ELDER_TERMS_ICON_TEST_INPUT"
exec /bin/sh
`
      );
      await chmod(shell, 0o755);
      await writeFile(
        config,
        `[general]\nauto_close=false\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=output.log\nmode=cooked\n`
      );
      await runGtkTest(
        context,
        ['-c', config],
        async (app) => {
          await openSettings(app);
          await expectElementKind(
            await app.getById('settings_general_icon_choose'),
            'button'
          ).click();
          const popup = await app.getById('settings_general_icon_picker');
          const family = expectElementKind(
            await app.getById('settings_general_icon_family'),
            'comboBox'
          );
          for (const [index, marker] of [
            'NOTO_OUTPUT',
            'MATERIAL_OUTPUT',
          ].entries()) {
            await family.selectChildAt(index);
            await output.write(`${marker}\n`);
            await waitForResult(async () => {
              expect((await popup.info()).states).toContain('showing');
              expect(await readFile(log, 'utf8')).toContain(marker);
            });
          }
          await app.input.pressKey('Escape');
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await clickWidget(app, await app.getById('terminal_view'));
          await app.input.pressKey('x');
          await app.input.pressKey('Return');
          await waitForResult(async () => {
            expect(await readFile(received, 'utf8')).toBe('x');
          });
        },
        {
          env: {
            SHELL: shell,
            ELDER_TERMS_ICON_TEST_FIFO: fifo,
            ELDER_TERMS_ICON_TEST_INPUT: received,
          },
        }
      );
    } finally {
      await output.close();
    }
  });
});

it('shows a saved connection icon at the left of the terminal title bar', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const icon = join(directory, 'icon.png');
    const config = join(directory, 'connection.ini');
    const image = new PNG({ width: 2, height: 1 });
    image.data.set([255, 0, 0, 255, 255, 0, 0, 255]);
    await writeFile(icon, PNG.sync.write(image));
    await writeFile(config, `[general]\nicon=file:${icon}\n`);
    await runGtkTest(context, ['--test-fixture', '-c', config], async (app) => {
      const titleIcon = expectElementKind(
        await app.getById('header_connection_icon'),
        'image'
      );
      const window = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      const windowId = (await window.x11Info()).windowId;
      await waitForResult(async () => {
        const [red, green, blue] = capturePixel(
          await titleIcon.capture(),
          0.5,
          0.5
        );
        expect(red).toBeGreaterThan(220);
        expect(green).toBeLessThan(35);
        expect(blue).toBeLessThan(35);
        expect(await nativeCenterPixel(app, windowId)).toBe(0xffff0000);
      });
      await openSettings(app);
      await expectElementKind(
        await app.getById('settings_general_icon_reset'),
        'button'
      ).click();
      await expectElementKind(
        await app.getById('settings_cancel_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await app.findById('settings_dialog')).toBeUndefined();
        const [red, green, blue] = capturePixel(
          await titleIcon.capture(),
          0.5,
          0.5
        );
        expect(red).toBeGreaterThan(220);
        expect(green).toBeLessThan(35);
        expect(blue).toBeLessThan(35);
        expect(await nativeCenterPixel(app, windowId)).toBe(0xffff0000);
      });
      await openSettings(app);
      await expectElementKind(
        await app.getById('settings_general_icon_reset'),
        'button'
      ).click();
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(
          (await (await app.getById('settings_dialog')).info()).states
        ).toContain('showing');
      });
      await expectElementKind(
        await app.getById('settings_cancel_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await app.findById('settings_dialog')).toBeUndefined();
        const [red, green, blue] = capturePixel(
          await titleIcon.capture(),
          0.5,
          0.5
        );
        expect(red < 220 || green > 35 || blue > 35).toBe(true);
        expect(await nativeCenterPixel(app, windowId)).not.toBe(0xffff0000);
      });
    });
  });
});

for (const preset of [
  {
    name: 'Noto Color Emoji',
    specification: 'noto:emoji_u1f44d',
    expected: ([red, green, blue]: ReturnType<typeof capturePixel>) => {
      expect(red).toBeGreaterThan(180);
      expect(green).toBeGreaterThan(100);
      expect(blue).toBeLessThan(100);
    },
    sample: [0.4, 0.5] as const,
    transparent: false,
  },
  {
    name: 'black Material Icons',
    specification: 'material:action/home',
    expected: ([red, green, blue]: ReturnType<typeof capturePixel>) => {
      expect(red).toBeLessThan(80);
      expect(green).toBeLessThan(80);
      expect(blue).toBeLessThan(80);
    },
    sample: [0.5, 0.5] as const,
    transparent: true,
  },
  {
    name: 'white Material Icons',
    specification: 'material:white:action/home',
    expected: ([red, green, blue]: ReturnType<typeof capturePixel>) => {
      expect(red).toBeGreaterThan(230);
      expect(green).toBeGreaterThan(230);
      expect(blue).toBeGreaterThan(230);
    },
    sample: [0.5, 0.5] as const,
    transparent: true,
  },
]) {
  it(`shows saved ${preset.name} artwork in the terminal title bar`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const config = join(directory, 'connection.ini');
      await writeFile(config, `[general]\nicon=${preset.specification}\n`);
      await runGtkTest(
        context,
        ['--test-fixture', '-c', config],
        async (app) => {
          const titleIcon = expectElementKind(
            await app.getById('header_connection_icon'),
            'image'
          );
          await waitForResult(async () => {
            const captured = await titleIcon.capture();
            preset.expected(
              capturePixel(captured, preset.sample[0], preset.sample[1])
            );
            if (preset.transparent) {
              const outside = capturePixel(captured, 0.01, 0.5);
              const padding = capturePixel(captured, 0.1, 0.5);
              for (let channel = 0; channel < 3; ++channel)
                expect(
                  Math.abs(padding[channel] - outside[channel])
                ).toBeLessThanOrEqual(2);
            }
          });
        }
      );
    });
  });
}
