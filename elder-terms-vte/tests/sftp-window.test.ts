import { createRequire } from 'node:module';
import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  utimes,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkCapture,
  type GtkTableElement,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import type { PNG as PngImage } from 'pngjs';
import { describe, expect, it, type TestContext } from 'vitest';
import {
  createTestEvidence,
  expectCaptureToMatchFixture,
  expectElementKind,
  type TestEvidence,
} from './test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const sftpAppPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/elder-terms-sftp', import.meta.url)
);
const sftpConnectionColorsFixturePath = fileURLToPath(
  new URL('./fixtures/sftp-connection-colors.png', import.meta.url)
);
const sftpConnectionColorsTransferOverlayFixturePath = fileURLToPath(
  new URL(
    './fixtures/sftp-connection-colors-transfer-overlay.png',
    import.meta.url
  )
);
const stableLocalModificationTime = new Date('2020-01-02T03:04:00Z');

interface SftpFixture {
  readonly app: GtkApp;
  readonly evidence: TestEvidence;
  readonly localDirectory: string;
}

const runSftpFixture = async (
  context: TestContext,
  pauseTransfer: boolean,
  generalSettings: readonly string[],
  body: (fixture: SftpFixture) => Promise<void>
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-sftp-'));
  try {
    const configHome = join(directory, 'xdg-config');
    const globalConfigDirectory = join(configHome, 'elder-terms');
    const globalConfigPath = join(globalConfigDirectory, 'global.ini');
    const localDirectory = join(directory, 'local');
    const configPath = join(directory, 'sftp.ini');
    await mkdir(join(localDirectory, 'documents'), { recursive: true });
    await mkdir(globalConfigDirectory, { recursive: true });
    await writeFile(join(localDirectory, 'hello.txt'), 'hello from local\n');
    await utimes(
      join(localDirectory, 'documents'),
      stableLocalModificationTime,
      stableLocalModificationTime
    );
    await utimes(
      join(localDirectory, 'hello.txt'),
      stableLocalModificationTime,
      stableLocalModificationTime
    );
    await writeFile(
      globalConfigPath,
      [
        '[sftp]',
        `local_directory=${localDirectory}`,
        'remote_directory=/remote',
        '',
      ].join('\n')
    );
    await writeFile(
      configPath,
      [
        '[general]',
        'name=Fixture files',
        'type=sftp',
        ...generalSettings,
        '',
        '[ssh]',
        'address=fixture.example',
        '',
      ].join('\n')
    );

    const evidence = createTestEvidence(context);
    try {
      const launcher = createGtkAppLauncher({
        appPath: sftpAppPath,
        env: {
          XDG_CONFIG_HOME: configHome,
        },
        onSystemOutput: evidence.recordSystemOutputEvent,
        xvfbTrayHost: true,
      });
      const args = ['--test-fixture', '-c', configPath];
      if (pauseTransfer) {
        args.unshift('--test-sftp-pause-transfer');
      }
      const apps: GtkApp[] = [];
      try {
        const app = await launcher.launch(args, {
          onOutput: evidence.recordAppOutputEvent,
        });
        apps.push(app);
        await body({ app, evidence, localDirectory });
      } finally {
        try {
          await evidence.flushOutputs(apps, launcher);
        } finally {
          await launcher.release();
        }
      }
    } finally {
      await evidence.release();
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
};

const expectTable = (element: GtkWidgetElement): GtkTableElement => {
  expect(element.kind).toBe('table');
  return element as GtkTableElement;
};

const findRow = async (table: GtkTableElement, name: string): Promise<number> =>
  waitForResult(async () => {
    const rows = await table.getRowCount();
    for (let row = 0; row < rows; row += 1) {
      const cell = await table.cellAt(row, 0);
      if ((await cell?.info())?.name === name) {
        return row;
      }
    }
    throw new Error(`SFTP row was not found: ${name}`);
  });

const openContextMenu = async (
  app: GtkApp,
  table: GtkTableElement,
  row: number
): Promise<void> => {
  await table.selectRow(row);
  const cell = await table.cellAt(row, 0);
  const bounds = (await cell?.capture())?.bounds;
  if (bounds === undefined) {
    throw new Error(`SFTP row ${row} did not expose bounds`);
  }
  await app.input.moveMouseTo(
    Math.round(bounds.x + bounds.width / 2),
    Math.round(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('right', true);
  await app.input.setMouseButton('right', false);
};

const expectShowing = async (element: GtkWidgetElement): Promise<void> => {
  await waitForResult(async () => {
    expect((await element.info()).states).toContain('showing');
  });
};

const expectHidden = async (element: GtkWidgetElement): Promise<void> => {
  await waitForResult(async () => {
    expect((await element.info()).states).not.toContain('showing');
  });
};

type RgbPixel = readonly [red: number, green: number, blue: number];

const capturePixel = (
  capture: GtkCapture,
  horizontalRatio: number,
  verticalRatio: number
): RgbPixel => {
  const png = PNG.sync.read(capture.image) as PngImage;
  const x = Math.min(
    png.width - 1,
    Math.max(0, Math.trunc(png.width * horizontalRatio))
  );
  const y = Math.min(
    png.height - 1,
    Math.max(0, Math.trunc(png.height * verticalRatio))
  );
  const offset = (y * png.width + x) * 4;
  return [
    png.data[offset] ?? 0,
    png.data[offset + 1] ?? 0,
    png.data[offset + 2] ?? 0,
  ];
};

const captureBorderSamples = (capture: GtkCapture): readonly RgbPixel[] => {
  const png = PNG.sync.read(capture.image) as PngImage;
  const readPixel = (x: number, y: number): RgbPixel => {
    const offset = (y * png.width + x) * 4;
    return [
      png.data[offset] ?? 0,
      png.data[offset + 1] ?? 0,
      png.data[offset + 2] ?? 0,
    ];
  };
  const centerX = Math.trunc(png.width / 2);
  const centerY = Math.trunc(png.height / 2);
  return [
    readPixel(centerX, 0),
    readPixel(centerX, 1),
    readPixel(centerX, png.height - 2),
    readPixel(centerX, png.height - 1),
    readPixel(0, centerY),
    readPixel(1, centerY),
    readPixel(png.width - 2, centerY),
    readPixel(png.width - 1, centerY),
  ];
};

describe('SFTP window', () => {
  it('applies configured exterior and browser RGB backgrounds', async (context) => {
    await runSftpFixture(
      context,
      false,
      ['exterior_background=#7A2468', 'background=#183C58'],
      async ({ app, evidence }) => {
        const componentBackground = [0x1f, 0x4d, 0x71] as const;
        const localTree = expectTable(await app.getById('sftp_local_tree'));
        const localPath = expectElementKind(
          await app.getById('sftp_local_path_entry'),
          'entry'
        );
        const remotePath = expectElementKind(
          await app.getById('sftp_remote_path_entry'),
          'entry'
        );
        await waitForResult(async () => {
          expect(await findRow(localTree, 'hello.txt')).toBeGreaterThanOrEqual(
            0
          );
        });
        await localPath.setText('/fixture/local');

        const header = await app.getById('sftp_header_bar');
        const status = await app.getById('sftp_status_bar');
        expect(capturePixel(await header.capture(), 0.08, 0.5)).toEqual([
          0x7a, 0x24, 0x68,
        ]);
        const statusCapture = await status.capture();
        expect(capturePixel(statusCapture, 0.8, 0.5)).toEqual([
          0x7a, 0x24, 0x68,
        ]);
        expect(capturePixel(statusCapture, 0.5, 0.05)).toEqual([
          0x7a, 0x24, 0x68,
        ]);
        expect(capturePixel(await localTree.capture(), 0.5, 0.8)).toEqual([
          0x18, 0x3c, 0x58,
        ]);
        expect(capturePixel(await localPath.capture(), 0.7, 0.5)).toEqual(
          componentBackground
        );
        expect(
          capturePixel(
            await (await app.getById('sftp_local_refresh_button')).capture(),
            0.15,
            0.5
          )
        ).toEqual(componentBackground);

        const treeCapture = await localTree.capture();
        await app.input.moveMouseTo(
          Math.trunc(treeCapture.bounds.x + treeCapture.bounds.width / 2),
          Math.trunc(treeCapture.bounds.y + treeCapture.bounds.height * 0.8)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await waitForResult(async () => {
          expect((await localTree.info()).states).toContain('focused');
        });
        await app.input.moveMouseTo(
          Math.trunc(statusCapture.bounds.x + statusCapture.bounds.width * 0.8),
          Math.trunc(statusCapture.bounds.y + statusCapture.bounds.height / 2)
        );
        await waitForResult(async () => {
          const [localPathCapture, remotePathCapture] = await Promise.all([
            localPath.capture(),
            remotePath.capture(),
          ]);
          expect(captureBorderSamples(remotePathCapture)).toEqual(
            captureBorderSamples(localPathCapture)
          );
        });

        const window = expectElementKind(
          await app.getById('sftp_window'),
          'window'
        );
        const capture = await evidence.captureEvidence(
          'sftp-connection-colors',
          async () => window.capture()
        );
        expect(capturePixel(capture, 0.5, 0.952)).toEqual([0x7a, 0x24, 0x68]);
        await expectCaptureToMatchFixture(
          capture,
          'sftp-connection-colors',
          sftpConnectionColorsFixturePath,
          evidence
        );
      }
    );
  });

  it('inherits global directories and navigates independent local and remote trees', async (context) => {
    await runSftpFixture(
      context,
      false,
      [],
      async ({ app, evidence, localDirectory }) => {
        expect(await app.getWindowCount()).toBe(1);
        const window = expectElementKind(
          await app.getById('sftp_window'),
          'window'
        );
        const localTree = expectTable(await app.getById('sftp_local_tree'));
        const remoteTree = expectTable(await app.getById('sftp_remote_tree'));
        const localPath = expectElementKind(
          await app.getById('sftp_local_path_entry'),
          'entry'
        );
        const remotePath = expectElementKind(
          await app.getById('sftp_remote_path_entry'),
          'entry'
        );

        await waitForResult(async () => {
          expect(await localPath.text()).toBe(localDirectory);
          expect(await remotePath.text()).toBe('/remote');
        });
        expect(await findRow(localTree, 'documents')).toBeGreaterThanOrEqual(0);
        expect(await findRow(localTree, 'hello.txt')).toBeGreaterThanOrEqual(0);
        expect(await findRow(remoteTree, 'archive')).toBeGreaterThanOrEqual(0);
        expect(await findRow(remoteTree, 'readme.txt')).toBeGreaterThanOrEqual(
          0
        );

        await remotePath.setText('/remote/archive');
        await app.input.pressKey('Return');
        await waitForResult(async () => {
          expect(await remotePath.text()).toBe('/remote/archive');
          expect(await findRow(remoteTree, 'old.log')).toBeGreaterThanOrEqual(
            0
          );
        });
        await expectElementKind(
          await app.getById('sftp_remote_up_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await remotePath.text()).toBe('/remote');
        });

        const capture = await evidence.captureEvidence(
          'sftp-dual-pane',
          async () => window.capture()
        );
        expect(capture.bounds.width).toBeGreaterThan(capture.bounds.height);
      }
    );
  });

  it('sends and receives selected items from the pane context menus', async (context) => {
    await runSftpFixture(
      context,
      false,
      [],
      async ({ app, localDirectory }) => {
        const localTree = expectTable(await app.getById('sftp_local_tree'));
        const remoteTree = expectTable(await app.getById('sftp_remote_tree'));
        await openContextMenu(
          app,
          localTree,
          await findRow(localTree, 'hello.txt')
        );
        const send = expectElementKind(
          await app.getById('sftp_send_item'),
          'menuItem'
        );
        await expectShowing(send);
        await send.click();
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('sftp_status_label'),
              'label'
            ).text()
          ).toBe('Sent 1 item');
          expect(await findRow(remoteTree, 'hello.txt')).toBeGreaterThanOrEqual(
            0
          );
        });

        await openContextMenu(
          app,
          remoteTree,
          await findRow(remoteTree, 'readme.txt')
        );
        const receive = expectElementKind(
          await app.getById('sftp_receive_item'),
          'menuItem'
        );
        await expectShowing(receive);
        await receive.click();
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('sftp_status_label'),
              'label'
            ).text()
          ).toBe('Received 1 item');
          expect(
            await readFile(join(localDirectory, 'readme.txt'), 'utf8')
          ).toBe('hello from remote\n');
        });
      }
    );
  });

  it('dims both panes during transfer and cancels from the progress overlay', async (context) => {
    await runSftpFixture(
      context,
      true,
      ['background=#183C58'],
      async ({ app, evidence }) => {
        const background = [0x18, 0x3c, 0x58] as const;
        const componentBackground = [0x1f, 0x4d, 0x71] as const;
        const localTree = expectTable(await app.getById('sftp_local_tree'));
        await openContextMenu(
          app,
          localTree,
          await findRow(localTree, 'hello.txt')
        );
        const sendItem = expectElementKind(
          await app.getById('sftp_send_item'),
          'menuItem'
        );
        expect(capturePixel(await sendItem.capture(), 0.8, 0.5)).toEqual(
          componentBackground
        );
        await sendItem.click();

        const overlay = await app.getById('sftp_transfer_overlay');
        const dim = await app.getById('sftp_dim_overlay');
        await expectShowing(overlay);
        await expectShowing(dim);
        for (const [widgetId, horizontalRatio] of [
          ['sftp_transfer_overlay', 0.05],
        ] as const) {
          expect(
            capturePixel(
              await (await app.getById(widgetId)).capture(),
              horizontalRatio,
              0.5
            )
          ).toEqual(background);
        }
        for (const [widgetId, horizontalRatio] of [
          ['sftp_transfer_progress', 0.05],
          ['sftp_transfer_cancel_button', 0.15],
        ] as const) {
          expect(
            capturePixel(
              await (await app.getById(widgetId)).capture(),
              horizontalRatio,
              0.5
            )
          ).toEqual(componentBackground);
        }
        const overlayCapture = await evidence.captureEvidence(
          'sftp-connection-colors-transfer-overlay',
          async () => overlay.capture()
        );
        await expectCaptureToMatchFixture(
          overlayCapture,
          'sftp-connection-colors-transfer-overlay',
          sftpConnectionColorsTransferOverlayFixturePath,
          evidence,
          {
            maxDiffPixels: 800,
            threshold: 0.01,
          }
        );
        expect((await localTree.info()).states).not.toContain('sensitive');
        await expectElementKind(
          await app.getById('sftp_transfer_cancel_button'),
          'button'
        ).click();

        await expectHidden(overlay);
        await expectHidden(dim);
        await waitForResult(async () => {
          expect((await localTree.info()).states).toContain('sensitive');
          expect(
            await expectElementKind(
              await app.getById('sftp_status_label'),
              'label'
            ).text()
          ).toBe('Transfer cancelled');
        });
      }
    );
  });
});
