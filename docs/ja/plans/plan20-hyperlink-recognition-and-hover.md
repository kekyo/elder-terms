# plan20：リンクの終端認識、設定配置、ホバー下線

作成日：2026年11月2日

状態：計画作成。実装は未着手です。

## 目的と再現例

隣接するMarkdown形式のリンクを、一続きのURLとして認識する問題を修正します。リンク表示の設定を色補正の途中から移し、ポインターが重なったときだけ下線を追加するモードを加えます。

再現に使う端末出力は次の文字列です。Markdownのラベルを隠して描画する機能の追加は含めず、表示されたURL部分の範囲と操作対象を正しくすることを目的とします。

```text
"[Combine Streamの仕様](https://docs.pipewire.org/page_module_combine_stream.html)、[PipeWire 1.0.5のcombine-stream実装](https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-combine-stream.c)"
```

期待するリンク先は次の2件です。閉じ括弧、読点、次のラベル、末尾の引用符は含めません。

```text
https://docs.pipewire.org/page_module_combine_stream.html
https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-combine-stream.c
```

調査基準と共通の実施条件は[plan15](plan15-transfer-jobs-and-tree-icons.md)に従います。ポインター処理を変更する[plan21](plan21-selection-mode-during-drag.md)より先に統合する順序です。

## 現在のコード

| 領域 | 現状 |
| --- | --- |
| 既定ルール | [hyperlink-settings.cpp](../../../shared/src/settings/hyperlink-settings.cpp)の`http-url-text`は空白・引用符・山括弧など以外を広く許可し、閉じ括弧や読点で停止しない |
| 認識と装飾 | [terminal-links.cpp](../../../elder-terms-terminal/src/terminal-links.cpp)が文字列・OSC 8のルール、注釈、パス検証、キャッシュを管理する |
| ポインター | [terminal-runtime.cpp](../../../elder-terms-terminal/src/terminal-runtime.cpp)が座標、リンクのhover、Ctrlクリック、選択ドラッグを扱う |
| 表示設定 | [terminal-settings.h](../../../shared/include/elder-terms/settings/terminal-settings.h)の`TerminalLinkDecoration`は`none`、`underline`、`validated` |
| 配置 | [settings-widget.cpp](../../../shared/src/settings-widget/settings-widget.cpp)では色補正・色覚補正の後、色プレビューの前にリンク表示設定が置かれる |
| リンク設定ページ | [hyperlink-settings-editor.cpp](../../../shared/src/settings-widget/hyperlink-settings-editor.cpp)に認識ルールと操作設定の画面がある |

## URL終端の規則

まずURL候補を取り出し、文脈上の区切りで有効な範囲を決めます。閉じ括弧をすべて一律に禁止するとURL中の正当な括弧まで失うため、対応する括弧と外側の文章を区別します。候補の最大長と走査量を制限し、大量の端末出力でも過剰な再走査を起こさない構成です。

| 入力 | 期待する扱い |
| --- | --- |
| `(https://host/a)`、`[label](https://host/a)` | 外側の閉じ括弧を含めずURLを認識する |
| `https://host/a_(b)` | URL内で対応する括弧を保持する |
| `https://host/a)、次の文章` | 未対応の閉じ括弧と日本語の区切りで終了する |
| `https://[2001:db8::1]/a` | IPv6の角括弧をURL内部として扱う |
| パーセント表記の括弧、クエリー、フラグメント | エンコードを勝手に解除せず、リンク先の値を保持する |
| 末尾の句読点 | 外側の文章と判定できる規則だけを適用する。クエリー中のカンマなどを無条件に切らない |
| 画面幅による折り返し | 同じ論理行のURLを保持する。明示的な改行は別の境界として扱う |
| OSC 8 | 端末が指定した完全なURIを対象とし、表示文字列用の括弧除去を適用しない |

認識後の文字範囲、注釈、Ctrlクリックの引数、「リンクをコピー」の内容を同じ正規化結果から作ります。表示範囲と実行対象が一致することが条件です。UTF-8のバイト数とセル範囲の変換も検証します。

既定のURL認識に必要な補正と、利用者が定義した正規表現ルールは分けます。ルールIDが既定値と同じという理由だけで、利用者が変更したルールへ補正を強制しません。組み込み認識方針として表すか、必要な境界情報をルールへ持たせるかを段階1で比較し、最小の変更にします。boxrig本体のソースや参照は変更しません。

