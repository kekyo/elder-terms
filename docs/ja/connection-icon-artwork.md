# 接続アイコンの素材とライセンス

elder-termsには、次のプリセット画像と名前を同梱している。

| 素材 | 配布元と固定した版 | 同梱する形式 |
| --- | --- | --- |
| Noto Color Emoji | [googlefonts/noto-emoji](https://github.com/googlefonts/noto-emoji/tree/e20cbc2bbec1926686be9f9bee7d1d2cfa1fea0e/2D/png/128)、`e20cbc2bbec1926686be9f9bee7d1d2cfa1fea0e` | 公式の128ピクセルPNG画像 |
| Material Icons | [google/material-design-icons](https://github.com/google/material-design-icons/tree/49d4db35df873165d6bd6ba09b063c7dafbac2f4/src)、`49d4db35df873165d6bd6ba09b063c7dafbac2f4` | ClassicのFilled画像。公式SVGを原本として保持し、ビルド時に256ピクセルPNGを生成 |
| Glyphs Poly | [gorango/glyphs](https://github.com/gorango/glyphs/tree/771993addf00d5a7080fadb763f0da72ddc0f716/packages/svg/core)、`771993addf00d5a7080fadb763f0da72ddc0f716` | `deps/glyphs`のsubmoduleを参照。PolyのSVGから配色と透過を保った256ピクセルPNGを生成 |
| 絵文字の名前 | [Unicode Emoji 18.0のemoji-test.txt](https://www.unicode.org/Public/18.0.0/emoji/emoji-test.txt)、2026年4月30日版 | 対応する絵文字の英語名 |

Materialの配布画像は背景を透過にし、黒い図柄と白い縁取り、白い図柄と黒い縁取りの二種類を用意している。
縁取りの幅は256ピクセルの生成画像に対して8ピクセルで固定する。
黒と白は選択画面の別々のグループに並び、タイトルバー、接続一覧、トレイ、ドックへ同じ画像を渡す。

Glyphs Polyは採用版の一覧にある860個を収録する。
原本の配色と余白を保ち、縁取りは追加しない。
検索に使う名前は原本のコンポーネント名に基づく。

Materialのビルド時はSVG対応の`gdk-pixbuf-thumbnailer`で図柄を描き、ImageMagickで縁取りを生成する。
透明部分との境界を広げて対色で描き、その上に元の図柄を重ねる。
[ImageMagickの縁取り例](https://usage.imagemagick.org/masking/#outline_or_halo_transparency)を参照。
SVGの原本は変更せず、完成した透過PNGを同梱する。

画像と名前に適用されるライセンスは、素材とともに同梱する。
Noto画像は[NOTO-LICENSE](../../data/connection-icons/NOTO-LICENSE)、
旗の画像は[NOTO-FLAGS-NOTICE](../../data/connection-icons/NOTO-FLAGS-NOTICE)、
Material画像は[MATERIAL-LICENSE](../../data/connection-icons/MATERIAL-LICENSE)、
Glyphs Poly画像は[MITライセンス](https://github.com/gorango/glyphs/blob/771993addf00d5a7080fadb763f0da72ddc0f716/license)、
絵文字の名前は[UNICODE-LICENSE](../../data/connection-icons/UNICODE-LICENSE)を参照。
