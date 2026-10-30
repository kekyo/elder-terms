# READMEのスクリーンショット撮影

この文書は、READMEとREADME_ja.mdで共用する44枚の画面画像を更新するための記録です。README冒頭の紹介画像6枚はウインドウ全体を示します。各節の画像は、対象の設定欄や操作結果と、その場所が分かる最小限の周辺を切り出します。

## 以前の画像から分かること

以前の画像のうち8枚には、`gnome-screenshot`で2026年8月5日または8月9日に撮影したメタデータが残っていました。残り4枚はGitの履歴から更新時期を確認しました。当時の実装はVTEを使っていました。正確なアプリのバージョン、表示倍率、撮影時の設定値は特定できません。

## 2026年10月23日の一括撮影

- アプリのバージョンは1.8.189です。対象ソースは`e3a88ab759ae061587e1f80238c75282aa778ce8`です。
- 現在のユーザーのX11セッションで`DISPLAY=:1`を使います。撮影するのはディスプレイ2の`DP-4`で、仮想デスクトップ上の範囲は`x=3840, y=1080, 幅=3840, 高さ=1600`です。
- 表示倍率は1です。GTKテーマはYaru-dark、UIフォントはUbuntu Sans 11、端末フォントはNoto Sans Monoです。UI言語を英語に固定し、同じ画像を日英のREADMEから参照します。
- 通常の撮影用アプリは個別のXDG設定ディレクトリとprivate D-Busを使い、ユーザーの保存済み設定から分離します。撮影用接続は`/tmp/elder-terms-readme/`以下のINIファイルに定義します。
- 物理画面の取得にはFFmpegの`x11grab`を使います。生成AIによる描画、画像合成、リサイズは行いません。各画像の正確な切り出し座標は`images/capture-manifest.json`を参照してください。

撮影中の状態と操作は`artifacts/readme-capture/shots.json`および`artifacts/readme-capture/actions.jsonl`に記録しています。これらは作業用の記録です。接続設定の例は`/tmp/elder-terms-readme/*.ini`にあります。一時ファイルを削除した後も再撮影できるよう、下記に画面の条件を残します。

## 2026年10月30日のIPスキャン・WebDAV画像の更新

`ip-scan-results.png`と`webdav-settings.png`を、バージョン1.10.69で再撮影しました。対象ソースは`413bce3258efbaf3d925ecaaf136601e1b4032a1`で、`make test`のビルド結果を使用しています。残り42枚は一括撮影時の画像です。

表示先には、撮影専用のXvfb（1600×1000、24 bit、`DISPLAY=:600`）を使用しました。表示倍率1、Yaru-dark、Ubuntu Sans 11、英語表示の条件は一括撮影時と同じです。個別のXDG設定ディレクトリとprivate D-Busでランチャーを起動し、FFmpegの`x11grab`で取得しました。画像の合成やリサイズは行っていません。

WebDAVのサンプル接続はHTTPS、アドレス`cloud.example.net`、公開パス`/remote.php/dav/files/demo`、ユーザー名`demo`です。ポート443とリモートディレクトリ`/`は組み込み既定値を使用します。アドレス欄の右側のIPスキャンボタンから接続URLまでが、画像の範囲です。

IPスキャン画像は、このボタンから実際の探索を完了し、9列の見出しとlocalhostの行を切り出しました。撮影時のlocalhostでは21・22・23番がTCP接続を受け付け、80・443・445・990番の列は空欄です。スキャン画面の大きさは1200×400です。

撮影用スクリプトと取得結果の記録は`artifacts/plan14/capture.mts`と`artifacts/plan14/capture.json`にあります。[画像の記録](../../images/capture-manifest.json)では、更新した2枚に個別の`application`と`environment`を保存しました。これらの項目がある画像では、その内容を最上位の共通項目より優先します。切り出し座標、取得日時、画像のハッシュも更新済みです。

