import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir, userInfo } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkTableElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';

const ftpAppPath = fileURLToPath(
  new URL(
    '../../.build/elder-terms-vte/elder-terms-file-transfer',
    import.meta.url
  )
);

describe('FTP window', () => {
  it('uses the common dual-pane file transfer UI', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-ftp-'));
    const evidence = createTestEvidence(context);
    const apps: GtkApp[] = [];
    let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
    try {
      const configHome = join(directory, 'xdg-config');
      const localDirectory = join(directory, 'local');
      const configPath = join(directory, 'ftp.ini');
      await Promise.all([
        mkdir(configHome, { recursive: true }),
        mkdir(localDirectory, { recursive: true }),
      ]);
      await writeFile(join(localDirectory, 'local.txt'), 'local file\n');
      await writeFile(
        configPath,
        [
          '[general]',
          'name=Fixture FTP',
          'type=ftp',
          '',
          '[ftp]',
          'address=fixture.example',
          'username=fixture-user',
          `local_directory=${localDirectory}`,
          'remote_directory=/remote',
          '',
        ].join('\n')
      );

      launcher = createGtkAppLauncher({
        appPath: ftpAppPath,
        env: {
          LANGUAGE: 'en',
          LC_ALL: 'C.UTF-8',
          XDG_CONFIG_HOME: configHome,
        },
        onSystemOutput: evidence.recordSystemOutputEvent,
        xvfbTrayHost: true,
      });
      const app = await launcher.launch(['--test-fixture', '-c', configPath], {
        onOutput: evidence.recordAppOutputEvent,
      });
      apps.push(app);

      expect(await app.getWindowCount()).toBe(1);
      const window = expectElementKind(
        await app.getById('file_transfer_window'),
        'window'
      );
      expect((await window.info()).name).toBe('Fixture FTP — FTP');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_title_label'),
          'label'
        ).text()
      ).toBe('FTP authentication');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_message_label'),
          'label'
        ).text()
      ).toContain('To log in anonymously, enter anonymous as the user name.');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_entry_label'),
          'label'
        ).text()
      ).toBe('User name');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_secondary_entry_label'),
          'label'
        ).text()
      ).toBe('Password:');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_status_label'),
          'label'
        ).text()
      ).toBe('Authenticating');
      const usernameEntry = expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      );
      expect(await usernameEntry.text()).toBe('fixture-user');
      const passwordEntry = expectElementKind(
        await app.getById('file_transfer_prompt_secondary_entry'),
        'entry'
      );
      await passwordEntry.setText('fixture-secret');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();

      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await app.getById('file_transfer_local_path_entry'),
            'entry'
          ).text()
        ).toBe(localDirectory);
        expect(
          await expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          ).text()
        ).toBe('/remote');
        expect(
          await expectElementKind(
            await app.getById('file_transfer_status_label'),
            'label'
          ).text()
        ).toBe('Ready');
      });
      const localTreeElement = await app.getById('file_transfer_local_tree');
      expect(localTreeElement.kind).toBe('table');
      const localTree = localTreeElement as GtkTableElement;
      const localFileRow = await waitForResult(async () => {
        const rows = await localTree.getRowCount();
        for (let row = 0; row < rows; row += 1) {
          const cell = await localTree.cellAt(row, 0);
          if ((await cell?.info())?.name === 'local.txt') {
            return row;
          }
        }
        throw new Error('FTP local file was not listed');
      });
      await localTree.selectRow(localFileRow);
      const localFileCell = await localTree.cellAt(localFileRow, 0);
      const localFileBounds = (await localFileCell?.capture())?.bounds;
      if (localFileBounds === undefined) {
        throw new Error('FTP local file did not expose bounds');
      }
      await app.input.moveMouseTo(
        Math.round(localFileBounds.x + localFileBounds.width / 2),
        Math.round(localFileBounds.y + localFileBounds.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);
      await expectElementKind(
        await app.getById('file_transfer_local_hash_item'),
        'menuItem'
      ).click();
      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await app.getById('file_transfer_prompt_title_label'),
            'label'
          ).text()
        ).toBe('File hash values');
      });
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_message_label'),
          'label'
        ).text()
      ).toBe('local.txt');
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_monospace_message_label'),
          'label'
        ).text()
      ).toBe(
        [
          'MD5: 47fb4296aacb9541c949c92d015f2d86',
          'SHA1: feb10f467848ecd032895fd1da1f7ec052b4b504',
          'SHA256: 3396c739f6e425babf76d33999d0fe3afcf3157b1ce2e68afc33968d49c72b94',
        ].join('\n')
      );
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();

      const remoteTreeElement = await app.getById('file_transfer_remote_tree');
      expect(remoteTreeElement.kind).toBe('table');
      const remoteTree = remoteTreeElement as GtkTableElement;
      const archiveRow = await waitForResult(async () => {
        const rows = await remoteTree.getRowCount();
        for (let row = 0; row < rows; row += 1) {
          const cell = await remoteTree.cellAt(row, 0);
          if ((await cell?.info())?.name === 'archive') {
            return row;
          }
        }
        throw new Error('FTP remote directory was not listed');
      });
      const archiveCell = await remoteTree.cellAt(archiveRow, 0);
      const archiveBounds = (await archiveCell?.capture())?.bounds;
      if (archiveBounds === undefined) {
        throw new Error('FTP remote directory did not expose bounds');
      }
      const remoteTreeBounds = (await remoteTree.capture()).bounds;
      await app.input.moveMouseTo(
        Math.round(archiveBounds.x + archiveBounds.width / 2),
        Math.round(archiveBounds.y + archiveBounds.height / 2)
      );
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          ).text()
        ).toBe('/remote/archive');
      });
      const longFileRow = await waitForResult(async () => {
        const rows = await remoteTree.getRowCount();
        for (let row = 0; row < rows; row += 1) {
          const cell = await remoteTree.cellAt(row, 0);
          if (
            (await cell?.info())?.name ===
            'long-remote-filename-that-keeps-extending-until-the-name-column-needs-more-space-than-the-file-transfer-pane-allows.log'
          ) {
            return row;
          }
        }
        throw new Error('FTP expanded remote file was not listed');
      });
      expect(await remoteTree.getColumnCount()).toBe(3);
      const longFileSizeCell = await remoteTree.cellAt(longFileRow, 1);
      const longFileModifiedCell = await remoteTree.cellAt(longFileRow, 2);
      if (
        longFileSizeCell === undefined ||
        longFileModifiedCell === undefined
      ) {
        throw new Error('FTP remote metadata cells were not exposed');
      }
      expect((await longFileSizeCell.info()).name).toBe('17 bytes');
      expect((await longFileModifiedCell.info()).name).toMatch(
        /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/
      );
      const longFileSizeCapture = await longFileSizeCell.capture();
      const longFileModifiedCapture = await longFileModifiedCell.capture();
      expect(longFileSizeCapture.bounds.width).toBeGreaterThanOrEqual(40);
      expect(longFileModifiedCapture.bounds.width).toBeGreaterThanOrEqual(96);
      expect(
        longFileModifiedCapture.bounds.x + longFileModifiedCapture.bounds.width
      ).toBeLessThanOrEqual(remoteTreeBounds.x + remoteTreeBounds.width);

      await expectElementKind(
        await app.getById('file_transfer_remote_up_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          ).text()
        ).toBe('/remote');
      });

      const readmeRow = await waitForResult(async () => {
        const rows = await remoteTree.getRowCount();
        for (let row = 0; row < rows; row += 1) {
          const cell = await remoteTree.cellAt(row, 0);
          if ((await cell?.info())?.name === 'readme.txt') {
            return row;
          }
        }
        throw new Error('FTP remote file was not listed');
      });
      await remoteTree.selectRow(readmeRow);
      const readmeCell = await remoteTree.cellAt(readmeRow, 0);
      const readmeBounds = (await readmeCell?.capture())?.bounds;
      if (readmeBounds === undefined) {
        throw new Error('FTP remote file did not expose bounds');
      }
      await app.input.moveMouseTo(
        Math.round(readmeBounds.x + readmeBounds.width / 2),
        Math.round(readmeBounds.y + readmeBounds.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);

      expect(
        await app.findById('file_transfer_remote_hash_item')
      ).toBeUndefined();
      const rename = expectElementKind(
        await app.getById('file_transfer_remote_rename_item'),
        'menuItem'
      );
      expect((await rename.info()).states).toContain('showing');
      await rename.click();
      const renameEntry = expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      );
      expect(await renameEntry.text()).toBe('readme.txt');
      await renameEntry.setText('ftp-guide.txt');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        const rows = await remoteTree.getRowCount();
        for (let row = 0; row < rows; row += 1) {
          const cell = await remoteTree.cellAt(row, 0);
          if ((await cell?.info())?.name === 'ftp-guide.txt') {
            return;
          }
        }
        throw new Error('FTP remote rename was not reflected in the tree');
      });

      const renamedRow = await waitForResult(async () => {
        const rows = await remoteTree.getRowCount();
        for (let row = 0; row < rows; row += 1) {
          const cell = await remoteTree.cellAt(row, 0);
          if ((await cell?.info())?.name === 'ftp-guide.txt') {
            return row;
          }
        }
        throw new Error('FTP renamed file was not listed');
      });
      await remoteTree.selectRow(renamedRow);
      const renamedCell = await remoteTree.cellAt(renamedRow, 0);
      const renamedBounds = (await renamedCell?.capture())?.bounds;
      if (renamedBounds === undefined) {
        throw new Error('FTP renamed file did not expose bounds');
      }
      await app.input.moveMouseTo(
        Math.round(renamedBounds.x + renamedBounds.width / 2),
        Math.round(renamedBounds.y + renamedBounds.height / 2)
      );
      await app.input.setMouseButton('right', true);
      await app.input.setMouseButton('right', false);
      const remove = expectElementKind(
        await app.getById('file_transfer_remote_delete_item'),
        'menuItem'
      );
      expect((await remove.info()).states).toContain('showing');
      await remove.click();
      expect(
        await expectElementKind(
          await app.getById('file_transfer_prompt_title_label'),
          'label'
        ).text()
      ).toBe('Delete selected item?');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        const rows = await remoteTree.getRowCount();
        for (let row = 0; row < rows; row += 1) {
          const cell = await remoteTree.cellAt(row, 0);
          if ((await cell?.info())?.name === 'ftp-guide.txt') {
            throw new Error('FTP deleted file was still listed');
          }
        }
        expect(
          await expectElementKind(
            await app.getById('file_transfer_status_label'),
            'label'
          ).text()
        ).toBe('Deleted 1 item');
      });
    } finally {
      try {
        await evidence.flushOutputs(apps, launcher);
      } finally {
        if (launcher !== undefined) {
          await launcher.release();
        }
        await evidence.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  });

  it('requires an explicit user name and defaults it to the current user', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-ftp-auth-'));
    const evidence = createTestEvidence(context);
    const apps: GtkApp[] = [];
    let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
    try {
      const configHome = join(directory, 'xdg-config');
      const localDirectory = join(directory, 'local');
      const configPath = join(directory, 'ftp.ini');
      await Promise.all([
        mkdir(configHome, { recursive: true }),
        mkdir(localDirectory, { recursive: true }),
      ]);
      await writeFile(
        configPath,
        [
          '[general]',
          'name=Fixture FTP authentication',
          'type=ftp',
          '',
          '[ftp]',
          'address=fixture.example',
          `local_directory=${localDirectory}`,
          'remote_directory=/remote',
          '',
        ].join('\n')
      );

      launcher = createGtkAppLauncher({
        appPath: ftpAppPath,
        env: {
          LANGUAGE: 'en',
          LC_ALL: 'C.UTF-8',
          XDG_CONFIG_HOME: configHome,
        },
        onSystemOutput: evidence.recordSystemOutputEvent,
        xvfbTrayHost: true,
      });
      const app = await launcher.launch(['--test-fixture', '-c', configPath], {
        onOutput: evidence.recordAppOutputEvent,
      });
      apps.push(app);

      const usernameEntry = expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      );
      const passwordEntry = expectElementKind(
        await app.getById('file_transfer_prompt_secondary_entry'),
        'entry'
      );
      await waitForResult(async () => {
        expect(await usernameEntry.text()).toBe(userInfo().username);
      });

      await usernameEntry.setText('');
      await passwordEntry.setText('anonymous@example.invalid');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await app.getById('file_transfer_prompt_message_label'),
            'label'
          ).text()
        ).toContain('User name must not be empty.');
      });

      await usernameEntry.setText('anonymous');
      await passwordEntry.setText('anonymous@example.invalid');
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
        ).toBe('Ready');
      });
    } finally {
      try {
        await evidence.flushOutputs(apps, launcher);
      } finally {
        if (launcher !== undefined) {
          await launcher.release();
        }
        await evidence.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  });
});
