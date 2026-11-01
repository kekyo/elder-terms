// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { readFile, writeFile } from 'node:fs/promises';
import { runScriptOnceToText, type FunCityLogEntry } from 'funcity';

const [input, output, version] = process.argv.slice(2);
if (input === undefined || output === undefined || version === undefined) {
  throw new Error(
    'Usage: prepare-appstream-metainfo.mts <template> <output> <version>'
  );
}
// Match the package version alphabet, which is also safe in an XML attribute.
if (!/^[0-9][0-9A-Za-z.+:~-]*$/.test(version)) {
  throw new Error(`Invalid application version: ${version}`);
}

const logs: FunCityLogEntry[] = [];
const text = await runScriptOnceToText(await readFile(input, 'utf8'), {
  sourceId: input,
  variables: new Map([
    ['version', version],
    ['date', new Date().toISOString().slice(0, 10)],
  ]),
  logs,
});
if (text === undefined || logs.length !== 0) {
  throw new Error(
    `Unable to render AppStream metadata: ${logs.map((entry) => entry.description).join('; ')}`
  );
}
await writeFile(output, text);
