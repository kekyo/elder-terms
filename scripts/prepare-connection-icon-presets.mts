// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { copyFile, mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { runScriptOnceToText } from 'funcity';

const notoRevision = 'e20cbc2bbec1926686be9f9bee7d1d2cfa1fea0e';
const materialRevision = '49d4db35df873165d6bd6ba09b063c7dafbac2f4';
const emojiTestSha256 = '8f3735cda1f92a779d78af67cf86066bb1f07143dc22f2ac29394d9bc57ab21a';
const [notoRoot, materialRoot, emojiTestPath] = process.argv.slice(2);
if (!notoRoot || !materialRoot || !emojiTestPath) {
  throw new Error('Usage: node scripts/prepare-connection-icon-presets.mts NOTO_REPO MATERIAL_REPO EMOJI_TEST_TXT');
}
const revision = (directory: string): string =>
  execFileSync('git', ['-C', directory, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision(notoRoot) !== notoRevision || revision(materialRoot) !== materialRevision) {
  throw new Error('Preset source revisions do not match the pinned artwork');
}

const outputRoot = join(import.meta.dirname, '..', 'data', 'connection-icons');
const renderText = async (template: string, values: Record<string, string>): Promise<string> => {
  const rendered = await runScriptOnceToText(template, {
    variables: new Map(Object.entries(values)),
    sourceId: 'prepare-connection-icon-presets',
  });
  if (rendered === undefined) throw new Error('Unable to render icon catalog text');
  return rendered;
};
const emojiNames = new Map<string, string>();
const emojiTest = await readFile(emojiTestPath);
if (createHash('sha256').update(emojiTest).digest('hex') !== emojiTestSha256) {
  throw new Error('Unicode emoji-test.txt does not match the pinned edition');
}
for (const line of emojiTest.toString('utf8').split('\n')) {
  const match = line.match(/^([0-9A-F ]+)\s*;\s*[^#]+#\s+\S+\s+E\S+\s+(.+)$/);
  if (!match) continue;
  const key = match[1].trim().toLowerCase().split(/\s+/)
    .filter((codepoint) => codepoint !== 'fe0f').join('_');
  if (!emojiNames.has(key)) emojiNames.set(key, match[2].trim());
}
const sourceGlyphName = (codepoints: string): string => {
  if (codepoints === '0023') return 'Keycap number sign';
  if (codepoints === '002a') return 'Keycap asterisk';
  if (/^003[0-9]$/.test(codepoints)) return `Keycap ${codepoints.slice(-1)}`;
  const regionalIndicator = Number.parseInt(codepoints, 16);
  if (regionalIndicator >= 0x1f1e6 && regionalIndicator <= 0x1f1ff) {
    return `Regional indicator ${String.fromCharCode(65 + regionalIndicator - 0x1f1e6)}`;
  }
  if (codepoints === '20e3') return 'Combining enclosing keycap';
  if (codepoints === 'fe82b') return 'Unknown flag';
  return `Unicode ${codepoints.toUpperCase().replaceAll('_', ' ')}`;
};

const entries: { specification: string; name: string; family: string; file: string }[] = [];
const notoInput = join(notoRoot, '2D', 'png', '128');
const notoOutput = join(outputRoot, 'noto');
await mkdir(notoOutput, { recursive: true });
for (const filename of (await readdir(notoInput)).filter((name) => name.endsWith('.png')).sort()) {
  const id = filename.slice(0, -4);
  const codepoints = id.startsWith('emoji_u')
    ? id.slice(7)
    : id.replace(/^u/, '').replaceAll('-u', '_');
  const normalized = codepoints.split('_').filter((codepoint) => codepoint !== 'fe0f').join('_');
  const name = emojiNames.get(normalized) ?? sourceGlyphName(codepoints);
  await copyFile(join(notoInput, filename), join(notoOutput, filename));
  entries.push({ specification: `noto:${id}`, name, family: 'noto', file: `noto/${filename}` });
}

const materialInput = join(materialRoot, 'src');
const materialSources: string[] = [];
for (const category of (await readdir(materialInput)).sort()) {
  const categoryPath = join(materialInput, category);
  for (const id of (await readdir(categoryPath)).sort()) {
    const source = join(categoryPath, id, 'materialicons', '24px.svg');
    try { await readFile(source); } catch { continue; }
    const relativeSource = `material-svg/${category}/${id}.svg`;
    const destination = join(outputRoot, relativeSource);
    await mkdir(join(outputRoot, 'material-svg', category), { recursive: true });
    await copyFile(source, destination);
    materialSources.push(relativeSource);
    entries.push({
      specification: `material:${category}/${id}`,
      name: id.replaceAll('_', ' '),
      family: 'material',
      file: `material/${category}/${id}.png`,
    });
  }
}

const sorted = entries.sort((left, right) =>
  left.family.localeCompare(right.family) || left.name.localeCompare(right.name) ||
  left.specification.localeCompare(right.specification));
await writeFile(join(outputRoot, 'catalog.tsv'),
  (await Promise.all(sorted.map((entry) => renderText(
    '{{specification}}\t{{name}}\t{{family}}\n', entry)))).join(''));
const notoFiles = ['catalog.tsv', ...sorted.filter((entry) => entry.family === 'noto')
  .map((entry) => entry.file)].sort();
const notoXml = [
  '<?xml version="1.0" encoding="UTF-8"?>',
  '<gresources>',
  '  <gresource prefix="/net/kekyo/elder-terms/presets">',
  ...(await Promise.all(notoFiles.map((file) => renderText(
    '    <file alias="{{file}}">connection-icons/{{file}}</file>', { file })))),
  '  </gresource>',
  '</gresources>',
  '',
].join('\n');
await writeFile(join(import.meta.dirname, '..', 'shared', 'connection-icon-noto.gresource.xml'), notoXml);
const materialFiles = sorted.filter((entry) => entry.family === 'material')
  .flatMap((entry) => [entry.file, entry.file.replace(/^material\//, 'material-white/')]);
const materialXml = [
  '<?xml version="1.0" encoding="UTF-8"?>',
  '<gresources>',
  '  <gresource prefix="/net/kekyo/elder-terms/presets">',
  ...(await Promise.all(materialFiles.map((file) => renderText(
    '    <file alias="{{file}}">{{file}}</file>', { file })))),
  '  </gresource>',
  '</gresources>',
  '',
].join('\n');
await writeFile(join(import.meta.dirname, '..', 'shared', 'connection-icon-material.gresource.xml'), materialXml);
const mesonFiles = [
  'material_icon_svg_sources = files(',
  ...(await Promise.all(materialSources.sort().map((file) => renderText(
    "  '../../data/connection-icons/{{file}}',", { file })))),
  ')',
  '',
].join('\n');
await mkdir(join(import.meta.dirname, '..', 'shared', 'material-icon-sources'), { recursive: true });
await writeFile(join(import.meta.dirname, '..', 'shared', 'material-icon-sources', 'meson.build'), mesonFiles);
await writeFile(join(outputRoot, 'NOTO-LICENSE'),
  execFileSync('git', ['-C', notoRoot, 'show', 'HEAD:2D/svg/LICENSE']));
await writeFile(join(outputRoot, 'NOTO-FLAGS-NOTICE'),
  execFileSync('git', ['-C', notoRoot, 'show', 'HEAD:third_party/region-flags/LICENSE']));
await writeFile(join(outputRoot, 'MATERIAL-LICENSE'),
  execFileSync('git', ['-C', materialRoot, 'show', 'HEAD:LICENSE']));
await writeFile(join(outputRoot, 'SOURCES.md'), [
  '# Connection icon artwork',
  '',
  `- Noto Color Emoji: [googlefonts/noto-emoji](https://github.com/googlefonts/noto-emoji/tree/${notoRevision}/2D/png/128), revision \`${notoRevision}\`, original 128-pixel PNG files. See [NOTO-LICENSE](NOTO-LICENSE) and [NOTO-FLAGS-NOTICE](NOTO-FLAGS-NOTICE).`,
  `- Classic Material Icons filled: [google/material-design-icons](https://github.com/google/material-design-icons/tree/${materialRevision}/src), revision \`${materialRevision}\`, original \`src/*/*/materialicons/24px.svg\` files. The build renders transparent 256-pixel PNG images with black or white glyphs and an 8-pixel outline in the opposite color. See [MATERIAL-LICENSE](MATERIAL-LICENSE).`,
  `- Emoji names: [Unicode Emoji 18.0 emoji-test.txt](https://www.unicode.org/Public/18.0.0/emoji/emoji-test.txt), SHA-256 \`${emojiTestSha256}\`. See [UNICODE-LICENSE](UNICODE-LICENSE).`,
  '- Japanese source documentation: [接続アイコンの素材とライセンス](../../docs/ja/connection-icon-artwork.md).',
  '- Glyphs Poly: [gorango/glyphs](https://github.com/gorango/glyphs/tree/771993addf00d5a7080fadb763f0da72ddc0f716/packages/svg/core), pinned by the `deps/glyphs` submodule. The build renders its 860 Poly variants as transparent 256-pixel PNG images with their original colors and padding. See the [MIT license](https://github.com/gorango/glyphs/blob/771993addf00d5a7080fadb763f0da72ddc0f716/license).',
  '',
].join('\n'));
console.log(`Prepared ${entries.length} presets (${entries.length - materialSources.length} Noto, ${materialSources.length} Material)`);
