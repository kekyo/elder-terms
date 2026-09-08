import assert from 'node:assert/strict';
import { chmod, mkdir, readFile, symlink, writeFile } from 'node:fs/promises';
import { createGtkAppLauncher, type GtkApp } from 'gestament';
import { waitForResult } from 'gestament/testing';

/**
 * Edits a scanned connection through the isolated XDG editor and launcher UI.
 * @param source Saved scan result inside the test container.
 * @returns Saved connection path for subsequent real terminal launches.
 */
export const editScannedConnection = async (
  source: string
): Promise<string> => {
  const configHome = '/tmp/scan-config';
  const dataHome = '/tmp/scan-editor-data';
  const directory = `${configHome}/elder-terms/connections`;
  const path = `${directory}/Scanned.ini`;
  await mkdir(directory, { recursive: true });
  await mkdir(`${dataHome}/applications`, { recursive: true });
  await symlink('/usr/share/mime', `${dataHome}/mime`);
  await writeFile(path, await readFile(source));
  const editor = `${dataHome}/editor.mjs`;
  await writeFile(
    editor,
    `#!/test-node
import { readFile, writeFile } from 'node:fs/promises';
const path = process.argv[2];
const content = await readFile(path, 'utf8');
await writeFile(path, content.replace(/^name=fixture$/m, 'name=Externally edited scan'));
await writeFile('/evidence/editor-path.txt', path);
`
  );
  await chmod(editor, 0o755);
  await writeFile(
    `${dataHome}/applications/scan-editor.desktop`,
    `[Desktop Entry]\nType=Application\nName=Scan editor\nExec=${editor} %f\nMimeType=text/plain;\nTerminal=false\n`
  );
  await writeFile(
    `${configHome}/mimeapps.list`,
    '[Default Applications]\ntext/plain=scan-editor.desktop;\n'
  );
  const launcher = createGtkAppLauncher({
    appPath: '/workspace/.build/elder-terms/elder-terms',
    env: {
      LANGUAGE: 'C',
      LC_ALL: 'C.UTF-8',
      XDG_CONFIG_HOME: configHome,
      XDG_DATA_HOME: dataHome,
      XDG_DATA_DIRS: dataHome,
      XDG_CONFIG_DIRS: dataHome,
    },
    xvfbTrayHost: true,
  });
  let app: GtkApp | undefined;
  try {
    app = await launcher.launch([]);
    const list = await app.getById('connection_list');
    assert.equal(list.kind, 'table');
    const bounds = (await (await list.cellAt(0, 0))?.capture())?.bounds;
    assert(bounds);
    await app.input.moveMouseTo(
      Math.round(bounds.x + bounds.width / 2),
      Math.round(bounds.y + bounds.height / 2)
    );
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    await app.input.setMouseButton('right', true);
    await app.input.setMouseButton('right', false);
    const edit = await app.getById('edit_connection_menu_item');
    assert.equal(edit.kind, 'menuItem');
    await edit.click();
    const name = await app.getById('settings_general_name_entry');
    assert.equal(name.kind, 'entry');
    await waitForResult(async () => {
      assert.equal(await readFile('/evidence/editor-path.txt', 'utf8'), path);
      assert.equal(await name.text(), 'Externally edited scan');
    });
    const notebook = await app.getById('settings_notebook');
    assert.equal(notebook.kind, 'tabList');
    for (let index = 0; index < (await notebook.getChildCount()); index++) {
      if (
        (await (await notebook.childAt(index))?.info())?.name === 'Terminal'
      ) {
        await notebook.selectChildAt(index);
        break;
      }
    }
    const autoClose = await app.getById('settings_terminal_auto_close_combo');
    assert.equal(autoClose.kind, 'comboBox');
    await autoClose.selectChildAt(2);
    const scrollbar = await app.getById('settings_terminal_page_scrollbar');
    assert.equal(scrollbar.kind, 'scrollbar');
    await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
    const mode = await app.getById('settings_terminal_fonts_mode_combo');
    assert.equal(mode.kind, 'comboBox');
    await mode.selectChildAt(2);
    const add = await app.getById('settings_terminal_fonts_add_button');
    assert.equal(add.kind, 'button');
    await add.click();
    const font = await app.getById('settings_terminal_font_family_2');
    assert.equal(font.kind, 'entry');
    await font.setText('IPAGothic');
    const apply = await app.getById('apply_button');
    assert.equal(apply.kind, 'button');
    await apply.click();
    await waitForResult(async () => {
      const saved = await readFile(path, 'utf8');
      assert(saved.includes('name=Externally edited scan'));
      assert(saved.includes('auto_close=false'));
      assert(
        saved.includes('font_families=Noto Sans Mono;Monospace;IPAGothic;')
      );
    });
    await writeFile(
      '/evidence/combined-settings.png',
      (await (await app.getById('settings_terminal_page')).capture()).image
    );
  } finally {
    if (app !== undefined)
      await writeFile(
        '/evidence/combined-launcher-output.json',
        JSON.stringify(await app.output(), null, 2)
      );
    await launcher.release();
  }
  return path;
};
