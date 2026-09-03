import { execFile } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { describe, expect, it } from 'vitest';

const execFileAsync = promisify(execFile);

const localizationFixturePath = fileURLToPath(
  new URL(
    '../../.build/shared/elder-terms-localization-fixture',
    import.meta.url
  )
);

const translations = [
  { language: 'ar', settings: 'الإعدادات', links: 'الروابط' },
  { language: 'es', settings: 'Configuración', links: 'Enlaces' },
  { language: 'fr', settings: 'Paramètres', links: 'Liens' },
  { language: 'hi', settings: 'सेटिंग्स', links: 'लिंक' },
  { language: 'ja', settings: '設定', links: 'リンク' },
  { language: 'ko', settings: '설정', links: '링크' },
  { language: 'pt', settings: 'Configurações', links: 'Links' },
  { language: 'ru', settings: 'Настройки', links: 'Ссылки' },
  { language: 'zh', settings: '设置', links: '链接' },
] as const;

describe('localization message catalogs', () => {
  it.each(translations)(
    'loads the $language catalog through the configured UI language',
    async ({ language, settings, links }) => {
      const directory = await mkdtemp(
        join(tmpdir(), 'elder-terms-localization-')
      );
      const configPath = join(directory, 'global.ini');
      try {
        await writeFile(
          configPath,
          `[general]\nui_language=${language}\n`,
          'utf8'
        );
        const { stdout } = await execFileAsync(
          localizationFixturePath,
          [configPath],
          {
            env: {
              ...process.env,
              LANGUAGE: 'C',
              LC_ALL: 'C.UTF-8',
            },
          }
        );

        expect(stdout.trim()).toBe(`${settings}\n${links}`);
      } finally {
        await rm(directory, { recursive: true, force: true });
      }
    }
  );
});
