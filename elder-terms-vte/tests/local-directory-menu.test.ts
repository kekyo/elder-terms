import {
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
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

describe('Local directory menu', () => {
  for (const [failure, language] of [
    [false, 'en'],
    [true, 'en'],
    [false, 'ja'],
  ] as const) {
    it(`${language}: ${failure ? 'reports a failed file manager launch' : 'opens exactly one local directory using the desktop association'}`, async (context) => {
      const directory = await mkdtemp(
        join(tmpdir(), 'elder-terms-open-directory-')
      );
      const evidence = createTestEvidence(context);
      const local = join(directory, 'local');
      const nested = join(local, '資料 # folder');
      const data = join(directory, 'data');
      const config = join(directory, 'config');
      const executable = join(directory, 'file-manager.mjs');
      const calls = join(directory, 'calls.json');
      await mkdir(nested, { recursive: true });
      await mkdir(join(data, 'applications'), { recursive: true });
      await mkdir(config);
      await symlink('/usr/share/mime', join(data, 'mime'));
      await writeFile(join(local, 'hello.txt'), 'hello');
      await writeFile(join(nested, 'inside 日本語 #.txt'), 'nested');
      await symlink(nested, join(local, 'directory-link'));
      await writeFile(calls, '');
      await writeFile(
        executable,
        `#!${process.execPath}
import { appendFile } from 'node:fs/promises';
const path = ${JSON.stringify(calls)};
await appendFile(path, JSON.stringify(process.argv.slice(2)) + '\\n');
`
      );
      await chmod(executable, 0o755);
      await writeFile(
        join(data, 'applications', 'fixture-file-manager.desktop'),
        '[Desktop Entry]\nType=Application\nName=Fixture file manager\n' +
          `Exec="${executable}" %f\nMimeType=inode/directory;\nTerminal=false\n` +
          (failure
            ? `Path=${join(directory, 'missing-working-directory')}\n`
            : '')
      );
      await writeFile(
        join(config, 'mimeapps.list'),
        '[Default Applications]\ninode/directory=fixture-file-manager.desktop;\n'
      );
      const profile = join(directory, 'ftp.ini');
      await writeFile(
        profile,
        [
          '[general]',
          'type=ftp',
          '[ftp]',
          'address=fixture.example',
          'username=fixture-user',
          `local_directory=${local}`,
          'remote_directory=/remote',
          '',
        ].join('\n')
      );
      const launcher = createGtkAppLauncher({
        // This suite exercises directory association fallback without desktop services.
        accessibilitySession: 'minimal',
        appPath: fileURLToPath(
          new URL(
            '../../.build/elder-terms-vte/elder-terms-file-transfer',
            import.meta.url
          )
        ),
        env: {
          LANGUAGE: language,
          LC_ALL: language === 'ja' ? 'ja_JP.UTF-8' : 'C.UTF-8',
          ELDER_TERMS_LOCALE_DIR: fileURLToPath(
            new URL('../../.build/po/', import.meta.url)
          ),
          XDG_CONFIG_HOME: config,
          XDG_CONFIG_DIRS: config,
          XDG_DATA_HOME: data,
          XDG_DATA_DIRS: data,
        },
        xvfbTrayHost: true,
        onSystemOutput: evidence.recordSystemOutputEvent,
      });
      const apps: GtkApp[] = [];
      try {
        const app = await launcher.launch(['--test-fixture', '-c', profile], {
          onOutput: evidence.recordAppOutputEvent,
        });
        apps.push(app);
        await expectElementKind(
          await app.getById('file_transfer_prompt_secondary_entry'),
          'entry'
        ).setText('secret');
        await expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        ).click();
        const tree = expectElementKind(
          await app.getById('file_transfer_local_tree'),
          'table'
        );
        const rowFor = async (table: GtkTableElement, name: string) =>
          await waitForResult(async () => {
            expect((await table.info()).states).toContain('sensitive');
            for (let row = 0; row < (await table.getRowCount()); row++) {
              if ((await (await table.cellAt(row, 0))?.info())?.name === name)
                return row;
            }
            throw new Error(`Missing row: ${name}`);
          });
        const openMenu = async (table: GtkTableElement, name: string) => {
          const row = await rowFor(table, name);
          for (const selected of await table.selectedRows())
            await table.deselectRow(selected);
          await table.selectRow(row);
          const cell = await table.cellAt(row, 0);
          if (!cell) throw new Error(`Missing cell: ${name}`);
          const bounds = (await cell.capture()).bounds;
          await app.input.moveMouseTo(
            Math.round(bounds.x + bounds.width / 2),
            Math.round(bounds.y + bounds.height / 2)
          );
          await app.input.setMouseButton('right', true);
          await app.input.setMouseButton('right', false);
          const menuItem = await app.getById(
            table === tree
              ? 'file_transfer_local_open_directory_item'
              : 'file_transfer_remote_new_directory_item'
          );
          await waitForResult(async () =>
            expect((await menuItem.info()).states).toContain('showing')
          );
          return row;
        };
        const remote = expectElementKind(
          await app.getById('file_transfer_remote_tree'),
          'table'
        );
        await rowFor(remote, 'readme.txt');
        await openMenu(tree, 'hello.txt');
        const item = expectElementKind(
          await app.getById('file_transfer_local_open_directory_item'),
          'menuItem'
        );
        expect((await item.info()).name).toBe(
          language === 'ja'
            ? '親ディレクトリを開く'
            : 'Open containing directory'
        );
        expect((await item.info()).states).toContain('enabled');
        await item.click();
        if (failure) {
          const notice = await app.getById(
            'file_transfer_operation_error_dialog'
          );
          await waitForResult(async () =>
            expect((await notice.info()).states).toContain('showing')
          );
          expect(
            (await readFile(calls, 'utf8'))
              .split('\n')
              .filter(Boolean)
              .map((line) => JSON.parse(line))
          ).toEqual([]);
          await evidence.captureEvidence('directory-launch-failed', async () =>
            notice.capture()
          );
          return;
        }
        const expected = [[local]];
        await waitForResult(async () =>
          expect(
            (await readFile(calls, 'utf8'))
              .split('\n')
              .filter(Boolean)
              .map((line) => JSON.parse(line))
          ).toEqual(expected)
        );
        await openMenu(tree, '資料 # folder');
        expect((await item.info()).name).toBe(
          language === 'ja' ? 'このディレクトリを開く' : 'Open this directory'
        );
        await item.click();
        expected.push([nested]);
        await waitForResult(async () =>
          expect(
            (await readFile(calls, 'utf8'))
              .split('\n')
              .filter(Boolean)
              .map((line) => JSON.parse(line))
          ).toEqual(expected)
        );
        // Expanding, rather than navigating, checks that a file's parent comes
        // from its full row path instead of the pane's current directory.
        const folder = await rowFor(tree, '資料 # folder');
        const folderCell = await tree.cellAt(folder, 0);
        if (!folderCell) throw new Error('Folder cell missing');
        const folderBounds = (await folderCell.capture()).bounds;
        const folderTreeBounds = (await tree.capture()).bounds;
        // The expander is outside the accessible cell's bounds.
        await app.input.moveMouseTo(
          folderTreeBounds.x + 10,
          Math.round(folderBounds.y + folderBounds.height / 2)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await openMenu(tree, 'inside 日本語 #.txt');
        expect((await item.info()).name).toBe(
          language === 'ja'
            ? '親ディレクトリを開く'
            : 'Open containing directory'
        );
        await item.click();
        expected.push([nested]);
        await waitForResult(async () =>
          expect(
            (await readFile(calls, 'utf8'))
              .split('\n')
              .filter(Boolean)
              .map((line) => JSON.parse(line))
          ).toEqual(expected)
        );
        await openMenu(tree, 'directory-link');
        expect((await item.info()).name).toBe(
          language === 'ja'
            ? '親ディレクトリを開く'
            : 'Open containing directory'
        );
        await item.click();
        expected.push([local]);
        await waitForResult(async () =>
          expect(
            (await readFile(calls, 'utf8'))
              .split('\n')
              .filter(Boolean)
              .map((line) => JSON.parse(line))
          ).toEqual(expected)
        );
        const fileRow = await rowFor(tree, 'hello.txt');
        const directoryRow = await rowFor(tree, '資料 # folder');
        for (const selected of await tree.selectedRows())
          await tree.deselectRow(selected);
        await tree.selectRow(fileRow);
        await tree.selectRow(directoryRow);
        const fileCell = await tree.cellAt(fileRow, 0);
        if (!fileCell) throw new Error('File cell missing');
        const fileBounds = (await fileCell.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round(fileBounds.x + 60),
          Math.round(fileBounds.y + fileBounds.height / 2)
        );
        await app.input.setMouseButton('right', true);
        await app.input.setMouseButton('right', false);
        expect(await tree.selectedRows()).toHaveLength(2);
        await waitForResult(async () => {
          expect((await item.info()).states).toContain('showing');
          expect((await item.info()).states).not.toContain('enabled');
        });
        const disabledBounds = (await item.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round(disabledBounds.x + disabledBounds.width / 2),
          Math.round(disabledBounds.y + disabledBounds.height / 2)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await app.input.pressKey('Escape');
        const treeBounds = (await tree.capture()).bounds;
        await app.input.moveMouseTo(
          treeBounds.x + 100,
          treeBounds.y + treeBounds.height - 10
        );
        await app.input.setMouseButton('right', true);
        await app.input.setMouseButton('right', false);
        await waitForResult(async () => {
          expect(await tree.selectedRows()).toEqual([]);
          expect((await item.info()).states).toContain('showing');
          expect((await item.info()).states).not.toContain('enabled');
        });
        await app.input.pressKey('Escape');
        await openMenu(remote, 'readme.txt');
        expect(
          await app.findById('file_transfer_remote_open_directory_item')
        ).toBeUndefined();
        expect((await item.info()).states).not.toContain('showing');
        await app.input.pressKey('Escape');
        expect(
          (await readFile(calls, 'utf8'))
            .split('\n')
            .filter(Boolean)
            .map((line) => JSON.parse(line))
        ).toEqual(expected);
        await evidence.captureEvidence('local-directory-menu', async () =>
          app.capture()
        );
      } finally {
        try {
          await evidence.flushOutputs(apps, launcher);
        } finally {
          try {
            await launcher.release();
          } finally {
            await evidence.release();
            await rm(directory, { recursive: true, force: true });
          }
        }
      }
    });
  }
});
