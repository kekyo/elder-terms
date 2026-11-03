// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const build = fileURLToPath(new URL('../../.build/', import.meta.url));
const environment = {
  ...process.env,
  LD_PRELOAD: undefined,
  LD_LIBRARY_PATH: undefined,
  LD_DEBUG_OUTPUT: undefined,
  LD_DEBUG: 'bindings',
  LD_BIND_NOW: '1',
  MALLOC_CONF: 'stats_print:true',
  // Exercise startup without opening windows or contacting a desktop session.
  GDK_BACKEND: 'x11',
  DISPLAY: '',
  LC_ALL: 'C',
};

describe('the bundled heap allocator', () => {
  it.each([
    'elder-terms/elder-terms',
    'elder-terms-terminal/elder-terms-terminal',
    'elder-terms-file-transfer/elder-terms-file-transfer',
  ])(
    'uses jemalloc for %s and its shared libraries without preloading',
    (path) => {
      const result = spawnSync(`${build}${path}`, ['--help'], {
        env: environment,
        encoding: 'utf8',
        maxBuffer: 16 * 1024 * 1024,
      });
      expect(result.error).toBeUndefined();
      expect(result.signal).toBeNull();
      expect([0, 1]).toContain(result.status);
      const bindings = result.stderr
        .split('\n')
        .filter((line) =>
          /normal symbol `(malloc|calloc|realloc|free|aligned_alloc|posix_memalign|malloc_usable_size|_Zn[aw][mj]|_Zd[al]Pv[mj]?)'/.test(
            line
          )
        );
      expect(bindings.length).toBeGreaterThan(0);
      for (const binding of bindings) {
        expect(binding).toMatch(/to .*\/libjemalloc\.so\.2 /);
      }
      expect(
        bindings.some((line) => /file .*\/libboxrig\.so\.0 /.test(line))
      ).toBe(true);
      expect(
        result.stderr.split('\n').find((line) => line.startsWith('Version:'))
      ).toBe('Version: "5.4.0-0-g7a34f18502e7b222724097cdcd499b437d189acc"');
    }
  );

  it('allocates and frees through jemalloc while matching with the shared boxrig core', () => {
    const result = spawnSync(
      `${build}elder-terms-terminal/boxrig-regex`,
      ['a|abc'],
      {
        input: 'xabcabc',
        env: environment,
        encoding: 'utf8',
        maxBuffer: 16 * 1024 * 1024,
      }
    );
    expect(result.error).toBeUndefined();
    expect(result.status).toBe(0);
    expect(result.stdout).toMatch(/^match 1 4 work=\d+\n$/);
    for (const name of ['malloc', 'calloc', 'realloc', 'free']) {
      const bindings = result.stderr
        .split('\n')
        .filter(
          (line) =>
            line.includes(`normal symbol \`${name}'`) &&
            /file .*\/libboxrig\.so\.0 /.test(line)
        );
      expect(bindings.length).toBeGreaterThan(0);
      for (const binding of bindings) {
        expect(binding).toMatch(/to .*\/libjemalloc\.so\.2 /);
      }
    }
  });
});
