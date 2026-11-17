// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { runScriptOnceToText } from 'funcity';

const [sourceRoot, outputSource, renderedRoot, glibCommand, thumbnailCommand] = process.argv.slice(2);
if (!sourceRoot || !outputSource || !renderedRoot || !glibCommand || !thumbnailCommand)
  throw new Error('Usage: render-glyphs-connection-icons.mts SOURCE_ROOT OUTPUT_C RENDERED_ROOT GLIB_COMPILE_RESOURCES GDK_PIXBUF_THUMBNAILER');

interface GlyphComponent {
  name: string;
  variants: { poly: string };
}

const components: GlyphComponent[] = JSON.parse(await readFile(join(sourceRoot, 'components.json'), 'utf8'));
const artwork: Record<string, string> = JSON.parse(await readFile(join(sourceRoot, 'map.json'), 'utf8'));
const names = new Set<string>();
const entries = components.map((component) => {
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_-]*$/.test(component.name) || names.has(component.name))
    throw new Error(`Invalid or duplicate Glyphs icon name: ${component.name}`);
  names.add(component.name);
  const svg = artwork[component.variants.poly];
  if (!svg) throw new Error(`Poly artwork is unavailable for ${component.name}`);
  return {
    id: component.name,
    name: component.name.replace(/([a-z0-9])([A-Z])/g, '$1 $2').replace(/[_-]/g, ' ').toLowerCase(),
    svg,
  };
}).sort((left, right) => left.name.localeCompare(right.name) || left.id.localeCompare(right.id));

const renderText = async (template: string, values: Record<string, string>): Promise<string> => {
  const result = await runScriptOnceToText(template, {
    variables: new Map(Object.entries(values)),
    sourceId: 'render-glyphs-connection-icons',
  });
  if (result === undefined) throw new Error('Unable to render Glyphs resource metadata');
  return result;
};

const run = async (command: string, args: string[]): Promise<void> => {
  const child = spawn(command, args, { stdio: ['ignore', 'ignore', 'pipe'] });
  let stderr = '';
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (chunk: string) => { stderr += chunk; });
  const code = await new Promise<number | null>((resolve, reject) => {
    child.once('error', reject);
    child.once('close', resolve);
  });
  if (code !== 0) throw new Error(`${command} exited with ${code}: ${stderr}`);
};

const outputDirectory = join(renderedRoot, 'glyphs-poly');
await mkdir(outputDirectory, { recursive: true });
let next = 0;
await Promise.all(Array.from({ length: 12 }, async () => {
  while (next < entries.length) {
    const entry = entries[next++];
    const svg = join(outputDirectory, `${entry.id}.svg`);
    // The submodule stays untouched; render the upstream SVG with its own colors
    // and padding, without the monochrome treatment used for Material icons.
    await writeFile(svg, entry.svg);
    try {
      await run(thumbnailCommand, ['-s', '256', svg, join(outputDirectory, `${entry.id}.png`)]);
    } finally {
      await rm(svg, { force: true });
    }
  }
}));
await writeFile(join(outputDirectory, 'catalog.tsv'), (await Promise.all(entries.map(entry =>
  renderText('glyphs-poly:{{id}}\t{{name}}\tglyphs-poly\n', entry)))).join(''));
const files = ['glyphs-poly/catalog.tsv', ...entries.map(entry => `glyphs-poly/${entry.id}.png`)];
const resourceXml = join(renderedRoot, 'glyphs-presets.xml');
await writeFile(resourceXml, await renderText(
  '<?xml version="1.0" encoding="UTF-8"?>\n<gresources>\n  <gresource prefix="/net/kekyo/elder-terms/presets">\n{{files}}\n  </gresource>\n</gresources>\n',
  { files: (await Promise.all(files.map(file => renderText('    <file>{{file}}</file>', { file })))).join('\n') },
));
await run(glibCommand, ['--generate-source', '--c-name', 'elder_terms_glyphs_presets',
  '--sourcedir', renderedRoot, '--target', outputSource, resourceXml]);
