# plan12：libsmb2事前検証の記録

検証日：2026年10月29日

段階4の検証結果です。SambaとWindows 11の双方で、公開APIによる転送と上書き確定が成功しました。libsmb2の正式版6.3.1でWindows向けの名前変更不具合を確認したため、修正済みの上流リビジョンを固定して再検証しています。

## 検証用の成果物

| ファイル | 役割 |
| --- | --- |
| `elder-terms-terminal/tests/smb/smb-probe.cpp` | cardioとlibsmb2を組み合わせるC++20の検証ドライバー |
| `elder-terms-terminal/tests/smb/Makefile` | 依存ライブラリとドライバーのビルド、Samba試験の起動 |
| `elder-terms-terminal/tests/smb/run-samba.mts` | 独立したSambaコンテナを用意し、成功・失敗・中断を検証して後片付けするドライバー |
| `elder-terms-terminal/tests/smb/Containerfile`、`samba-entry.sh` | localhostにだけ公開する試験用Samba共有 |
| `elder-terms-terminal/tests/smb/run-win11.mts`、`windows-fixture.ps1` | `win11`に試験用共有を作成し、Linuxから検証して削除するハーネス |

libsmb2は[修正済みの上流ソース](https://github.com/sahlberg/libsmb2/tree/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64)を使用します。コミットは`727feaaa6d0f4fed1bae57745fbc2c5db9a30e64`です。上流ソースは編集せず、共有ライブラリを`.build/plan12-smb-sdk`へ配置しました。

当初の6.3.1では、Windowsの上書き確定が`STATUS_OBJECT_NAME_INVALID`になりました。[同版のSET_INFOエンコーダー](https://github.com/sahlberg/libsmb2/blob/4a78c54675233acd6de8997a347b0d371d5069a9/lib/smb2-cmd-set-info.c#L137)は、名前の区切り文字を変換する際に別のバッファを参照します。[採用版の修正](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/lib/smb2-cmd-set-info.c#L54)では、名前のバッファ内で変換します。

採用版には属性構造体の拡張もあるため、同じリビジョンのヘッダーと共有ライブラリを一組として使います。6.3.1のヘッダーやシステムライブラリとの混在は認めません。配布でも固定版をアプリとともに扱う必要があります。

NTLMSSPによるユーザー名・パスワード認証を対象とし、検証用ビルドではKerberosを無効にしています。APIの使用前に、同じ版の[README](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/README)、[高水準APIのコメント](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/include/smb2/libsmb2.h)、[低水準APIのコメント](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/include/smb2/libsmb2-raw.h)を確認しました。

## 再実行

ホストにはGCC/G++、Make、CMake、Git、pkg-config、GLib/GIOの開発用ファイル、Node.js、Podmanが必要です。これらの導入に管理者権限が必要な環境では、利用者に導入を依頼します。

```sh
make -C elder-terms-terminal/tests/smb deps
make -C elder-terms-terminal/tests/smb test
```

初回は依存ソースを取得し、Sambaのコンテナイメージを作成します。試験ごとにパスワードを生成し、コンテナ名と公開ポートを分けています。ポートはlocalhostに限定し、試験が失敗した場合もコンテナを削除します。既存の共有やホストのSamba設定は変更しません。

既存の試験専用共有へ接続する場合は、パスワードを`ELDER_SMB_PASSWORD`環境変数へ設定し、次の形式で起動します。通常の試験データを作成・削除するため、業務用共有を指定しないでください。

```sh
.build/smb-probe/smb-probe HOST SHARE USER PORT basic
.build/smb-probe/smb-probe HOST SHARE USER PORT encrypt
```

`ELDER_SMB_REPARSE_PATH`には、試験用リンクの共有内パスを指定できます。指定したリンクは削除せず、リンク自体を開くか、サーバーに拒否されることを確認します。

## Sambaで確認した結果

検証サーバーはUbuntu 24.04のSamba 4.19.5です。署名を必須とし、通常の共有、暗号化必須の共有、読み取り専用共有を用意しました。[Sambaの設定資料](https://www.samba.org/samba/docs/current/man-html/smb.conf.5.html)に従った構成です。

| 条件・操作 | 結果 |
| --- | --- |
| SMB2.1、SMB3.1.1 | いずれも接続・一覧・変更操作に成功 |
| 署名必須 | 接続と転送に成功 |
| 暗号化必須 | SMB3.1.1で接続と転送に成功 |
| SMB2.1で暗号化を要求 | 接続失敗。暗号化なしへの切り替えは起きない |
| 誤ったパスワード | 接続時に`STATUS_LOGON_FAILURE` |
| 存在しない共有 | 接続時に`STATUS_BAD_NETWORK_NAME` |
| 読み取り専用共有 | 一覧は成功し、フォルダー作成はアクセス拒否 |
| 日本語・空白を含む名前、大文字小文字の異なる参照 | 作成、内容確認、名前変更、削除に成功 |
| 空ファイル、1 MiB＋37バイトのファイル | 複数チャンクで送受信し、全バイトと長さが一致 |
| 排他的作成 | 既存ファイルへの作成要求は`EEXIST` |
| 通常の名前変更 | 存在しない名前へは成功し、既存ファイルの置換は拒否 |
| 上書き確定 | 低水準の置換付き名前変更が成功。確定後の内容が送信内容と一致 |
| 使用中ファイル | 共有を許可しないハンドルを開いた間は、別の書き込み用オープンを拒否 |
| 読み取り専用ファイル | 属性設定後の書き込み用オープンを拒否。属性解除後に後片付け可能 |
| 更新日時 | 指定したUnix秒を設定し、読み取り結果と一致 |
| 接続処理の中断 | 応答待ち中のキャンセル・期限切れでコールバックが一度だけ完了し、セッションを破棄 |
| 接続断 | ソケットの切断をエラーとして通知 |
| 読み取り・書き込み中のキャンセル | サーバーが操作を処理した後の応答を遮断して再現。中断後に新しいセッションで接続し、内容確認と所有済みファイルの後片付けに成功 |
| 読み取り中の接続断 | 中断後の再接続、内容確認、後片付けに成功 |
| 複数アドレスを持つホスト名 | `localhost`の最初のIPv6接続が拒否されてもIPv4候補へ接続。修正前の失敗と修正後の成功を確認 |
| 名前解決の取消 | 非同期解決を開始した直後に取り消し、キャンセルとして完了 |

転送中断は、TCP中継で対象のREAD/WRITE要求と応答を観測してから発生させています。一定時間のsleepに依存して中断位置を決める試験ではありません。

## 製品へ採用する構成と残る確認

名前解決にはGResolverの非同期APIを使い、libsmb2へは数値IPを渡します。libsmb2の接続実装には同期`getaddrinfo`があるためです。[接続実装](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/lib/socket.c)を確認しました。解決したアドレスを順に試し、名前解決と接続のそれぞれに期限を設けます。名前解決中のキャンセルも確認済みです。

cardioで読み書き可能なFDを待ち、結果を`smb2_service`へ渡します。ドライバーでは同時に一つのSMB要求だけを発行します。取消時はFD待機を解除した後でコンテキストを破棄し、保留中の完了通知を受け終えてから要求の状態を解放します。製品でも要求の直列化と、切断後に古いハンドルを使わない管理が必要です。

アップロードでは一時ファイルを排他的に作成し、低水準の`SMB2_FILE_RENAME_INFORMATION`で置換の有無を指定する方針です。[通常renameの実装](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/lib/libsmb2.c)は置換を指定しないため、上書き確定にはそのまま使いません。作成結果が不明な接続断を、所有を確認できた一時ファイルと区別する処理は、転送エンジンとの統合時にも検証します。

リンクをたどらない属性取得には、低水準CREATEの`FILE_OPEN_REPARSE_POINT`を使用します。通常の`smb2_stat_async`はリンクをたどらない契約の代用になりません。この指定で扱えるのはパスの最後の要素なので、中間のディレクトリと再帰処理の対象も確認する必要があります。Sambaの試験ではサーバー側でもリンク追跡を禁止しており、Windowsのジャンクションへの対応を証明した結果ではありません。

POSIX権限とリンク作成は機能情報を無効にする方針です。更新日時の適用と`upload_commit`は、両サーバーで確認できたため採用できます。採用版の暗号化設定は、未指定ならサーバーが要求した暗号化を許容し、必須指定なら条件を満たさない接続を拒否する方針です。署名は既定で必須とします。

libsmb2のLGPL-2.1の文書と共有ライブラリを配布物へ含める作業は段階7で行います。[上流のライセンス](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/LICENCE-LGPL-2.1.txt)を参照します。

## Windowsの検証結果

agent-rover 0.10.0の公開APIを使ったハーネスを用意しました。`win11`へ接続し、管理者であることを検査してから、一時ユーザー、通常・暗号化・読み取り専用共有、Linux側IPだけを許可するファイアウォール規則を作ります。既存の規則は変更しません。

通常の`npm install`で検証用依存を導入した後、管理者で起動した`win11`エージェントに対して次のコマンドを実行できます。

```sh
node elder-terms-terminal/tests/smb/run-win11.mts
```

共有外の検証用ディレクトリへのジャンクションを作り、リンクをたどらずにreparse属性を取得できることを確認しました。通常の転送、SMB2.1、暗号化必須、暗号化条件を満たさない接続の拒否、誤認証、共有不存在、読み取り専用共有での作成拒否も成功しています。各成功試験では、サイズ別の読み書き、排他的作成、上書き確定、使用中ファイル、読み取り専用属性、更新日時、名前変更と削除まで実行しました。

初回は管理者検査で停止しましたが、利用者が管理者として起動し直した後に実アクセスを完了しました。共有外の検証用ファイルが変化していないことと、共有内に転送の残骸がないことをWindows側でも確認しています。試験用共有、ファイアウォール規則、ユーザー、ディレクトリ、転送したスクリプトを削除し、agent-roverを解放しました。

## ハンドルと応答データの寿命

製品のファイル読み書きには、公開の低水準CREATE・READ・WRITE・CLOSEを採用し、ファイル識別子を値として保持します。高水準APIのハンドルへ変換する関数は、[採用版の実装](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/lib/libsmb2.c#L4070)でメモリーを確保しますが、コンテキストの破棄時にその割り当てを追跡する仕組みがありません。[破棄処理](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/lib/init.c#L447)も確認し、ヘッダーにある自動解放の説明だけには依存しない方針としました。中断試験も生の識別子を使う形で再実行しています。

低水準READ・WRITEの[APIコメント](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/include/smb2/libsmb2-raw.h)には完了データをNULLとする記述がありますが、同版の[高水準読み書き実装](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/lib/libsmb2.c)は公開の応答構造体から実際のバイト数を取得します。製品でもコールバック内で必要な値をコピーし、コールバック後に応答ポインターを保持しません。READの受け取り先とWRITEの送信元の寿命は、各非同期要求の完了まで保ちます。
