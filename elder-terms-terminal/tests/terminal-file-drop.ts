// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { promisify } from 'node:util';
import type { GtkApp } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect } from 'vitest';

/** Drags local file URIs from an independent GTK window and waits for the terminal's send menu. */
export const dropTerminalFiles = async (
  app: GtkApp,
  uris: readonly string[]
): Promise<void> => {
  const env = await app.environment();
  const source = spawn(
    'gjs',
    [
      '-c',
      `
          imports.gi.versions.Gtk = '3.0';
          const {Gtk, Gdk} = imports.gi;
          Gtk.init(null);
          const w = new Gtk.Window({type:Gtk.WindowType.POPUP, title:'URI drop test', default_width:110, default_height:65});
          const box = new Gtk.EventBox();
          box.add(new Gtk.Label({label:'Drag file'}));
          box.drag_source_set(Gdk.ModifierType.BUTTON1_MASK, [], Gdk.DragAction.COPY);
          box.drag_source_add_uri_targets();
          box.connect('drag-begin', (_box, context) => {
            print('begin');
            context.connect('action-changed', (_ctx, action) => print('action ' + action));
          });
          box.connect('drag-data-get', (_box, _context, data) => data.set_uris(ARGV));
          w.add(box); w.move(5, 5); w.show_all(); Gtk.main();
        `,
      ...uris,
    ],
    { env }
  );
  let signals = '';
  source.stdout.on('data', (bytes) => {
    signals += bytes.toString();
  });
  source.stderr.on('data', (bytes) => {
    signals += bytes.toString();
  });
  try {
    await waitForResult(async () => {
      await promisify(execFile)('xwininfo', ['-name', 'URI drop test'], {
        env,
      });
    });
    const terminal = (await (await app.getById('terminal_view')).capture())
      .bounds;
    await app.input.moveMouseTo(40, 30);
    await app.input.setMouseButton('left', true);
    try {
      await app.input.moveMouseTo(65, 32);
      await waitForResult(async () => expect(signals).toContain('begin'));
      await app.input.moveMouseTo(
        terminal.x + terminal.width / 2,
        terminal.y + terminal.height / 2
      );
      await waitForResult(async () => expect(signals).toContain('action 2'));
    } finally {
      await app.input.setMouseButton('left', false);
    }
    await app.getById(
      uris.length === 1 ? 'drop_text_send_item' : 'drop_ymodem_send_item'
    );
  } finally {
    if (source.exitCode === null && source.signalCode === null) {
      const closed = once(source, 'close');
      source.kill();
      await closed;
    }
  }
};
