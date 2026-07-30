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
  gtkCss: string | undefined,
  body: (fixture: SftpFixture) => Promise<void>
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-sftp-'));
  try {
    const configHome = join(directory, 'xdg-config');
    const globalConfigDirectory = join(configHome, 'elder-terms');
    const globalConfigPath = join(globalConfigDirectory, 'global.ini');
    const gtkConfigDirectory = join(configHome, 'gtk-3.0');
    const localDirectory = join(directory, 'local');
    const configPath = join(directory, 'sftp.ini');
    await mkdir(join(localDirectory, 'documents'), { recursive: true });
    await mkdir(globalConfigDirectory, { recursive: true });
    if (gtkCss !== undefined) {
      await mkdir(gtkConfigDirectory, { recursive: true });
      await writeFile(join(gtkConfigDirectory, 'gtk.css'), gtkCss);
    }
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

const clickTreeExpander = async (
  app: GtkApp,
  table: GtkTableElement,
  row: number
): Promise<void> => {
  const tableCapture = await table.capture();
  const cell = await table.cellAt(row, 0);
  const cellCapture = await cell?.capture();
  if (cellCapture === undefined) {
    throw new Error(`SFTP row ${row} did not expose bounds`);
  }
  // GtkTreeView exposes the row through AT-SPI, but not its expander.
  await app.input.moveMouseTo(
    tableCapture.bounds.x + 10,
    Math.round(cellCapture.bounds.y + cellCapture.bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
};

const captureRowBounds = async (
  table: GtkTableElement,
  row: number
): Promise<GtkCapture['bounds']> => {
  const cell = await table.cellAt(row, 0);
  const capture = await cell?.capture();
  if (capture === undefined) {
    throw new Error(`SFTP row ${row} did not expose bounds`);
  }
  return capture.bounds;
};

interface BrightInkVerticalMargins {
  readonly top: number;
  readonly bottom: number;
}

const brightInkVerticalMargins = (
  capture: GtkCapture,
  horizontalStartRatio: number,
  horizontalEndRatio: number,
  minimumChannel: number
): BrightInkVerticalMargins => {
  const png = PNG.sync.read(capture.image) as PngImage;
  const firstX = Math.max(0, Math.trunc(png.width * horizontalStartRatio));
  const lastX = Math.min(png.width, Math.ceil(png.width * horizontalEndRatio));
  let firstInkRow = png.height;
  let lastInkRow = -1;
  for (let y = 0; y < png.height; y += 1) {
    for (let x = firstX; x < lastX; x += 1) {
      const offset = (y * png.width + x) * 4;
      if (
        (png.data[offset] ?? 0) >= minimumChannel &&
        (png.data[offset + 1] ?? 0) >= minimumChannel &&
        (png.data[offset + 2] ?? 0) >= minimumChannel &&
        (png.data[offset + 3] ?? 0) >= 224
      ) {
        firstInkRow = Math.min(firstInkRow, y);
        lastInkRow = Math.max(lastInkRow, y);
      }
    }
  }
  if (lastInkRow < 0) {
    throw new Error('SFTP tree did not contain bright text pixels');
  }
  return {
    top: firstInkRow,
    bottom: png.height - lastInkRow - 1,
  };
};

const horizontalGap = (
  left: GtkCapture['bounds'],
  right: GtkCapture['bounds']
): number => right.x - (left.x + left.width);

const verticalGap = (
  top: GtkCapture['bounds'],
  bottom: GtkCapture['bounds']
): number => bottom.y - (top.y + top.height);

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
      undefined,
      async ({ app, evidence }) => {
        const exteriorComponentBackground = [0x85, 0x27, 0x71] as const;
        const componentBackground = [0x1b, 0x45, 0x65] as const;
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
        expect(capturePixel(await header.capture(), 0.9, 0.5)).toEqual(
          exteriorComponentBackground
        );
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
        await window.moveTo(16, 16);
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
      undefined,
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

  it('pads content groups and controls outside the tree lists', async (context) => {
    await runSftpFixture(context, false, [], undefined, async ({ app }) => {
      const localTree = expectTable(await app.getById('sftp_local_tree'));
      await findRow(localTree, 'documents');
      const [
        paned,
        localGroup,
        remoteGroup,
        localPath,
        localUp,
        localRefresh,
        tree,
        status,
        statusLabel,
      ] = await Promise.all(
        [
          'sftp_root_paned',
          'sftp_local_group',
          'sftp_remote_group',
          'sftp_local_path_entry',
          'sftp_local_up_button',
          'sftp_local_refresh_button',
          'sftp_local_tree',
          'sftp_status_bar',
          'sftp_status_label',
        ].map(async (id) => (await app.getById(id)).capture())
      );

      expect(localGroup.bounds.x - paned.bounds.x).toBeGreaterThanOrEqual(12);
      expect(localGroup.bounds.y - paned.bounds.y).toBeGreaterThanOrEqual(12);
      expect(
        paned.bounds.x +
          paned.bounds.width -
          (remoteGroup.bounds.x + remoteGroup.bounds.width)
      ).toBeGreaterThanOrEqual(12);
      expect(
        paned.bounds.y +
          paned.bounds.height -
          (localGroup.bounds.y + localGroup.bounds.height)
      ).toBeGreaterThanOrEqual(12);
      expect(
        horizontalGap(localGroup.bounds, remoteGroup.bounds)
      ).toBeGreaterThanOrEqual(12);

      expect(localPath.bounds.x - localGroup.bounds.x).toBeGreaterThanOrEqual(
        12
      );
      expect(
        localGroup.bounds.x +
          localGroup.bounds.width -
          (localRefresh.bounds.x + localRefresh.bounds.width)
      ).toBeGreaterThanOrEqual(12);
      expect(
        horizontalGap(localPath.bounds, localUp.bounds)
      ).toBeGreaterThanOrEqual(8);
      expect(
        horizontalGap(localUp.bounds, localRefresh.bounds)
      ).toBeGreaterThanOrEqual(8);
      expect(verticalGap(localPath.bounds, tree.bounds)).toBeGreaterThanOrEqual(
        8
      );
      expect(
        localGroup.bounds.y +
          localGroup.bounds.height -
          (tree.bounds.y + tree.bounds.height)
      ).toBeGreaterThanOrEqual(12);

      expect(statusLabel.bounds.x - status.bounds.x).toBeGreaterThanOrEqual(12);
      expect(statusLabel.bounds.y - status.bounds.y).toBeGreaterThanOrEqual(8);
      expect(
        status.bounds.y +
          status.bounds.height -
          (statusLabel.bounds.y + statusLabel.bounds.height)
      ).toBeGreaterThanOrEqual(8);
    });
  });

  it('expands a populated directory on the first expander click', async (context) => {
    await runSftpFixture(
      context,
      false,
      [],
      undefined,
      async ({ app, localDirectory }) => {
        await mkdir(join(localDirectory, 'documents', 'nested'));
        await writeFile(
          join(localDirectory, 'documents', 'inside.txt'),
          'inside\n'
        );

        const localTree = expectTable(await app.getById('sftp_local_tree'));
        const documentsRow = await findRow(localTree, 'documents');
        const initialRowCount = await localTree.getRowCount();
        await clickTreeExpander(app, localTree, documentsRow);

        expect(await findRow(localTree, 'nested')).toBeGreaterThan(
          documentsRow
        );
        expect(await findRow(localTree, 'inside.txt')).toBeGreaterThan(
          documentsRow
        );
        expect(await localTree.getRowCount()).toBe(initialRowCount + 2);
        expect(
          (await (await localTree.cellAt(documentsRow, 0))?.info())?.states
        ).toContain('expanded');
      }
    );
  });

  it('keeps mixed tree row geometry stable while scrolling', async (context) => {
    await runSftpFixture(
      context,
      false,
      [],
      ['treeview.view {', '  -GtkTreeView-expander-size: 32;', '}', ''].join(
        '\n'
      ),
      async ({ app, localDirectory }) => {
        const localTree = expectTable(await app.getById('sftp_local_tree'));
        await findRow(localTree, 'documents');
        await Promise.all([
          ...Array.from({ length: 10 }, (_, index) =>
            mkdir(join(localDirectory, `dir-${String(index).padStart(3, '0')}`))
          ),
          ...Array.from({ length: 30 }, (_, index) =>
            writeFile(
              join(
                localDirectory,
                index === 10
                  ? 'file-010\nsecond-line'
                  : `file-${String(index).padStart(3, '0')}`
              ),
              'file\n'
            )
          ),
        ]);
        await expectElementKind(
          await app.getById('sftp_local_refresh_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await localTree.getRowCount()).toBe(42);
        });

        const firstDirectoryRow = await findRow(localTree, 'dir-000');
        const secondDirectoryRow = await findRow(localTree, 'dir-001');
        const firstDirectoryBounds = await captureRowBounds(
          localTree,
          firstDirectoryRow
        );
        const secondDirectoryBounds = await captureRowBounds(
          localTree,
          secondDirectoryRow
        );
        expect(firstDirectoryBounds.height).toBeGreaterThanOrEqual(30);
        const directoryPitch = secondDirectoryBounds.y - firstDirectoryBounds.y;

        const previousFileRow = await findRow(localTree, 'file-009');
        const multilineFileRow = previousFileRow + 1;
        const treeCapture = await localTree.capture();
        await app.input.moveMouseTo(
          Math.round(treeCapture.bounds.x + treeCapture.bounds.width / 2),
          Math.round(treeCapture.bounds.y + treeCapture.bounds.height / 2)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await waitForResult(async () => {
          expect((await localTree.info()).states).toContain('focused');
        });
        await app.input.scrollWheel(0, 8);

        const previousFileBounds = await captureRowBounds(
          localTree,
          previousFileRow
        );
        const multilineFileBounds = await captureRowBounds(
          localTree,
          multilineFileRow
        );
        expect(multilineFileBounds.height).toBe(firstDirectoryBounds.height);
        expect(multilineFileBounds.y - previousFileBounds.y).toBe(
          directoryPitch
        );

        await app.input.scrollWheel(0, -8);
        expect(
          (await captureRowBounds(localTree, firstDirectoryRow)).height
        ).toBe(firstDirectoryBounds.height);
      }
    );
  });

  it('centers text across fallback font runs', async (context) => {
    await runSftpFixture(
      context,
      false,
      ['background=#000000'],
      [
        'treeview.view {',
        '  background-color: rgb(0, 0, 0);',
        '  color: rgb(255, 255, 255);',
        '  font-family: "Ubuntu Sans";',
        '  font-size: 11pt;',
        '}',
        '',
      ].join('\n'),
      async ({ app, evidence, localDirectory }) => {
        const entries = [
          {
            id: 'latin',
            name: 'English',
          },
          {
            id: 'cyrillic',
            name: 'Кириллица',
          },
          {
            id: 'arabic',
            name: 'العربية',
          },
          {
            id: 'devanagari',
            name: 'हिन्दी',
          },
          {
            id: 'thai',
            name: 'ภาษาไทย',
          },
          {
            id: 'hebrew',
            name: 'עברית',
          },
          {
            id: 'hangul',
            name: '한국어',
          },
          {
            id: 'mixed-fallback',
            name: 'オリジナルサウンドトラック 英雄伝説Ⅳ「朱紅い雫」',
          },
        ] as const;
        await Promise.all(
          entries.map(async ({ name }) => {
            await writeFile(join(localDirectory, name), 'fixture\n');
          })
        );

        const localTree = expectTable(await app.getById('sftp_local_tree'));
        await expectElementKind(
          await app.getById('sftp_local_refresh_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await localTree.getRowCount()).toBe(entries.length + 2);
        });

        for (const { id, name } of entries) {
          const row = await findRow(localTree, name);
          const cell = await localTree.cellAt(row, 0);
          const capture = await cell?.capture();
          if (capture === undefined) {
            throw new Error(`${id} SFTP row did not expose bounds`);
          }
          await evidence.captureEvidence(
            `sftp-fallback-${id}`,
            async () => capture
          );
          const margins = brightInkVerticalMargins(capture, 0, 1, 128);
          expect
            .soft(capture.bounds.height, `${id} row height`)
            .toBeLessThanOrEqual(26);
          expect
            .soft(
              Math.min(margins.top, margins.bottom),
              `${id} minimum vertical margin`
            )
            .toBeGreaterThanOrEqual(4);
          expect
            .soft(
              Math.abs(margins.top - margins.bottom),
              `${id} vertical centering`
            )
            .toBeLessThanOrEqual(3);
        }
      }
    );
  });

  it('fully lays out multilingual rows and the dynamic scroll range', async (context) => {
    await runSftpFixture(
      context,
      false,
      ['background=#000000'],
      [
        'treeview.view {',
        '  background-color: rgb(0, 0, 0);',
        '  color: rgb(255, 255, 255);',
        '  font-family: "Ubuntu Sans";',
        '  font-size: 11pt;',
        '}',
        '',
      ].join('\n'),
      async ({ app, evidence, localDirectory }) => {
        const localTree = expectTable(await app.getById('sftp_local_tree'));
        const verticalScrollbar = expectElementKind(
          await app.getById('sftp_local_vertical_scrollbar'),
          'scrollbar'
        );
        await findRow(localTree, 'hello.txt');
        const initialFirstRow = await captureRowBounds(localTree, 0);
        const initialSecondRow = await captureRowBounds(localTree, 1);
        const rowPitch = initialSecondRow.y - initialFirstRow.y;

        const japaneseName = 'a-オリジナルサウンドトラック 英雄伝説Ⅳ.txt';
        await Promise.all([
          writeFile(join(localDirectory, japaneseName), 'music\n'),
          ...Array.from({ length: 160 }, (_, index) =>
            writeFile(
              join(
                localDirectory,
                `scroll-entry-${String(index).padStart(3, '0')}.txt`
              ),
              'file\n'
            )
          ),
        ]);
        await expectElementKind(
          await app.getById('sftp_local_refresh_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await localTree.getRowCount()).toBe(163);
        });

        const scrollRange = await verticalScrollbar.valueInfo();
        expect.soft(scrollRange.maximum).toBeGreaterThanOrEqual(rowPitch * 140);

        const japaneseRow = await findRow(localTree, japaneseName);
        const japaneseCell = await localTree.cellAt(japaneseRow, 0);
        const japaneseCapture = await japaneseCell?.capture();
        if (japaneseCapture === undefined) {
          throw new Error('Japanese SFTP row did not expose bounds');
        }
        await evidence.captureEvidence(
          'sftp-multilingual-row',
          async () => japaneseCapture
        );
        const japaneseMargins = brightInkVerticalMargins(
          japaneseCapture,
          0,
          1,
          224
        );
        expect.soft(japaneseCapture.bounds.height).toBeLessThanOrEqual(26);
        expect.soft(japaneseMargins.bottom).toBeGreaterThanOrEqual(5);
        expect
          .soft(Math.abs(japaneseMargins.top - japaneseMargins.bottom))
          .toBeLessThanOrEqual(1);

        const treeCapture = await localTree.capture();
        const initialTreeMargins = brightInkVerticalMargins(
          treeCapture,
          0.1,
          0.45,
          128
        );
        expect.soft(initialTreeMargins.bottom).toBeLessThanOrEqual(rowPitch);
        await app.input.moveMouseTo(
          Math.round(treeCapture.bounds.x + treeCapture.bounds.width / 2),
          Math.round(treeCapture.bounds.y + treeCapture.bounds.height / 2)
        );
        await app.input.scrollWheel(0, 240);
        const bottomScrollRange = await verticalScrollbar.valueInfo();
        expect(bottomScrollRange.value).toBeGreaterThan(scrollRange.value);
        expect(
          bottomScrollRange.maximum - bottomScrollRange.value
        ).toBeLessThanOrEqual(treeCapture.bounds.height + rowPitch);
        const finalRow = (await localTree.getRowCount()) - 1;
        const finalRowBounds = await captureRowBounds(localTree, finalRow);
        expect(finalRowBounds.y).toBeGreaterThanOrEqual(treeCapture.bounds.y);
        expect(finalRowBounds.y + finalRowBounds.height).toBeLessThanOrEqual(
          treeCapture.bounds.y + treeCapture.bounds.height
        );
        const bottomCapture = await evidence.captureEvidence(
          'sftp-scrolled-tree-bottom',
          async () => localTree.capture()
        );
        const bottomTreeMargins = brightInkVerticalMargins(
          bottomCapture,
          0.1,
          0.45,
          128
        );
        expect.soft(bottomTreeMargins.bottom).toBeLessThanOrEqual(rowPitch);
        expect
          .soft(bottomTreeMargins.top)
          .toBeLessThanOrEqual(
            initialTreeMargins.top + Math.floor(rowPitch / 2)
          );

        await app.input.scrollWheel(0, -240);
        const topScrollRange = await verticalScrollbar.valueInfo();
        expect(topScrollRange.value).toBeLessThanOrEqual(
          topScrollRange.minimum + rowPitch
        );
        const firstRowBounds = await captureRowBounds(localTree, 0);
        expect(firstRowBounds.y).toBeGreaterThanOrEqual(treeCapture.bounds.y);
        expect(firstRowBounds.y + firstRowBounds.height).toBeLessThanOrEqual(
          treeCapture.bounds.y + treeCapture.bounds.height
        );
        const topCapture = await evidence.captureEvidence(
          'sftp-scrolled-tree-top',
          async () => localTree.capture()
        );
        expect
          .soft(brightInkVerticalMargins(topCapture, 0.1, 0.45, 128).bottom)
          .toBeLessThanOrEqual(rowPitch);
      }
    );
  });

  it('sends and receives selected items from the pane context menus', async (context) => {
    await runSftpFixture(
      context,
      false,
      [],
      undefined,
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
      undefined,
      async ({ app, evidence }) => {
        const background = [0x18, 0x3c, 0x58] as const;
        const componentBackground = [0x1b, 0x45, 0x65] as const;
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
