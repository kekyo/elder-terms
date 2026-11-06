// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const execute = promisify(execFile);

it('shares UI-created themes across connections and preserves overrides and invalid drafts', async () => {
  const result = await execute('xvfb-run', ['-a', fileURLToPath(new URL('../../.build/shared/named-theme-widget-test', import.meta.url))],
    { env: { ...process.env, G_DEBUG: 'fatal-criticals', NO_AT_BRIDGE: '1' } });
  expect(result.stdout).toContain('Theme widget passed');
});

it('saves independent settings files, detects conflicts and retries partial failures', async () => {
  const result = await execute(fileURLToPath(new URL('../../.build/shared/settings-edit-test', import.meta.url)));
  expect(result.stdout).toContain('Settings edits passed');
});

it('resolves theme inheritance, explicit values, startup selection and invalid input', async () => {
  const result = await execute(fileURLToPath(new URL('../../.build/shared/named-settings-theme-test', import.meta.url)));
  expect(result.stdout).toContain('Named settings themes passed');
});
