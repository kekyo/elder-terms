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
import type { GtkElementOfKind } from 'gestament';
import { expectElementKind, runSharedGtkTest } from './test-helpers.ts';

it('types an icon name on opening the picker and saves the filtered result', async (context) => {
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
        await expectElementKind(
          await app.getById('settings_general_icon_choose'),
          'button'
        ).click();
        const search = expectElementKind(
          await app.getById('settings_general_icon_search'),
          'entry'
        );
        const grid = expectElementKind(
          await app.getById('settings_general_icon_grid'),
          'container'
        );
        const total = await grid.getChildCount();
        for (const key of 'folder') await app.input.pressKey(key);
        await waitForResult(async () => {
          expect(await search.text()).toBe('folder');
          expect(await grid.getChildCount()).toBeGreaterThan(1);
          expect(await grid.getChildCount()).toBeLessThan(total);
        });
        const popup = await app.getById('settings_general_icon_picker');
        await writeFile(
          join(directory, 'filtered-icons.png'),
          (await popup.capture()).image
        );
        await search.setText('FoLdEr_OpEn');
        await waitForResult(async () => {
          expect(await grid.getChildCount()).toBe(1);
          expect((await (await grid.childAt(0))!.info()).name).toBe(
            'folder open'
          );
        });
        await app.input.pressKey('Return');
        await waitForResult(async () => {
          expect((await popup.info()).states).not.toContain('showing');
          expect(
            await expectElementKind(
              await app.getById('settings_general_icon_selection'),
              'label'
            ).text()
          ).toBe('folder open (White)');
        });
        await expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        ).click();
        await waitForResult(async () =>
          expect(await readFile(configuration, 'utf8')).toContain(
            'icon=material:white:file/folder_open'
          )
        );
      }
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

