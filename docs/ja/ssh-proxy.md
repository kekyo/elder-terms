# SSH proxyの使用

SSH踏み台を経由して、SSH、SFTP、Telnet、WebDAVの接続先へアクセスできます。
接続先のアドレスやユーザー名は、それぞれの接続設定に入力します。
独立した「SSH proxy」タブには、踏み台の接続情報を入力して使用を有効にします。
SSHとSFTPでは、踏み台と最終接続先を別々に認証し、ホスト鍵もそれぞれ確認します。
踏み台の確認には「SSH proxy」と表示します。

## 設定と継承

グローバル設定のSSH proxyタブで共通の踏み台を設定できます。
接続別の設定で「無効」を選ぶと、その接続では継承された踏み台を使用しません。
「継承」に戻すとグローバル設定を再び使用します。
ローカルシェルとシリアルには適用せず、接続中の経路設定は読み取り専用です。
設定を変更した場合は、再接続または新しい接続ウィンドウで反映します。

| `[ssh_proxy]` のキー | 既定値 | 意味 |
| --- | --- | --- |
| `enabled` | `false` | 踏み台を使用する場合は `true`。 |
| `address` | 空 | 踏み台のホスト名、IPv4、IPv6。URLやポートを含めずに指定します。 |
| `port` | `22` | 踏み台のSSHポート。1～65535。 |
| `username` | 空 | 踏み台のユーザー名入力の初期値。 |
| `identity_file` | 空 | 踏み台用の秘密鍵。空欄では既存SSH接続と同じ鍵の選択を使用します。 |

パスワードや秘密鍵のパスフレーズは、必要なときに入力します。設定ファイルには保存しません。

```ini
[general]
type=telnet
name=Internal console

[telnet]
address=console.internal.example
port=23

[ssh_proxy]
enabled=true
address=bastion.example.com
port=22
username=operator
identity_file=/home/operator/.ssh/bastion_key
```

踏み台の名前は手元で解決し、最終接続先の名前は踏み台に渡します。
そのため、手元では名前を解決できない内部ホスト名も使用できます。
踏み台への接続、認証、TCP転送に失敗しても直接接続へ切り替えません。
INIを編集した場合も、不正な有効化指定や有効時の不正な踏み台設定を接続前に拒否します。
WebDAVでは `NO_PROXY` / `no_proxy` が指定されていても、明示したSSH proxyを使用します。

## 接続と暗号化

踏み台側ではTCP転送が許可されている必要があります。
OpenSSHでは `AllowTcpForwarding`、`PermitOpen`、認証鍵ごとの転送制限などを確認して下さい。
[OpenSSHのサーバー設定](https://man.openbsd.org/sshd_config)を参照して下さい。

SSH/SFTPは最終接続先までSSHを使用します。HTTPSでは最終接続先とのTLSと証明書の名前検証を維持します。
TelnetやHTTPの場合、SSHで暗号化されるのは手元から踏み台までで、踏み台から最終接続先までの区間は平文です。

SSH端末から開くSFTPは、最終接続先の認証済み接続を共有します。
端末とSFTPの一方を閉じても、もう一方を使い続けられます。
SFTP単独起動と再接続にも同じSSH proxy設定を使用します。

踏み台は1台に対応します。多段接続、任意のProxyCommand、外部向けの汎用SOCKSサービスは提供しません。