URIで許される文字と文章中の自動認識は別の契約です。文字の解釈は[RFC 3986](https://www.rfc-editor.org/rfc/rfc3986.html)に照らして確認し、曖昧な末尾をすべてのURLで完全判定できるとは説明しません。明示的なOSC 8や利用者ルールを保持します。

## ホバー下線

新しい表示値の案は`hover`です。既存の3モードを残し、ポインターが対象リンクに重なっている間だけ、アプリが下線を追加します。Ctrlキーの押下は下線表示の条件にせず、開く操作は現在のCtrlクリックのままです。

| 場面 | ホバーモードの動作 |
| --- | --- |
| 通常時 | アプリによる下線を追加しない |
| 対象リンク上 | そのリンクの表示範囲へ下線を追加する |
| ポインターが離れる、ウインドウ外へ出る | 追加した下線を解除する |
| 出力、スクロール、再配置で内容が変わる | 最後のポインター位置で再判定する。古い注釈へ下線を残さない |
| 同じURLが別の位置にもある | ポインターが指すリンクだけを強調する |
| 端末自身が下線を指定している | ホバー終了後も元の下線・二重線などを保持する |
| ローカルパスを非同期検証している | 検証結果と現在のリンクを照合し、古い結果で別のリンクを強調しない |

リンクの同一性には画面、論理行とその世代、ルール、認識範囲、OSC 8の識別情報を使います。URL文字列だけで同一視しません。折り返された一つのリンクは対応する範囲をまとめて強調します。

装飾は現在の注釈APIを使い、端末が指定する下線を補う`UNDERLINE_FALLBACK`の契約を維持します。根拠は[boxrigの色補正文書](../../../deps/boxrig/docs/ja/color-filters.md)と[公開コメント](../../../deps/boxrig/include/boxrig/ui/view.h)です。認識結果やパス検証キャッシュを、hoverが変わるたびに作り直さない構成にします。

## 設定位置

「リンク表示」は既存のリンク設定ページの先頭へ移す案を採用候補とします。同じページで表示方法を選び、その下で認識ルールと起動動作を編集できる並びです。色補正ページでは補正項目と色プレビューを連続させます。

設定キーは通常の継承可能な`[terminal] link_decoration`のまま使えます。画面上の配置と保存先は独立しているため、位置を移すためだけに設定ファイルを移行する必要はありません。グローバル設定、接続設定、実行中の設定のすべてで、保存・継承・Apply・Cancelを確認します。

## 段階1：再現文字列の認識と操作を修正する

[boxrig-links-test.cpp](../../../elder-terms-terminal/tests/cpp/boxrig-links-test.cpp)などへ提示文字列をそのまま出力する回帰テストを追加します。検証対象は2件の範囲とリンク先、括弧・読点の非リンク領域、折り返しです。現状のREDを確認してから修正します。

正当な括弧、IPv6、クエリー、OSC 8、利用者ルールのケースも確認します。GUIでCtrlクリック先を試験用の起動先へ記録し、「リンクをコピー」の内容を比較します。完了条件は、提示例の2件が別々に正しく操作でき、既存の認識方式が維持されることです。

## 段階2：設定を移し、ホバーモードを追加する

設定値と配置を変更し、保存・継承・適用を実装します。既存のhover判定から装飾を更新する処理を追加し、静止したポインターの下で出力が変わる場合も反映します。

完了条件は、ホバーの前後で追加下線が出入りし、端末自身の下線が消えないことです。画面上の色補正項目とプレビューも連続して表示されます。

## 段階3：ポインター操作と全体検証

[terminal-hyperlink.test.ts](../../../elder-terms-terminal/tests/terminal-hyperlink.test.ts)、[boxrig-link-styles.test.ts](../../../elder-terms-terminal/tests/boxrig-link-styles.test.ts)、[boxrig-link-refresh.test.ts](../../../elder-terms-terminal/tests/boxrig-link-refresh.test.ts)へ機能検証を追加します。ドラッグによる選択、Ctrlクリック、スクロール、出力更新、再接続、破棄を確認します。

ポインターの移動と下線の出入りを動画でアサートし、設定画面と表示を目視確認します。入力・描画の既存性能試験を含む`make test`を実行し、利用者向け説明を更新して完了とします。

## 最終完了条件

1. 提示された文字列から2件のURLを正しい終端で認識し、表示・実行・コピーの対象が一致する。
2. URL内部の正当な括弧、IPv6、折り返し、OSC 8、利用者定義ルールを損なわない。
3. リンク表示をリンク設定ページで操作でき、色補正とプレビューの間に挟まらない。
4. ホバー時だけ追加下線が現れ、出力・スクロール・退出で更新され、元の端末属性は保持される。
5. 動画、目視確認、リンク・選択・全体テストが完了している。
