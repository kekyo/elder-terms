// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const execute = promisify(execFile);

it('saves independent settings files, detects conflicts and retries partial failures', async () => {
  const result = await execute(fileURLToPath(new URL('../../.build/shared/settings-edit-test', import.meta.url)));
  expect(result.stdout).toContain('Settings edits passed');
});

it('resolves theme inheritance, explicit values, startup selection and invalid input', async () => {
  const result = await execute(fileURLToPath(new URL('../../.build/shared/named-settings-theme-test', import.meta.url)));
  expect(result.stdout).toContain('Named settings themes passed');
});
