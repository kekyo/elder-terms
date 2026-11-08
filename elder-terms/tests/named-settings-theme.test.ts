// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import type { GtkWidgetElement } from 'gestament';
import { runLauncherGtkTest } from './test-helpers.ts';
import { expectElementKind } from '../../shared/tests/connection-test-helpers.ts';

const namedChild = async (
  root: GtkWidgetElement,
  name: string
): Promise<GtkWidgetElement | undefined> => {
  if ((await root.info()).name === name) return root;
  if ('getChildCount' in root && 'childAt' in root) {
    for (let index = 0; index < (await root.getChildCount()); index++) {
      const child = await root.childAt(index);
      if (child) {
        const found = await namedChild(child, name);
        if (found) return found;
      }
    }
  }
  return undefined;
};

it('edits defaults and named themes in shared application tabs and saves only the changed file', async (context) => {
  let globalPath = '';
  let themePath = '';
  await runLauncherGtkTest(
    context,
    async (connections) => {
      globalPath = join(connections, '..', 'global.ini');
      themePath = join(connections, '..', 'theme.ini');
      await writeFile(globalPath, '[general]\nbackground=#111111\n');
    },
    async ({ app }) => {
      await expectElementKind(
        await app.getById('application_menu_button'),
        'toggleButton'
      ).click();
      await expectElementKind(
        await app.getById('application_settings_menu_item'),
        'menuItem'
      ).click();
      const notebook = expectElementKind(
        await app.getById('application_dialog_notebook'),
        'tabList'
      );
      expect(await notebook.getChildCount()).toBe(4);
      await notebook.selectChildAt(2);
      await expectElementKind(
        await app.getById('themes_new_button'),
        'button'
      ).click();
      await expectElementKind(
        await app.getById('themes_name_entry'),
        'entry'
      ).setText('Red theme');
      for (const field of ['exterior_background', 'background']) {
        const id = `theme_0_general_${field}_button`;
        await expectElementKind(await app.getById(id), 'button').click();
        const chooser = await app.getById(`${id}_dialog`);
        await expectElementKind(
          await namedChild(chooser, 'Red'),
          'radio'
        ).click();
        await expectElementKind(
          await namedChild(chooser, 'Select'),
          'button'
        ).click();
      }
      expect(
        (
          await (
            await app.getById('theme_0_general_background_mode_combo')
          ).info()
        ).states
      ).toContain('showing');
      await notebook.selectChildAt(3);
      await notebook.selectChildAt(2);
      expect(
        await expectElementKind(
          await app.getById('themes_name_entry'),
          'entry'
        ).text()
      ).toBe('Red theme');
      await mkdir('../test-results/plan23', { recursive: true });
      await writeFile(
        '../test-results/plan23/named-theme-editor.png',
        (await (await app.getById('application_dialog')).capture()).image
      );
      await expectElementKind(
        await app.getById('application_dialog_save_button'),
        'button'
      ).click();
      await waitForResult(async () =>
        expect(await readFile(themePath, 'utf8')).toContain('name=Red theme')
      );
      expect(await readFile(globalPath, 'utf8')).toBe(
        '[general]\nbackground=#111111\n'
      );
      const themeContents = await readFile(themePath, 'utf8');
      expect(themeContents).toMatch(/exterior_background=#[0-9A-F]{6}/u);
      expect(themeContents).toMatch(/\nbackground=#[0-9A-F]{6}/u);
      expect(themeContents).not.toContain('font_size_dip');
      await expectElementKind(
        await app.getById('application_menu_button'),
        'toggleButton'
      ).click();
      await expectElementKind(
        await app.getById('application_settings_menu_item'),
        'menuItem'
      ).click();
      await expectElementKind(
        await app.getById('application_dialog_notebook'),
        'tabList'
      ).selectChildAt(1);
      await expectElementKind(
        await app.getById('global_settings_terminal_font_size_dip_entry'),
        'entry'
      ).setText('15');
      await expectElementKind(
        await app.getById('application_dialog_save_button'),
        'button'
      ).click();
      await waitForResult(async () =>
        expect(await readFile(globalPath, 'utf8')).toContain('font_size_dip=15')
      );
      expect(await readFile(themePath, 'utf8')).toBe(themeContents);
    }
  );
});
