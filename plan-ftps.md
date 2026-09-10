# libcurlによるFTPS対応計画

作成日: 2026-09-10

調査対象: 現在のコードベース、基準コミット `c0d16cb`（`chore: finalize libcurl FTP packaging and validation`）。

状態: 計画作成済み、実装未着手。明示的FTPSと暗黙的FTPSの両方を対象とする提案であり、この文書の保存を実装開始の指示とは扱わない。

## 1. 対応方針

既存のFTPバックエンドを拡張し、FTP接続の設定で暗号化方式を選べるようにする。接続種別は `[general] type=ftp` のまま、FTPタブに「暗号化」を追加する。

| 選択肢 | 保存値 `[ftp] tls_mode` | 組込み既定ポート | 接続方法 |
| --- | --- | --- | --- |
| FTP（暗号化なし） | `none` | 21 | 現在の平文FTP |
| FTPS（明示的TLS） | `explicit` | 21 | FTP接続後にTLSへ切り替えてから認証 |
| FTPS（暗黙的TLS） | `implicit` | 990 | TCP接続直後からTLSで通信 |

方式の違いとポートの根拠はcurl公式の[FTPS解説](https://everything.curl.dev/ftp/ftps.html)に従う。設定未指定時は現在のFTP動作を維持する。FTPSを選んだ場合、TLSの確立と検証に失敗した接続では認証・転送を続行しない。

- FTPSでは制御接続と、一覧取得・アップロード・ダウンロードに使う全データ接続を暗号化する。明示的FTPSでは、接続開始時の挨拶とTLS切替要求は平文になるが、ユーザー名・パスワードはTLS確立後に送る。
- サーバー証明書の信頼チェーン、有効期間、接続先名を検証する。TLSの最低バージョンは1.2、上限はlibcurlとTLSバックエンドに委ねる。
- Passiveを既定とし、現在のActive、IPv4／IPv6、EPSV／PASV、EPRT／PORTも検証対象に含める。
- ログイン時のユーザー名・パスワード入力、パスワードを保存しない動作、2ペインブラウザー、FIFOでの操作直列化、有界バッファ、キャンセルと非同期終了を引き継ぐ。
- 今回の範囲はFTPへのTLS追加。クライアント証明書認証、証明書検証を無効化する設定、初回接続での証明書恒久承認、証明書ピンニング、プロキシ設定、IP scanへの990番ポート追加は対象外とする。
- 既存のC++20／GTK3／Meson、npm／Vitest／gestamentを使用する。ビルド方式の移行、FTPの再実装、外部コードの変更は含めない。

## 2. 現在の実装と変更箇所

| 現在の実装 | 計画への影響 |
| --- | --- |
| `shared/include/elder-terms/settings/ftp-settings.h` と `shared/src/settings/ftp-settings.cpp` は平文FTPの接続情報を定義する。ポート既定は21。 | TLS方式、CAファイル、方式に応じた実効ポートを追加する。公開型・関数のDoxygenコメントも更新する。 |
| `shared/src/settings/settings-store.cpp` は不正値を警告し、その値を採用せず既定値・継承値を使用する。 | TLS方式の不正値が `none` に変わらないよう、接続を阻止できる検証結果を保持する必要がある。 |
| `shared/src/settings-widget/settings-widget.cpp` の `create_ftp_page()` と同期・変更処理は、アドレス、ポート、ユーザー名、データ接続方式、初期ディレクトリを扱う。実行中の接続先設定は読取り専用。 | 同じ継承・適用・取消の仕組みにTLS設定を追加する。 |
| `elder-terms-vte/src/ftp/ftp-client.cpp` の `endpoint_url()` はURLのschemeを `ftp` に固定する。 | 暗黙的FTPSだけ `ftps` にする。アドレスのエスケープ、IPv6、パス構築は既存処理を使う。 |
| `elder-terms-vte/src/ftp/curl-ftp-session.cpp` は操作ごとに `curl_easy_reset()` し、`CURLOPT_PROTOCOLS_STR="ftp"`、`CURLOPT_USE_SSL=CURLUSESSL_NONE` を設定する。 | 各操作で接続設定からTLSオプションを再適用する。初回ログインだけの変更では足りない。 |
| 同セッションはlibcurl 7.88.1以上、非同期DNS、FTP機能を実行時に確認する。 | FTPS選択時にはTLS機能と必要なプロトコルも確認する。 |
| `elder-terms-vte/src/file-transfer/file-transfer-main.cpp` はFTPの認証、タイトル、開始失敗表示、worker終了待機を扱う。 | FTP／FTPSの表示を選択方式に合わせ、TLS設定不正・接続失敗を既存UIで通知する。 |
| `elder-terms-vte/tests/cpp/ftp-test-server.cpp` は自前の平文FTP fixture。client、stream、lifetime、runtime-capabilitiesのC++テストと `ftp-live-window.test.ts` がある。 | fixtureへ実TLSを追加し、既存の実転送シナリオを拡張できる。 |
| `elder-terms-vte/meson.build` はlibcurl 7.88.1以上を要求し、`prereq.sh` は `libcurl4-openssl-dev` と `ca-certificates` を含む。配布パッケージの明示的実行時依存には `ca-certificates` がない。 | 最低libcurlバージョンを維持して検証する。配布先のCAストアもパッケージ検証に含める。 |

調査環境ではpkg-configがlibcurl 8.5.0、OpenSSL 3.0.13を報告し、curl-configはAsynchDNS、SSL、FTP、FTPSを報告した。これは開発環境の調査結果であり、最低対応版や配布環境での動作確認の代わりにはしない。

## 3. 設定・接続・証明書の詳細

### 設定形式と継承

```ini
[general]
type=ftp

[ftp]
address=ftp.example.com
tls_mode=explicit
username=alice
data_connection_mode=passive
local_directory=
remote_directory=.
```

独自CAを使う場合は `[ftp] ca_file=/absolute/path/company-ca.pem` を追加する。暗黙的FTPSは `tls_mode=implicit`。独自ポートは現在と同じ `port=2121` の形式で指定する。

- `tls_mode` の有効値は `none`、`explicit`、`implicit`。組込み既定は `none`。未指定はグローバル設定を継承する。
- ポートは接続設定、明示されたグローバル設定、TLS方式に対応する組込み既定値の順で解決する。現在の `SettingValueSource` を利用して、保存された21と組込み既定の21を区別する。
- TLS方式を変更した際、組込み既定を使用中の場合だけ実効ポートが21／990に追従する。ユーザーが明示した21、990、独自ポートやグローバルの指定値は上書きしない。ポートを空に戻した場合の表示も同じ解決処理を使用する。
- `ca_file` 未指定は継承、明示的な空文字列はシステムCA、絶対パスは指定CAバンドルとする。グローバルに独自CAがあっても、接続単位でシステムCAを選べるようにする。
- 指定CAファイルはPEM形式のCAバンドルとし、絶対パスで保存する。相対パスやシェル展開に依存した解決は行わない。存在しない・読めない・不正なファイルでは接続を失敗させる。
- `ca_file` が空ならlibcurlの組込みCA設定を使い、指定がある場合は `CURLOPT_CAINFO` に渡す。既定CAの場所はlibcurlのビルド設定によるため、パスをアプリに固定しない。独自バンドルの指定を「このCAだけを信頼する」という制限機能とは定義しない。根拠: [CURLOPT_CAINFO](https://curl.se/libcurl/c/CURLOPT_CAINFO.html)。
- 不明・空・壊れた `tls_mode` は、未指定と区別して接続エラーにする。読込み段階でキー・出所を持つ構造化検証結果を残し、コピー、継承、起動用設定の上書きにも反映する。下位設定の不正値を有効な上位指定が置き換えた場合はそのキーのエラーを解消する。警告文字列の検索や `none` へのフォールバックで処理しない。
- この検証結果は設定UIで修正できるように扱い、FTP起動時にはネットワーク接続前に確認する。他プロトコルや既存項目の不正値処理を一律に変更しない。

### libcurlへの適用

| 設定 | 平文FTP | 明示的FTPS | 暗黙的FTPS |
| --- | --- | --- | --- |
| URL scheme／`CURLOPT_PROTOCOLS_STR` | `ftp`／`ftp` | `ftp`／`ftp` | `ftps`／`ftps` |
| `CURLOPT_USE_SSL` | `CURLUSESSL_NONE` | `CURLUSESSL_ALL` | `CURLUSESSL_ALL` |
| `CURLOPT_FTPSSLAUTH` | 適用不要 | `CURLFTPAUTH_TLS` | 適用不要 |
| `CURLOPT_SSL_VERIFYPEER` | TLS未使用 | `1L` | `1L` |
| `CURLOPT_SSL_VERIFYHOST` | TLS未使用 | `2L` | `2L` |
| `CURLOPT_SSLVERSION` | TLS未使用 | `CURL_SSLVERSION_TLSv1_2` | `CURL_SSLVERSION_TLSv1_2` |

- `CURLUSESSL_ALL` で制御・データ双方の保護を要求する。`CURLUSESSL_TRY` や制御接続だけを保護する方式は使用しない。`AUTH`、`PBSZ`、`PROT`の手動送信は追加せずlibcurlに任せる。根拠: [CURLOPT_USE_SSL](https://curl.se/libcurl/c/CURLOPT_USE_SSL.html)。
- `CURLFTPAUTH_TLS` は `AUTH TLS` を優先する指定であり、失敗時の `AUTH SSL` 試行を禁止する指定ではない。最低TLSバージョンの制約は別に適用する。両AUTH要求を拒否された場合には認証しない。根拠: [CURLOPT_FTPSSLAUTH](https://curl.se/libcurl/c/CURLOPT_FTPSSLAUTH.html)、[CURLOPT_SSLVERSION](https://curl.se/libcurl/c/CURLOPT_SSLVERSION.html)。
- 信頼チェーン検証と接続先名検証の両方を有効にする。URLには設定されたホスト名を保持し、DNS解決した数値IPへの置換でSNIや名前検証を変えない。IP指定には一致するIP SANが必要。根拠: [CURLOPT_SSL_VERIFYPEER](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html)、[CURLOPT_SSL_VERIFYHOST](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYHOST.html)。
- `CURLOPT_FTP_SSL_CCC` は無効のままとし、認証後も制御接続を暗号化する。根拠: [CURLOPT_FTP_SSL_CCC](https://curl.se/libcurl/c/CURLOPT_FTP_SSL_CCC.html)。
- TLSオプションはworker内で `curl_easy_reset()` 後に毎回設定し、戻り値を確認する。現在の最低版にも対応するようenum値の `long` キャストを保つ。
- 1ウィンドウの接続設定をセッション中は固定する。新しい設定で接続し直す場合は新しいセッションを作る。`curl_easy_reset()` は既存接続とTLSセッションキャッシュを消去しないため、リセットを設定変更後の再認証手段として扱わない。根拠: [curl_easy_reset](https://curl.se/libcurl/c/curl_easy_reset.html)。
- 既定のTLSセッションキャッシュを維持する。データ接続でセッション再利用を要求するサーバーとの相互接続は実際に検証する。根拠: [CURLOPT_SSL_SESSIONID_CACHE](https://curl.se/libcurl/c/CURLOPT_SSL_SESSIONID_CACHE.html)。
- 実行時能力検査は既存の `feature_names`／`protocols` を使う。平文FTPにTLSを要求せず、明示的FTPSではFTP＋SSL、暗黙的FTPSではFTPS＋SSLを追加確認する。API利用前に既存の版・構造体世代チェックを行う。根拠: [curl_version_info](https://curl.se/libcurl/c/curl_version_info.html)。
- libcurl処理は現在の専用workerと非同期adapterで実行する。TLS処理のための追加workerや、GTK側のポーリング待機は導入しない。

libcurlオプションは公式文書と、導入済み `/usr/include/x86_64-linux-gnu/curl/curl.h` のAPIコメントを調査済み。実装時に追加する外部APIについても公式文書と対象環境のコメントを確認し、非推奨・内部APIは使用しない。

### UIと失敗時の動作

- 暗号化方式は既存の継承可能な選択UIに組み込む。FTPのデータ接続方式とは別項目として表示する。
- CA設定は接続画面で「継承」「システムのCA」「指定ファイル」、グローバル画面では後者の2択とする。「指定ファイル」では入力欄とファイル選択を表示し、未入力では保存・適用できない。
- 平文FTPではCA設定を無効表示する。FTPSに戻した際には編集中の値を復元する。実行中のウィンドウからはTLS方式・CA設定を変更できない。
- FTPS選択時のウィンドウと認証・接続失敗表示にFTPSを使用する。接続前の方式表示を、証明書の検証に成功した表示として扱わない。
- 設定エラー、CAファイルの読込み失敗、証明書検証失敗、TLS交渉失敗、ログイン拒否を区別して通知する。libcurlのエラーコードを判断に使い、英語のエラーテキストを解析しない。同じコードに集約される証明書問題は検証失敗としてまとめ、詳細メッセージを添える。
- TLSや証明書のエラーを無視する接続ボタンは設けない。パスワードや秘密情報を診断ログへ追加しない。

## 4. テスト方針と共通の進め方

各段階で期待動作のテストを先に追加し、ルートの `npm run test` を全体実行してREDを確認する。その段階の実装後、同じ全体実行でGREENを確認してコミットする。未実装の新APIを参照してコンパイルを壊すことをREDの代用にせず、可能な限りINI入力、既存クライアントAPI、実ウィンドウの操作から不足する動作を再現する。

- ビルドと全体テストは同じビルドディレクトリで重複起動しない。既存のルートテストがC++、共有設定UI、ランチャー、端末・ファイル転送を順に検証する流れを使用する。
- 自前のC++ FTP fixtureへOpenSSLによるTLS transportを合成し、既存のコマンド処理・ファイル操作を使用する。OpenSSLへの直接依存はテストサーバーに限定する。アプリ本体はlibcurl経由でTLSを使用する。
- テストfixtureは制御・データ接続それぞれのTLS確立と実際の暗号化I/Oを確認できるようにする。`PROT P` がログにあることだけではデータの保護を検証済みとしない。テストサーバーの鍵や証明書は製品へ同梱しない。
- テスト用CAと証明書は一時ディレクトリで生成する。正常、有効期限切れ、未信頼、名前不一致などの証明書を用意し、期限切れになるのを待つテストや外部の公開FTPサービスに依存しない。生成・起動の補助スクリプトが必要ならNode.jsを使用する。
- テスト専用の `libssl-dev`／`openssl` を段階1でMesonと `prereq.sh` のビルド・テスト前提へ明示する。必要なパッケージやPodmanイメージが不足する場合は具体名を示してインストールを促す。未実行の必須検証を完了扱いにしない。
- fixtureのREADY、TLS確立、データ接続待機、最終応答待機、切断完了のイベントを同期点にする。経過時間だけで「キャンセルできた」「平文を送っていない」と判断しない。
- UIの応答性とキャンセル・ウィンドウ終了は既存のgestament動画検証を拡張して確認する。画像マスターの追加・変更時は画像を目視する。
- TLSの接続・転送結果、保存した設定を再読込みした動作、実ファイルの内容を検証する。ソースコード中のオプション名や証明書ファイルの文字列だけを調べるテストにはしない。
- 新しい失敗が出た場合は計画との整合と原因を見直し、不要になった暫定変更を戻す。各段階の変更対象は以下の成果物に必要な範囲とする。

## 5. 段階1: 設定ファイルから明示的FTPSを使えるようにする

成果物: INIで `tls_mode=explicit` を指定し、実際のファイル転送ウィンドウで暗号化された一覧取得・送受信ができる。

1. fixtureの明示的TLS、証明書生成、TLSイベントを整備し、既存API／INIから呼ぶ成功・失敗テストを追加する。全体テストで未対応のREDを確認する。
2. `tls_mode`、`ca_file`、不正TLS設定を接続エラーにする読込み経路を追加する。保存・継承・再読込みまで同じ段階で扱う。この段階では `implicit` を指定した接続は未対応として拒否し、段階2の実装完了まで平文接続や明示的FTPSで代用しない。
3. セッションに `CURLUSESSL_ALL`、証明書検証、最低TLSバージョン、CA指定、実行時能力確認を追加する。既存workerの停止・失敗時cleanupを使う。
4. ファイル転送ウィンドウに設定を渡し、FTPSのタイトルと認証・失敗表示を追加する。INIからの起動とPassiveでの実転送を観測可能にする。
5. READMEのFTPS未対応記述と設定例を、この段階で利用可能な明示的FTPSの説明に更新する。英語・日本語と変更した公開コメントを揃える。

完了条件:

- [ ] 明示的FTPSでログイン、一覧、バイナリの送受信、ディレクトリ作成、名前変更、削除が成功する。
- [ ] 正しい独自CAでは成功し、未信頼・期限切れ・名前不一致・壊れたCAファイルでは失敗する。
- [ ] `AUTH TLS`／`AUTH SSL`拒否、`PROT P`拒否、データ接続のTLS失敗で平文の認証・転送へ移行しない。
- [ ] 不正なTLS方式と、この段階では未対応の暗黙的FTPSはネットワーク接続前に拒否され、設定の保存・継承・起動用上書きでも安全性を維持する。
- [ ] 連続する一覧・転送、制御接続が張り直される場合にもTLS設定を適用する。TLS能力のない実行時環境ではFTPSだけを拒否する。
- [ ] 実ウィンドウから操作でき、既存の平文FTPを含むルート全体テストが成功する。

コミット例: `feat: support explicit FTPS with certificate verification`

## 6. 段階2: 暗黙的FTPSとデータ接続・終了処理を完成させる

成果物: 両FTPS方式をINIから選択でき、Active／Passiveを含む転送とキャンセルが安定して動作する。

1. fixtureへ暗黙的TLSを追加し、最初の受信がTLSであることを確認するテストと実効ポートのテストを先にRED確認する。
2. `ftps://` のURLと許可プロトコル、FTPSの能力検査、組込みポート990の解決を追加する。明示ポートとグローバル継承を保持する。
3. 両方式のActive／Passive、IPv4／IPv6、EPSV／PASV・EPRT／PORTの既存代替動作、MLSD／LISTを検証する。データ接続先を制御接続の相手に制限する現在の方針も確認する。
4. TLS接続中・データハンドシェイク中・送受信中・最終応答待ち・終了時の切断を検証する。中断・短い受信・最終応答エラーを成功にしない。部分ファイル処理は既存の転送方針に従う。
5. セッション再利用を要求するfixtureケースで、一覧後の複数転送を検証する。TLS 1.2と1.3、両方式に対応したREADMEへ更新する。

完了条件:

- [ ] 暗黙的FTPSは接続直後からTLSを使用し、両方式で実ファイルの一覧・送受信・変更操作が成功する。
- [ ] 組込みポートは21／990に追従し、明示値21・990・独自ポートとグローバルの指定値を変更しない。
- [ ] 両方式×Active／Passive×IPv4／IPv6で実転送が成功し、IPv4の従来コマンドへの代替も動作する。
- [ ] TLS 1.2／1.3と、データ接続でのTLSセッション再利用要求を満たす接続が成功する。TLS 1.1以下だけを許す相手との接続は拒否する。
- [ ] TLS処理中でもUIの応答性、キャンセル、ウィンドウ終了を維持し、全workerと待機操作の終了を確認できる。
- [ ] TLS切断と最終応答の異常を転送成功として扱わず、ルート全体テストが成功する。

コミット例: `feat: support implicit FTPS and verify transfer lifecycle`

## 7. 段階3: ランチャーの設定UIからFTPSを使えるようにする

成果物: 利用者がINIを編集せず、両FTPS方式と信頼するCAを選び、接続を開始できる。

1. 共有設定UIとランチャーで、暗号化方式、CA選択、ポート表示、保存・適用・取消・継承を操作するテストを追加してRED確認する。
2. `settings-widget.cpp`、`settings-presentation.cpp`、対応するテストfixtureと翻訳へ設定項目を追加する。実効ポートの計算を重複実装しない。
3. 英語・日本語のFTPS表示と失敗理由を確認する。証明書エラーとログイン拒否を混同せず、必要な修正内容を既存エラーパネルで確認できるようにする。
4. ランチャーで接続を作成・保存し、起動した実ウィンドウから両FTPS方式のファイル転送を完了するシナリオを追加する。READMEの利用手順もUIからの操作へ更新する。

完了条件:

- [ ] 接続設定とグローバル設定で暗号化方式とCAを選べる。独自CAの継承からシステムCAへの明示的な切替も保存・復元できる。
- [ ] 方式変更時のポート表示、明示ポートの保持、リセット、保存、取消、外部再読込みが正しい。
- [ ] 不正なTLS設定をUIで確認・修正でき、修正前には接続しない。独自CAファイルの未入力などで保存・適用を止める。
- [ ] 実行中はTLS方式・CAを編集できず、次回起動した接続に保存した設定を適用する。
- [ ] ランチャーからの実FTPS転送、英語・日本語の表示とエラー通知、必要な画像の目視確認を終え、ルート全体テストが成功する。

コミット例: `feat: expose FTPS encryption and CA settings in the launcher`

## 8. 段階4: 配布環境と実サーバーで検証する

成果物: 開発環境と配布パッケージの両方でFTPSを利用でき、対応範囲が利用者向け文書に反映されている。

1. 配布パッケージの実行時依存へ `ca-certificates` を追加し、段階1で追加したテスト用依存が全対象の `prereq.sh` 経路に反映されていることを確認する。パッケージの生成結果とインストール後の動作をテストする。
2. 一時的な隔離コンテナの信頼ストアへテストCAを登録し、`ca_file` 未指定で成功すること、未登録では拒否されることを確認する。ホストの信頼ストアは変更しない。
3. 既存の `ELDER_TERMS_TEST_FTP_APP` による実行対象切替を使い、インストール済みアプリでも全ウィンドウシナリオを含むルート全体テストを実行する。必要な共有ライブラリとTLSバックエンドが配布先で解決されることを確認する。
4. 最低対応のlibcurl 7.88.1を使うDebian 12の検証環境と、通常の64bit環境、既存のDebian trixie i386経路でビルド・全体テストを確認する。最低版環境の選定根拠は[Debian bookwormのlibcurl4パッケージ](https://packages.debian.org/bookworm/libcurl4)。i386では `ELDER_TERMS_TEST_DEBIAN_TRIXIE_I386=1` を付けて既存の任意検証を有効にする。
5. fixture以外の実FTPSサーバー（vsftpdを候補）を隔離コンテナで起動し、明示的／暗黙的FTPSとTLSセッション再利用要求のあるデータ転送を確認する。[vsftpd公式設定文書](https://security.appspot.com/vsftpd/vsftpd_conf.html)と採用版の同梱man pageを確認し、再現可能な設定をテスト側に置く。
6. README英語・日本語へ方式の選択、証明書と接続先名、独自CA、失敗時の確認箇所、TLSを有効にしたlibcurlとCAパッケージの必要条件を記載する。FTPSでは暗号化された制御内容をNAT機器が読めないため、データ接続用の経路設定が必要になる点には[公式FTPS解説](https://everything.curl.dev/ftp/ftps.html)を付ける。

完了条件:

- [ ] 開発ツリーとインストール済みパッケージの両方で、両FTPS方式の実ウィンドウ転送が成功する。
- [ ] システムCAと指定CAの両経路で成功・拒否を検証し、パッケージにCAの実行時依存を含める。
- [ ] 最低libcurl版、通常64bit、i386で必要なビルド・全体テストが成功する。
- [ ] fixture以外のサーバーでも保護されたデータ転送とセッション再利用を確認する。検証したサーバー・libcurl・TLSバックエンドの版を記録する。
- [ ] README、翻訳、公開コメントに実装済み機能との矛盾がなく、外部依存による制約には出典がある。

コミット例: `chore: validate and package FTPS support`

## 9. 最終完了条件と実施記録

実装完了時は、各段階の完了条件と成果物を照合し、以下を全て確認する。

- [ ] 両FTPS方式を設定UIから選択し、既存のファイル操作を暗号化して実行できる。
- [ ] 証明書・TLS・設定の異常で認証・転送を続行せず、平文FTPへ暗黙に切り替わらない。
- [ ] 設定の継承、方式別ポート、独自CA、実行中の設定保護とエラー表示が整合する。
- [ ] 平文FTP、SFTP、その他の既存機能を含むルート全体テストが成功し、TLS中のキャンセル・終了と配布環境の検証も完了する。
- [ ] 各段階が完了条件を満たした状態でコミットされ、不要な暫定コード・テスト専用秘密鍵を成果物へ残さない。

計画作成時の確認: 現行コードと直前のlibcurl移行履歴、公式文書、導入済みAPIコメント、設定の継承・不正値処理、実FTPテストと配布経路を調査した。実装対象、各段階の観測可能な成果物、全体RED／GREEN、段階別コミット、最終完了条件を記載した。今回の変更は計画文書のみで、ビルド手順への影響がないためビルド・テストは実行しない。

実装開始後は、各段階についてREDの失敗内容、GREENの全体実行結果、動画・画像の確認、配布環境、コミットID、必要に応じた計画の修正理由をここへ追記する。
