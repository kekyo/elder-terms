import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkApp } from 'gestament';
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
          await app.getById('file_transfer_status_label'),
          'label'
        ).text()
      ).toBe('Authenticating');
      const passwordEntry = expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
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
      expect((await app.getById('file_transfer_local_tree')).kind).toBe(
        'table'
      );
      expect((await app.getById('file_transfer_remote_tree')).kind).toBe(
        'table'
      );
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
