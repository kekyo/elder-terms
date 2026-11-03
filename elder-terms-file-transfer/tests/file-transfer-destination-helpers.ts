// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import type { GtkApp, GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect } from 'vitest';
import { expectElementKind } from '../../shared/tests/connection-test-helpers.ts';

const directoryRow = async (
  tree: GtkTableElement,
  path: string
): Promise<number> =>
  await waitForResult(async () => {
    expect((await tree.info()).states).toContain('sensitive');
    const name = path.slice(path.lastIndexOf('/') + 1);
    for (let row = 0; row < (await tree.getRowCount()); row++) {
      const shown = (await (await tree.cellAt(row, 0))?.info())?.name;
      if (shown === name || shown === '/' + name) return row;
    }
    throw new Error(`Missing destination directory: ${path}`);
  });

/**
 * Selects the current directory from its parent, as a transfer destination.
 * @param app File transfer application.
 * @param remote Whether to operate on the remote pane.
 * @returns Original directory path, for reopening after the transfer.
 * @remarks The directory must have a browsable parent; a virtual root is not a destination.
 */
export const selectCurrentTransferDirectory = async (
  app: GtkApp,
  remote: boolean
): Promise<string> => {
  const side = remote ? 'remote' : 'local';
  const entry = expectElementKind(
    await app.getById(`file_transfer_${side}_path_entry`),
    'entry'
  );
  const path = await entry.text();
  expect(path).not.toBe('/');
  const tree = expectElementKind(
    await app.getById(`file_transfer_${side}_tree`),
    'table'
  );
  await waitForResult(async () =>
    expect((await tree.info()).states).toContain('sensitive')
  );
  await expectElementKind(
    await app.getById(`file_transfer_${side}_up_button`),
    'button'
  ).click();
  // Up changes the entry only after its new listing is installed.
  await waitForResult(async () => expect(await entry.text()).not.toBe(path));
  const row = await directoryRow(tree, path);
  for (const selected of await tree.selectedRows())
    await tree.deselectRow(selected);
  await tree.selectRow(row);
  return path;
};

/**
 * Opens a directory currently listed in its parent pane.
 * @param app File transfer application.
 * @param remote Whether to operate on the remote pane.
 * @param path Display path returned by selectCurrentTransferDirectory.
 * @returns Completion after the directory is open and its tree is available.
 */
export const openTransferDirectory = async (
  app: GtkApp,
  remote: boolean,
  path: string
): Promise<void> => {
  const side = remote ? 'remote' : 'local';
  const tree = expectElementKind(
    await app.getById(`file_transfer_${side}_tree`),
    'table'
  );
  const row = await directoryRow(tree, path);
  for (const selected of await tree.selectedRows())
    await tree.deselectRow(selected);
  await tree.selectRow(row);
  const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
  await app.input.moveMouseTo(
    Math.round(bounds.x + bounds.width / 2),
    Math.round(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
  await waitForResult(async () =>
    expect((await tree.info()).states).toContain('focused')
  );
  await app.input.pressKey('Return');
  await waitForResult(async () => {
    expect(
      await expectElementKind(
        await app.getById(`file_transfer_${side}_path_entry`),
        'entry'
      ).text()
    ).toBe(path);
    expect((await tree.info()).states).toContain('sensitive');
  });
};
