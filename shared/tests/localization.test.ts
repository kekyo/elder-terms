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
  {
    language: 'ar',
    settings: 'الإعدادات',
    links: 'الروابط',
    hashMessages: [
      'جارٍ حساب قيم التجزئة…',
      'أُلغي حساب قيم التجزئة',
      'فشل حساب قيم التجزئة',
      'تعذر حساب قيم التجزئة',
      'اكتمل حساب قيم التجزئة',
      'قيم تجزئة الملف',
      'حساب قيم التجزئة',
    ],
  },
  {
    language: 'es',
    settings: 'Configuración',
    links: 'Enlaces',
    hashMessages: [
      'Calculando valores hash…',
      'Cálculo de hash cancelado',
      'Error al calcular el hash',
      'No se pudieron calcular los valores hash',
      'Cálculo de hash completado',
      'Valores hash del archivo',
      'Calcular valores hash',
    ],
  },
  {
    language: 'fr',
    settings: 'Paramètres',
    links: 'Liens',
    hashMessages: [
      'Calcul des valeurs de hachage…',
      'Calcul des valeurs de hachage annulé',
      'Échec du calcul des valeurs de hachage',
      'Impossible de calculer les valeurs de hachage',
      'Calcul des valeurs de hachage terminé',
      'Valeurs de hachage du fichier',
      'Calculer les valeurs de hachage',
    ],
  },
  {
    language: 'hi',
    settings: 'सेटिंग्स',
    links: 'लिंक',
    hashMessages: [
      'हैश मानों की गणना की जा रही है…',
      'हैश मान गणना रद्द की गई',
      'हैश मान गणना विफल रही',
      'हैश मानों की गणना नहीं हो सकी',
      'हैश मान गणना पूरी हुई',
      'फ़ाइल हैश मान',
      'हैश मानों की गणना करें',
    ],
  },
  {
    language: 'ja',
    settings: '設定',
    links: 'リンク',
    hashMessages: [
      'ハッシュ値を計算中…',
      'ハッシュ値の計算をキャンセルしました',
      'ハッシュ値の計算に失敗しました',
      'ハッシュ値を計算できませんでした',
      'ハッシュ値の計算が完了しました',
      'ファイルのハッシュ値',
      'ハッシュ値を計算',
    ],
  },
  {
    language: 'ko',
    settings: '설정',
    links: '링크',
    hashMessages: [
      '해시 값을 계산하는 중…',
      '해시 값 계산이 취소되었습니다',
      '해시 값 계산에 실패했습니다',
      '해시 값을 계산하지 못했습니다',
      '해시 값 계산이 완료되었습니다',
      '파일 해시 값',
      '해시 값 계산',
    ],
  },
  {
    language: 'pt',
    settings: 'Configurações',
    links: 'Links',
    hashMessages: [
      'Calculando valores de hash…',
      'Cálculo de hash cancelado',
      'Falha no cálculo de hash',
      'Não foi possível calcular os valores de hash',
      'Cálculo de hash concluído',
      'Valores de hash do arquivo',
      'Calcular valores de hash',
    ],
  },
  {
    language: 'ru',
    settings: 'Настройки',
    links: 'Ссылки',
    hashMessages: [
      'Вычисление хеш-значений…',
      'Вычисление хеш-значений отменено',
      'Не удалось вычислить хеш-значения',
      'Не удалось вычислить хеш-значения',
      'Вычисление хеш-значений завершено',
      'Хеш-значения файла',
      'Вычислить хеш-значения',
    ],
  },
  {
    language: 'zh',
    settings: '设置',
    links: '链接',
    hashMessages: [
      '正在计算哈希值…',
      '已取消哈希值计算',
      '哈希值计算失败',
      '无法计算哈希值',
      '哈希值计算完成',
      '文件哈希值',
      '计算哈希值',
    ],
  },
] as const;

describe('localization message catalogs', () => {
  it.each(translations)(
    'loads the $language catalog through the configured UI language',
    async ({ language, settings, links, hashMessages }) => {
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

        expect(stdout.trim()).toBe(
          [settings, links, ...hashMessages].join('\n')
        );
      } finally {
        await rm(directory, { recursive: true, force: true });
      }
    }
  );
});
