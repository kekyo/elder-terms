// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFileSync } from 'node:child_process';
import assert from 'node:assert/strict';

for (const annotations of [0, 3]) {
  const rows = execFileSync(process.argv[2], ['30', String(annotations), '2'], {
    encoding: 'utf8',
  })
    .trim()
    .split('\n')
    .slice(1)
    .map((line) => line.split('\t'));
  assert.equal(rows.length, 2);
  for (const row of rows) {
    assert.equal(
      Number(row[1]),
      30,
      'Actual history stays unchanged while typing'
    );
    assert.equal(
      Number(row[2]),
      annotations,
      'All requested annotations are retained'
    );
    assert.equal(
      Number(row[3]),
      annotations === 0 ? 0 : 1,
      'The visible annotation remains valid after input'
    );
    assert.ok(Number(row[4]) > 0, 'The text-changing input is accepted');
    assert.ok(
      row.slice(5).every((value) => Number(value) >= 0),
      'Durations are nonnegative'
    );
  }
}
console.log('Headless text updates and retained annotation ranges passed');
