import { prepareTestFonts, type TestFontSource } from './test-fonts.mts';
import {
  getTestFontEnvironment,
  testFontDirectory,
} from './test-font-environment.mts';

// Package versions and the Noto CJK revision are pinned together with SHA-256.
// Debian packages provide the original copyright files without installing them.
const sources: readonly TestFontSource[] = [
  {
    family: 'Noto Sans Mono',
    format: 'deb',
    download: {
      url: 'https://deb.debian.org/debian/pool/main/f/fonts-noto/fonts-noto-mono_20201225-2_all.deb',
      sha256:
        '7636f002caeb4a4082f69e2d9b8eb89da6c50b9c67f55db84e68638049b06006',
    },
    fontPath: 'usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf',
    licensePath: 'usr/share/doc/fonts-noto-mono/copyright',
  },
  {
    family: 'DejaVu Sans Mono',
    format: 'deb',
    download: {
      url: 'https://deb.debian.org/debian/pool/main/f/fonts-dejavu/fonts-dejavu-mono_2.37-8_all.deb',
      sha256:
        '3003e98a5debfdeadc7040a7f715fe9fe6fb67f68deacf6049b54e30f07fc014',
    },
    fontPath: 'usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf',
    licensePath: 'usr/share/doc/fonts-dejavu-mono/copyright',
  },
  {
    family: 'IPAGothic',
    format: 'deb',
    download: {
      url: 'https://deb.debian.org/debian/pool/main/f/fonts-ipafont/fonts-ipafont-gothic_00303-23_all.deb',
      sha256:
        '5f8e0270654fec6577fffa96015067352440065c0b49bbc8b076745a6b4bf3d3',
    },
    fontPath: 'usr/share/fonts/opentype/ipafont-gothic/ipag.ttf',
    licensePath: 'usr/share/doc/fonts-ipafont-gothic/copyright',
  },
  {
    family: 'Noto Sans CJK JP',
    format: 'font',
    filename: 'NotoSansCJKjp-Regular.otf',
    download: {
      url: 'https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf',
      sha256:
        '68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5',
    },
    license: {
      url: 'https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/LICENSE',
      sha256:
        '6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2',
    },
  },
];

const environment = getTestFontEnvironment();
await prepareTestFonts({
  directory: testFontDirectory,
  baseConfig: environment.ELDER_TERMS_TEST_FONTCONFIG_BASE,
  sources,
});
