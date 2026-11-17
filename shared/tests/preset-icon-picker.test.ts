// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind, runSharedGtkTest } from './test-helpers.ts';

it('searches all groups and selects black and white icons independently', async (context) => {
  const temporary = await mkdtemp(join(tmpdir(), 'elder-icon-search-'));
  const configuration = join(temporary, 'connection.ini');
  try {
    await runSharedGtkTest(
      context,
      [
        `--save-file=${configuration}`,
        '--connection=general.icon=material:white:hardware/computer',
      ],
      async ({ app, directory }) => {
        const choose = expectElementKind(
          await app.getById('settings_general_icon_choose'),
          'button'
        );
        await choose.click();
        const search = expectElementKind(
          await app.getById('settings_general_icon_search'),
          'entry'
        );
        const emoji = expectElementKind(
          await app.getById('settings_general_icon_grid_emoji'),
          'container'
        );
        const black = expectElementKind(
          await app.getById('settings_general_icon_grid_material_black'),
          'container'
        );
        const white = expectElementKind(
          await app.getById('settings_general_icon_grid_material_white'),
          'container'
        );
        const popup = await app.getById('settings_general_icon_picker');
        for (const key of 'folder') await app.input.pressKey(key);
        await waitForResult(async () => {
          expect(await search.text()).toBe('folder');
          expect(await emoji.getChildCount()).toBe(2);
          expect(await black.getChildCount()).toBe(12);
          expect(await white.getChildCount()).toBe(12);
        });
        await writeFile(
          join(directory, 'filtered-icons.png'),
          (await popup.capture()).image
        );
        await search.setText('FoLdEr_OpEn');
        await waitForResult(async () => {
          expect(await black.getChildCount()).toBe(1);
          expect(await white.getChildCount()).toBe(1);
          expect((await (await black.childAt(0))!.info()).name).toBe(
            'folder open'
          );
        });
        const blackImage = expectElementKind(await black.childAt(0), 'image');
        const whiteImage = expectElementKind(await white.childAt(0), 'image');
        await waitForResult(async () => {
          const left = PNG.sync.read(
            (await (await blackImage.imageInfo()).capture()).image
          );
          const right = PNG.sync.read(
            (await (await whiteImage.imageInfo()).capture()).image
          );
          let changed = 0;
          for (let offset = 0; offset < left.data.length; offset += 4)
            if (Math.abs(left.data[offset] - right.data[offset]) > 100)
              ++changed;
          expect(changed).toBeGreaterThan(100);
        });
        await writeFile(
          join(directory, 'black-and-white-results.png'),
          (await popup.capture()).image
        );
        const bounds = (await whiteImage.imageInfo()).bounds;
        await app.input.moveMouseTo(
          bounds.x + bounds.width / 2,
          bounds.y + bounds.height / 2
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        const selection = expectElementKind(
          await app.getById('settings_general_icon_selection'),
          'label'
        );
        await waitForResult(async () =>
          expect(await selection.text()).toBe('folder open (White)')
        );
        await expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        ).click();
        await waitForResult(async () =>
          expect(await readFile(configuration, 'utf8')).toContain(
            'icon=material:white:file/folder_open'
          )
        );
        await choose.click();
        await search.setText('file/folder-open');
        await app.input.pressKey('Return');
        await waitForResult(async () =>
          expect(await selection.text()).toBe('folder open (Black)')
        );
        await expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        ).click();
        await waitForResult(async () =>
          expect(await readFile(configuration, 'utf8')).toContain(
            'icon=material:file/folder_open'
          )
        );
      }
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

it('navigates group boundaries and scrolls the continuous list to its last icon', async (context) => {
  await runSharedGtkTest(context, [], async ({ app, directory }) => {
    const choose = expectElementKind(
      await app.getById('settings_general_icon_choose'),
      'button'
    );
    await choose.click();
    const popup = await app.getById('settings_general_icon_picker');
    const emoji = expectElementKind(
      await app.getById('settings_general_icon_grid_emoji'),
      'container'
    );
    const black = expectElementKind(
      await app.getById('settings_general_icon_grid_material_black'),
      'container'
    );
    const white = expectElementKind(
      await app.getById('settings_general_icon_grid_material_white'),
      'container'
    );
    const scroll = expectElementKind(
      await app.getById('settings_general_icon_scrollbar'),
      'scrollbar'
    );
    await app.input.pressKey('Tab');
    await app.input.pressKey('End');
    const lastEmoji = (await emoji.childAt((await emoji.getChildCount()) - 1))!;
    await waitForResult(async () =>
      expect((await lastEmoji.info()).states).toContain('selected')
    );
    await app.input.pressKey('Down');
    await waitForResult(async () =>
      expect((await (await black.childAt(0))!.info()).states).toContain(
        'selected'
      )
    );
    await writeFile(
      join(directory, 'emoji-material-boundary.png'),
      (await popup.capture()).image
    );
    await app.input.pressKey('End');
    await app.input.pressKey('Down');
    await waitForResult(async () =>
      expect((await (await white.childAt(0))!.info()).states).toContain(
        'selected'
      )
    );
    await app.input.pressKey('Up');
    const lastBlack = (await black.childAt((await black.getChildCount()) - 1))!;
    await waitForResult(async () =>
      expect((await lastBlack.info()).states).toContain('selected')
    );
    await app.input.pressKey('Down');
    await app.input.pressKey('End');
    const last = expectElementKind(
      await white.childAt((await white.getChildCount()) - 1),
      'image'
    );
    await waitForResult(async () =>
      expect((await last.info()).states).toContain('selected')
    );
    const info = await last.info();
    await scroll.setValue(0);
    await writeFile(
      join(directory, 'emoji-first.png'),
      (await popup.capture()).image
    );
    await scroll.setValue((await scroll.valueInfo()).maximum);
    await writeFile(
      join(directory, 'material-white-last.png'),
      (await popup.capture()).image
    );
    // Activate the focused item without depending on GTK3's offscreen ATK extents.
    await app.input.pressKey('Return');
    const selection = expectElementKind(
      await app.getById('settings_general_icon_selection'),
      'label'
    );
    await waitForResult(async () =>
      expect(await selection.text()).toBe(`${info.name} (White)`)
    );
    await choose.click();
    await waitForResult(async () =>
      expect((await last.info()).states).toContain('selected')
    );
    expect(await scroll.value()).toBeGreaterThan(0);
    await app.input.pressKey('Escape');
    await waitForResult(async () =>
      expect((await popup.info()).states).not.toContain('showing')
    );
    expect(await selection.text()).toBe(`${info.name} (White)`);
  });
});
