// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, it } from 'vitest';

const execute = promisify(execFile);

it('refuses terminal and transfer startup with an unresolved theme reference', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-unresolved-theme-'));
  try {
    await mkdir(join(directory, 'elder-terms'));
    for (const [application, type] of [
      ['elder-terms-terminal', 'local'],
      ['elder-terms-file-transfer', 'ftp'],
    ]) {
      const connection = join(directory, 'connection.ini');
      await writeFile(connection, `[general]\ntype=${type}\ntheme=missing\n`);
      let failure: unknown;
      try {
        await execute(
          'xvfb-run',
          [
            '-a',
            fileURLToPath(
              new URL(
                `../../.build/${application}/${application}`,
                import.meta.url
              )
            ),
            '-c',
            connection,
            '--test-fixture',
          ],
          {
            env: {
              ...process.env,
              XDG_CONFIG_HOME: directory,
              NO_AT_BRIDGE: '1',
            },
            timeout: 5000,
          }
        );
      } catch (error) {
        failure = error;
      }
      expect(failure).toMatchObject({ code: 1 });
      const output = failure as { stdout: string; stderr: string };
      expect(output.stdout + output.stderr).toContain(
        'Unresolved theme: missing'
      );
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

it('shares UI-created themes across connections and preserves overrides and invalid drafts', async () => {
  const result = await execute(
    'xvfb-run',
    [
      '-a',
      fileURLToPath(
        new URL('../../.build/shared/named-theme-widget-test', import.meta.url)
      ),
    ],
    { env: { ...process.env, G_DEBUG: 'fatal-criticals', NO_AT_BRIDGE: '1' } }
  );
  expect(result.stdout).toContain('Theme widget passed');
});

it('saves independent settings files, detects conflicts and retries partial failures', async () => {
  const result = await execute(
    fileURLToPath(
      new URL('../../.build/shared/settings-edit-test', import.meta.url)
    )
  );
  expect(result.stdout).toContain('Settings edits passed');
});

it('resolves theme inheritance, explicit values, startup selection and invalid input', async () => {
  const result = await execute(
    fileURLToPath(
      new URL('../../.build/shared/named-settings-theme-test', import.meta.url)
    )
  );
  expect(result.stdout).toContain('Named settings themes passed');
});