it('scrolls through both preset families and saves a clicked icon', async (context) => {
  const temporary = await mkdtemp(join(tmpdir(), 'elder-icon-picker-'));
  const configuration = join(temporary, 'connection.ini');
  try {
    await runSharedGtkTest(
      context,
      [`--save-file=${configuration}`],
      async ({ app, directory }) => {
        const choose = expectElementKind(
          await app.getById('settings_general_icon_choose'),
          'button'
        );
        const selection = expectElementKind(
          await app.getById('settings_general_icon_selection'),
          'label'
        );
        await choose.click();
        const popup = await app.getById('settings_general_icon_picker');
        const family = expectElementKind(
          await app.getById('settings_general_icon_family'),
          'comboBox'
        );
        const grid = expectElementKind(
          await app.getById('settings_general_icon_grid'),
          'container'
        );
        const scroll = expectElementKind(
          await app.getById('settings_general_icon_scrollbar'),
          'scrollbar'
        );
        let white: GtkElementOfKind<'radio'> | undefined;
        for (const [index, name] of ['noto', 'material'].entries()) {
          if (index > 0) await choose.click();
          await family.selectChildAt(index);
          if (name === 'material') {
            const colorRow = await app.getById(
              'settings_general_icon_color_row'
            );
            expect((await colorRow.info()).states).toContain('showing');
            white = expectElementKind(
              await app.getById('settings_general_icon_color_white'),
              'radio'
            );
          }
          await waitForResult(async () => {
            const pixels = PNG.sync.read((await grid.capture()).image);
            let dark = 0;
            for (let offset = 0; offset < pixels.data.length; offset += 4)
              if (
                pixels.data[offset] < 100 &&
                pixels.data[offset + 1] < 100 &&
                pixels.data[offset + 2] < 100
              )
                dark++;
            expect(dark).toBeGreaterThan(400);
          });
          await writeFile(
            join(directory, `${name}-first.png`),
            (await popup.capture()).image
          );
          const count = await grid.getChildCount();
          expect(count).toBeGreaterThan(100);
          const last = await grid.childAt(count - 1);
          expect(last).toBeDefined();
          await scroll.setValue((await scroll.valueInfo()).maximum);
          await waitForResult(async () => {
            const info = await last!.info();
            expect(info.states).toContain('showing');
            expect(info.name.length).toBeGreaterThan(0);
          });
          await writeFile(
            join(directory, `${name}-last.png`),
            (await popup.capture()).image
          );
          const info = await last!.info();
          if (name === 'material') {
            const gridViewport = (await grid.capture()).visibleBounds;
            await app.input.moveMouseTo(
              gridViewport.x + 10,
              gridViewport.y - 55
            );
            const black = expectElementKind(
              await app.getById('settings_general_icon_color_black'),
              'radio'
            );
            expect(await black.isChecked()).toBe(true);
            const previous = await selection.text();
            const position = await scroll.value();
            const previousThumbnails = PNG.sync.read(
              (await grid.capture()).image
            );
            await white!.click();
            expect(await white!.isChecked()).toBe(true);
            expect(await scroll.value()).toBe(position);
            expect((await popup.info()).states).toContain('showing');
            expect(await selection.text()).toBe(previous);
            await waitForResult(async () => {
              const changedThumbnails = PNG.sync.read(
                (await grid.capture()).image
              );
              expect(changedThumbnails.width).toBe(previousThumbnails.width);
              let changed = 0;
              for (
                let offset = 0;
                offset < changedThumbnails.data.length;
                offset += 4
              )
                if (
                  changedThumbnails.data[offset] !==
                    previousThumbnails.data[offset] ||
                  changedThumbnails.data[offset + 1] !==
                    previousThumbnails.data[offset + 1] ||
                  changedThumbnails.data[offset + 2] !==
                    previousThumbnails.data[offset + 2]
                )
                  changed++;
              expect(changed).toBeGreaterThan(200);
            });
            await writeFile(
              join(directory, 'material-white-last.png'),
              (await popup.capture()).image
            );
          }
          const bounds = (await expectElementKind(last, 'image').imageInfo())
            .bounds;
          // GTK3's IconView item extents omit the vertical scroll offset.
          // Use the exposed adjustment to map its image into the viewport.
          const y = bounds.y - (await scroll.value()) + bounds.height / 2;
          const viewport = (await grid.capture()).visibleBounds;
          expect(y).toBeGreaterThanOrEqual(viewport.y);
          expect(y).toBeLessThan(viewport.y + viewport.height);
          await app.input.moveMouseTo(bounds.x + bounds.width / 2, y);
          await app.input.setMouseButton('left', true);
          await app.input.setMouseButton('left', false);
          await waitForResult(async () =>
            expect(await selection.text()).toBe(
              name === 'material' ? `${info.name} (White)` : info.name
            )
          );
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(await readFile(configuration, 'utf8')).toMatch(
              new RegExp(
                `icon=${name === 'material' ? 'material:white' : name}:`
              )
            )
          );
        }
        await choose.click();
        expect(await white!.isChecked()).toBe(true);
        const selected = await selection.text();
        await app.input.pressKey('Escape');
        await waitForResult(async () =>
          expect((await popup.info()).states).not.toContain('showing')
        );
        expect(await selection.text()).toBe(selected);
        await choose.click();
        await waitForResult(async () =>
          expect((await popup.info()).states).toContain('showing')
        );
        await family.selectChildAt(0);
        const gridBounds = (await grid.capture()).visibleBounds;
        await app.input.moveMouseTo(gridBounds.x + 1, gridBounds.y + 1);
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await app.input.pressKey('End');
        const last = await grid.childAt((await grid.getChildCount()) - 1);
        await waitForResult(async () => {
          expect((await last!.info()).states).toContain('selected');
          expect((await last!.info()).name.length).toBeGreaterThan(0);
        });
        const name = (await last!.info()).name;
        await app.input.pressKey('Return');
        await waitForResult(async () =>
          expect(await selection.text()).toBe(name)
        );
      }
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
