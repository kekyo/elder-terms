import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkTableElement,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it, type TestContext } from 'vitest';
import {
  createTestEvidence,
  expectElementKind,
  type TestEvidence,
} from './test-helpers';

const sftpAppPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/elder-terms-sftp', import.meta.url)
);

interface SftpFixture {
  readonly app: GtkApp;
  readonly evidence: TestEvidence;
  readonly localDirectory: string;
}

const runSftpFixture = async (
  context: TestContext,
  pauseTransfer: boolean,
  body: (fixture: SftpFixture) => Promise<void>
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-sftp-'));
  const localDirectory = join(directory, 'local');
  const configPath = join(directory, 'sftp.ini');
  await mkdir(join(localDirectory, 'documents'), { recursive: true });
  await writeFile(join(localDirectory, 'hello.txt'), 'hello from local\n');
  await writeFile(
    configPath,
    [
      '[general]',
      'name=Fixture files',
      'type=sftp',
      '',
      '[ssh]',
      'address=fixture.example',
      '',
      '[sftp]',
      `local_directory=${localDirectory}`,
      'remote_directory=/remote',
      '',
    ].join('\n')
  );

  const evidence = createTestEvidence(context);
  const launcher = createGtkAppLauncher({
    appPath: sftpAppPath,
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
      await evidence.release();
      await rm(directory, { recursive: true, force: true });
    }
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

describe('SFTP window', () => {
  it('shows independent local and remote trees and navigates both panes', async (context) => {
    await runSftpFixture(
      context,
      false,
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
    await runSftpFixture(context, false, async ({ app, localDirectory }) => {
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
        expect(await readFile(join(localDirectory, 'readme.txt'), 'utf8')).toBe(
          'hello from remote\n'
        );
      });
    });
  });

  it('dims both panes during transfer and cancels from the progress overlay', async (context) => {
    await runSftpFixture(context, true, async ({ app }) => {
      const localTree = expectTable(await app.getById('sftp_local_tree'));
      await openContextMenu(
        app,
        localTree,
        await findRow(localTree, 'hello.txt')
      );
      await expectElementKind(
        await app.getById('sftp_send_item'),
        'menuItem'
      ).click();

      const overlay = await app.getById('sftp_transfer_overlay');
      const dim = await app.getById('sftp_dim_overlay');
      await expectShowing(overlay);
      await expectShowing(dim);
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
    });
  });
});