同日、列見出しを`WebDAV(443)`・`WebDAV(80)`へ短縮した後、IPスキャン画像を再撮影しました。バージョンは1.10.72、対象ソースは`cee8b4a12541235618f901d584f278e2baff6e6f`です。表示先は`DISPLAY=:605`で、ほかの撮影条件は同じです。スクリプトと取得結果は`artifacts/plan14/header-labels/capture.mts`と`artifacts/plan14/header-labels/capture.json`に保存しています。

## 画像ごとの条件

| 用途 | 画像 | 再撮影する状態 |
| --- | --- | --- |
| 紹介 | `terminal.png` | ローカルシェルでディレクトリ一覧、C++ソース、ANSI色、Unicode文字を表示した端末ウインドウ全体 |
| 紹介 | `launcher-settings.png` | サンプル接続6件があるランチャーの全体。「Local terminal」の「General」タブを表示 |
| 紹介 | `complex-terminal.png` | 現行の端末描画で`btop`を実行したウインドウ全体 |
| 紹介 | `serial-terminal.png` | ローカルPTYからサンプルの起動・状態ログを受信中のシリアル端末全体。115200 bps、8N1 |
| 紹介 | `colored-terminal.png` | 青い外郭と背景を設定し、`htop`を実行した端末ウインドウ全体 |
| 紹介 | `sftp.png` | 127.0.0.1:40222の一時OpenSSHサーバーへ実際に接続したSFTPウインドウ全体。ローカルは`/tmp/elder-local`、リモートは`/tmp/elder-remote` |
| 基本操作 | `launcher.png` | ランチャーで「Local terminal」を選択した接続一覧 |
| 基本操作 | `local-terminal.png` | 起動直後のローカルシェルのプロンプト |
| 基本操作 | `settings-terminal.png` | 「Terminal」タブで文字コード、キー、画面サイズ、履歴、BELなどが見える範囲 |
| 基本操作 | `font-size.png` | 同じ本文を表示する端末2枚。左は16 DIP、右はそこから`Ctrl+=`を3回押した状態 |
| 基本操作 | `serial-indicator.png` | シリアル端末の接続・通信インジケーター。PTYには物理モデムラインがないため、実機の信号状態として説明しない |
| 接続設定 | `configuration-structure.png` | 「General」で明示した自動終了の値と、組み込み既定値を継承するコンパクトモードを並べる |
| 接続設定 | `connection-icon-picker.png` | Noto Color Emojiの選択ポップアップを開き、絵文字の候補と画像ファイル選択を表示 |
| 接続設定 | `ip-scan-results.png` | 実際にIPスキャンを完了し、9列の見出しとlocalhostの行だけを切り出す |
| 接続設定 | `ssh-sftp-settings.png` | 「SSH」タブのアドレス、ユーザー名、秘密鍵、端末種別。SFTPも共有する設定 |
| 接続設定 | `ssh-telnet-reconnect.png` | ループバックのTELNET接続を切断し、再接続ボタンが使える状態 |
| 接続設定 | `ftp-ftps-settings.png` | 「FTP」タブでパッシブ転送、明示的FTPS、証明書確認を設定 |
| 接続設定 | `webdav-settings.png` | 「WebDAV」タブでHTTPS、アドレス欄右側のIPスキャンボタン、公開パス、認証、完成したURLを表示 |
| ファイル転送 | `file-transfer-item-menu.png` | 実SFTP接続でサンプルのリモートファイルを選択し、右クリックメニューを開く |
| 端末とシリアル | `local-startup-process.png` | 「Local」の起動コマンドを`/bin/bash --login`に指定 |
| 端末とシリアル | `serial-device-selector.png` | 「Serial」で安定デバイスID、検出されたFTDIデバイス、現在のデバイスノードを表示。実ハードウェアの設定変更は行わない |
| 端末とシリアル | `serial-connection-monitoring.png` | 「Serial」の接続監視信号の選択欄 |
| 端末とシリアル | `terminal-encoding-keys.png` | 「Terminal」のUTF-8、Backspace、カーソルキー、Enter/Return設定 |
| 端末とシリアル | `terminal-text-selection.png` | ローカル端末のサンプルC++ソースを選択した状態 |
| 端末とシリアル | `terminal-send-paste.png` | 「Transfer」のテキスト送信速度と改行追従設定 |
| 端末とシリアル | `terminal-break-menu.png` | 接続中のTELNET端末で、右クリックメニューの「Send BREAK」を有効な状態で表示 |
| 端末とシリアル | `scrollback-settings.png` | 「Terminal」のスクロール履歴を20,000行に設定 |
| 端末とシリアル | `bel-sound-settings.png` | 「Terminal」のBELサウンドとファイル選択ボタン |
| 外観 | `compact-mode.png` | コンパクトモードの端末で、タイトル横にCONN、LOG、SD、RDを表示 |
| 外観 | `indicator-color-settings.png` | 「General」の点灯色を青、消灯色を灰色に設定 |
| 外観 | `text-size-settings.png` | 「Terminal appearance」の文字サイズを14.5 DIPに設定 |
| 外観 | `cursor-background-settings.png` | 縦線カーソル、透過背景、混色、背景画像の設定欄 |
| 外観 | `text-contrast-settings.png` | Oklchの明暗差補正を選び、「Compare colors」を展開した前後比較 |
| 外観 | `reverse-polarity-settings.png` | 反転方式Cを選び、「Compare colors」を展開した前後比較 |
| 外観 | `color-distinction-settings.png` | D型向け再配色、強さ60、「Compare colors」を展開した前後比較 |
| 外観 | `font-family-settings.png` | DejaVu Sans Mono、Noto Sans Mono CJK JP、Monospaceの優先順位付き一覧 |
| アプリと自動化 | `hotkey-settings.png` | 「General」で接続を開くショートカットを`Ctrl+Alt+L`に設定 |
| アプリと自動化 | `system-tray.png` | 現行GNOMEセッションのトレイのトップメニュー。「Open elder-terms」「Connections」「Application settings」「About」「Quit」を表示。接続サブメニューは開かない |
| アプリと自動化 | `terminal-type-settings.png` | 「SSH」の端末種別`xterm-256color` |
| アプリと自動化 | `language-settings.png` | ランチャーの「Application settings」で表示言語を表示 |
| アプリと自動化 | `about-dialog.png` | 「About」のバージョン1.8.189とホットキーの検出状態 |
| アプリと自動化 | `logging-settings.png` | ログ有効、接続名と日時を使うファイル名、UTF-8テキスト形式 |
| アプリと自動化 | `macro-settings.png` | `READY`に一致したとき`START`とCRLFを送るマクロルールを選択 |
| アプリと自動化 | `terminal-link-actions.png` | 表示テキストのローカルソースパスを認識し、エディターで該当行を開くルール |

SFTPのサーバーは127.0.0.1だけで待ち受け、公開鍵認証を使います。ホスト側のSSH設定とユーザーの`authorized_keys`は変更しません。撮影ではリモートディレクトリに`/tmp/elder-remote`を使いました。撮影後は一時サーバーを停止します。

通常の設定画像はprivate D-Busで撮影します。トレイはGNOMEのStatusNotifierWatcherが必要なので、実ユーザーのD-Busを継承します。起動前に`net.kekyo.elder-terms`の所有者がいないことを確認し、表示先をディスプレイ2に限定して一時的なランチャーを起動します。撮影用のXDG設定ディレクトリは引き続き分離します。

## 更新時の確認

撮影対象のソース、表示環境、サンプル接続を固定します。各節の対象が画像内で読めること、切り出しに隣の行の断片や欠けた文字がないことを目視します。比較画像は本文、表示倍率、範囲をそろえます。最後に日英READMEの全画像参照と画像の実体を照合します。
