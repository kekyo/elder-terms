import { execFile } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createGtkAppLauncher } from 'gestament';
import { describe, expect, it } from 'vitest';
import { expectElementKind } from './test-helpers';

describe('File transfer window icons', () => {
  for (const [kind, tlsMode] of [
    ['sftp', 'none'],
    ['ftp', 'none'],
    ['ftp', 'explicit'],
    ['ftp', 'implicit'],
  ]) {
    it(`publishes the bundled icon for ${kind} with ${tlsMode} TLS`, async () => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-icon-'));
      const launcher = createGtkAppLauncher({
        appPath: fileURLToPath(
          new URL(
            '../../.build/elder-terms-vte/elder-terms-file-transfer',
            import.meta.url
          )
        ),
        env: { LANGUAGE: 'en', LC_ALL: 'C.UTF-8', XDG_CONFIG_HOME: directory },
        xvfbTrayHost: true,
      });
      try {
        const config = join(directory, 'connection.ini');
        await writeFile(
          config,
          [
            '[general]',
            `type=${kind}`,
            '[ssh]',
            'address=fixture.example',
            '[sftp]',
            `local_directory=${directory}`,
            '[ftp]',
            'address=fixture.example',
            `tls_mode=${tlsMode}`,
            `local_directory=${directory}`,
            '',
          ].join('\n')
        );
        const app = await launcher.launch(['--test-fixture', '-c', config]);
        const window = expectElementKind(
          await app.getById('file_transfer_window'),
          'window'
        );
        const x11 = await window.x11Info();
        const icon = await promisify(execFile)(
          'xprop',
          ['-id', x11.windowId, '-len', '16', '32c', '_NET_WM_ICON'],
          { env: { ...process.env, ...(await app.environment()) } }
        );
        expect(icon.stdout).toMatch(
          /_NET_WM_ICON\(CARDINAL\) = [1-9]\d*, [1-9]\d*/u
        );
      } finally {
        await launcher.release();
        await rm(directory, { recursive: true, force: true });
      }
    });
  }
});
