// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

import { readFile, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';

interface Row {
  [name: string]: string;
}
const directory = resolve(process.argv[2]);
const readRows = async (file: string): Promise<Row[]> => {
  const [header, ...lines] = (await readFile(join(directory, file), 'utf8'))
    .trim()
    .split('\n');
  const columns = header.split('\t');
  return lines.map((line) =>
    Object.fromEntries(
      line.split('\t').map((cell, index) => [columns[index], cell])
    )
  );
};
const events = await readRows('events.tsv');
const peers = await readRows('peer.tsv');
const configuration = JSON.parse(
  await readFile(join(directory, 'configuration.json'), 'utf8')
);
const trace =
  configuration.tracing === 'off' ? [] : await readRows('trace.tsv');
if (configuration.tracing !== 'off' && trace.length === 0)
  throw new Error('Empty trace');
if (trace.some((row) => row.kind === 'dropped'))
  throw new Error('Trace buffer overflow');
if (
  events.length !== Number(configuration.count) ||
  peers.length !== events.length
)
  throw new Error('Input or peer responses are missing');
const milliseconds = (end: string, start: string): number =>
  Number(BigInt(end) - BigInt(start)) / 1e6;
const samples = events.map((event, index) => {
  const peer = peers[index];
  if (
    event.key !== peer.key ||
    BigInt(peer.received_ns) < BigInt(event.sent_ns) ||
    BigInt(peer.written_ns) < BigInt(peer.received_ns) ||
    BigInt(event.visible_ns) < BigInt(peer.received_ns)
  )
    throw new Error(`Invalid response order ${event.key}`);
  const find = (kind: string): Row | undefined =>
    trace.find((row) => row.kind === kind && row.sequence === event.key);
  const gtk = find('gtk_key');
  const received = find('received');
  const parsed = find('parsed');
  const draw =
    parsed &&
    trace.find(
      (row) =>
        row.kind === 'draw' &&
        row.sequence === event.key &&
        BigInt(row.start_ns) >= BigInt(parsed.end_ns) &&
        BigInt(row.revision) >= BigInt(parsed.revision)
    );
  const composite =
    draw &&
    trace.find(
      (row) =>
        row.kind === 'composite' &&
        row.sequence === event.key &&
        row.value === draw.value &&
        row.value !== '0' &&
        BigInt(row.start_ns) >= BigInt(draw.end_ns)
    );
  const frame =
    parsed &&
    draw &&
    trace.find(
      (row) =>
        row.kind === 'frame' &&
        row.sequence === event.key &&
        BigInt(row.start_ns) >= BigInt(parsed.end_ns) &&
        BigInt(row.end_ns) <= BigInt(draw.start_ns)
    );
  if (
    configuration.tracing !== 'off' &&
    (!gtk || !received || !parsed || !draw || !composite)
  )
    throw new Error(
      `Missing phase for key ${event.key}: ${JSON.stringify({ gtk: !!gtk, received: !!received, parsed: !!parsed, draw: !!draw, composite: !!composite })}`
    );
  if (gtk && received && parsed && draw && composite) {
    const ordered = [
      event.sent_ns,
      gtk.start_ns,
      peer.received_ns,
      received.end_ns,
      parsed.end_ns,
      draw.start_ns,
      draw.end_ns,
      composite.start_ns,
      event.visible_ns,
    ];
    if (
      BigInt(composite.end_ns) < BigInt(composite.start_ns) ||
      ordered.some(
        (value, index) =>
          index > 0 && BigInt(value) < BigInt(ordered[index - 1])
      )
    )
      throw new Error(`Invalid phase order for key ${event.key}`);
    if (configuration.tracing === 'detail' && !frame)
      throw new Error(`Missing frame for key ${event.key}`);
  }
  return {
    key: Number(event.key),
    totalMs: milliseconds(event.visible_ns, event.sent_ns),
    toPeerMs: milliseconds(peer.received_ns, event.sent_ns),
    gtkMs: gtk ? milliseconds(gtk.start_ns, event.sent_ns) : null,
    gtkToPeerMs: gtk ? milliseconds(peer.received_ns, gtk.start_ns) : null,
    peerToReceiveMs: received
      ? milliseconds(received.end_ns, peer.received_ns)
      : null,
    receiveToParseMs:
      received && parsed ? milliseconds(parsed.end_ns, received.end_ns) : null,
    parseToDrawMs:
      parsed && draw ? milliseconds(draw.start_ns, parsed.end_ns) : null,
    gtkDrawSignalMs: draw ? milliseconds(draw.end_ns, draw.start_ns) : null,
    frameMs: frame ? milliseconds(frame.end_ns, frame.start_ns) : null,
    toCompositeMs: composite
      ? milliseconds(composite.end_ns, event.sent_ns)
      : null,
    history: parsed ? Number(parsed.history) : null,
    outputBytes: Number(peer.output_bytes),
    outputLines: Number(peer.output_lines),
    updates: Number(peer.output_updates),
  };
});
const distribution = (values: number[]) => {
  const sorted = [...values].sort((a, b) => a - b);
  return {
    median:
      (sorted[Math.floor((sorted.length - 1) / 2)] +
        sorted[Math.floor(sorted.length / 2)]) /
      2,
    p95: sorted[Math.ceil(sorted.length * 0.95) - 1],
    max: sorted[sorted.length - 1],
  };
};
const functions: Record<
  string,
  { calls: number; wallMs: number; cpuMs: number; maxMs: number }
> = {};
for (const row of trace) {
  const item = (functions[row.kind] ??= {
    calls: 0,
    wallMs: 0,
    cpuMs: 0,
    maxMs: 0,
  });
  const elapsed = milliseconds(row.end_ns, row.start_ns);
  item.calls++;
  item.wallMs += elapsed;
  item.cpuMs += Number(row.cpu_ns) / 1e6;
  item.maxMs = Math.max(item.maxMs, elapsed);
}
const summary = {
  configuration,
  count: samples.length,
  totalMs: distribution(samples.map((row) => row.totalMs)),
  toPeerMs: distribution(samples.map((row) => row.toPeerMs)),
  samples,
  functions,
  activeBackends: [
    ...new Set(
      trace
        .filter((row) => row.kind === 'backend')
        .map((row) => Number(row.value))
    ),
  ],
  note: 'Function times overlap; do not sum them. gtkDrawSignalMs is the GTK draw signal, not all rendering work. Cairo renders in frame; GL renders asynchronously. visible_ns is sampled window pixels and can precede the end of the composite function, not physical display time.',
};
await writeFile(
  join(directory, 'summary.json'),
  JSON.stringify(summary, null, 2)
);
console.log(
  JSON.stringify({
    count: summary.count,
    totalMs: summary.totalMs,
    toPeerMs: summary.toPeerMs,
    histories: [...new Set(samples.map((row) => row.history))],
  })
);
