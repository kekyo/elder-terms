# ソースからのビルドとパッケージ生成

ソースから利用する場合のビルド手順と、開発・配布用の手順です。配布済みのdebパッケージを使う場合は[READMEのインストール](../../README_ja.md#インストール)を参照してください。

## セルフビルド

NFSにはシステム版の[libnfs](https://github.com/sahlberg/libnfs) 5.0.2以降と開発用ヘッダーが必要です。

WebDAVにはHTTP/HTTPSと非同期DNSを有効にしたlibcurl 7.88.1以降、および[libxml2](https://gnome.pages.gitlab.gnome.org/libxml2/) 2.9.14以降が必要です。

FTPクライアントには、FTPと非同期DNSを有効にした[libcurl](https://curl.se/libcurl/) 7.88.1以降が必要です。FTPSには[TLSを有効にしたlibcurl](https://curl.se/libcurl/c/libcurl-tutorial.html)が必要です。Debian／Ubuntuの配布パッケージはOpenSSL版libcurlを使用し、既定の信頼ストアのために `ca-certificates` に依存します。ソースからビルドする場合はOpenSSLのヘッダーとライブラリも必要です。証明書確認ではlibcurlと同じバージョンのOpenSSLが必要で、接続前にアプリケーションが確認します。

UbuntuまたはDebianでビルドする場合は、C++20対応コンパイラ、Meson、Ninja、gettextと、使用するライブラリの開発パッケージをインストールします。
描画にはGTK3 3.24以降、Pango 1.50以降、Cairo 1.18以降、IBus 1.5.29以降、Wayland client 1.22以降が必要です。[boxrigのビルド要件](https://github.com/kekyo/boxrig/blob/0.12.0/docs/ja/building.md)も参照してください。

```bash
sudo apt update
sudo apt install build-essential git meson ninja-build pkg-config gettext imagemagick libgdk-pixbuf2.0-bin librsvg2-common \
  libglib2.0-dev libgtk-3-dev libgdk-pixbuf-2.0-dev libcanberra-dev libx11-dev libxfixes-dev \
  libxkbcommon-dev liburing-dev libudev-dev libsystemd-dev libssh-dev libnfs-dev libcurl4-openssl-dev \
  libcairo2-dev libpango1.0-dev libfontconfig1-dev libepoxy-dev libibus-1.0-dev \
  libwayland-dev libgl-dev libegl-dev libssl-dev libxml2-dev libtasn1-6-dev libtasn1-bin doxygen patch ca-certificates xz-utils xdg-utils
```

このほか、Node.js 22.18以降が必要です。[TypeScriptを直接実行できる版](https://nodejs.org/docs/latest-v22.x/api/typescript.html)を使用します。ディストリビューションのパッケージを使う場合は、インストール後にバージョンを確認してください。

Material IconsのPNG生成には、SVGを読み込める`gdk-pixbuf-thumbnailer`とImageMagickの`convert`を使います。
生成した画像は配布物に含まれるため、利用者の環境にこれらの生成ツールは不要です。

```bash
sudo apt install nodejs
node --version
```

あるいは、ディストリビューション付属の Node.js が古い場合は、 [Node.js の公式配布物](https://nodejs.org/ja/download) や任意のバージョン管理ツール（例えば [nvm](https://github.com/nvm-sh/nvm)）を使用して下さい。

新しくソースコードを取得する場合は、依存する [cardio](https://github.com/kekyo/cardio/)、[libxyzm](https://github.com/kekyo/libxyzm/)、[boxrig](https://github.com/kekyo/boxrig/) も取得するため、サブモジュールを含めてcloneします。

```bash
git clone --recurse-submodules https://github.com/kekyo/elder-terms.git
cd elder-terms
```

既にclone済みの場合は、次のコマンドでサブモジュールを初期化・更新出来ます。

```bash
git submodule update --init --recursive
```

npm依存パッケージを取得して、全ワークスペースをビルドします。

```bash
npm ci
npm run build
```

ビルドしたランチャーは、リポジトリのルートから次のコマンドで起動出来ます。ランチャーは同じビルドディレクトリにあるターミナル実行ファイルと、SFTP/FTP共通のファイル転送実行ファイルを自動的に検出します。

```bash
./.build/elder-terms/elder-terms
```

次のコマンドでも起動できます。

```bash
npm run dev
```

テストは次のコマンドで実行してください。UIテストを含むため、完了まで時間がかかります。

```bash
npm run test
```

## debパッケージの生成

パッケージ生成スクリプトは、ランチャー、ターミナル、SFTP/FTP共通のファイル転送実行ファイル、専用共有ライブラリとUIデータ、翻訳カタログ、デスクトップエントリ、XDG自動起動エントリ、アイコン、文書、ライセンス表記を、一つの`elder-terms` debパッケージにまとめます。

対応するパッケージの組み合わせは次の通りです。

| ディストリビューション | リリース | アーキテクチャ |
| :--- | :--- | :--- |
| Debian | trixie | amd64, i386, arm64, armhf, riscv64 |
| Ubuntu | 24.04 | amd64, arm64 |
| Ubuntu | 26.04 | amd64, arm64 |

Debian bookwormは対象外です。標準の[Cairo 1.16](https://packages.debian.org/bookworm/libcairo2-dev)と[IBus 1.5.27](https://packages.debian.org/bookworm/libibus-1.0-dev)が、描画ライブラリの必要なバージョンに届かないためです。

ホストにはPodman、非ネイティブターゲット用のbinfmt/QEMU、GNU Make、`dpkg-deb`、`readelf`、Node.js 22.18以降、npmと`npx`が必要です。
プリセット画像の準備には、SVGを読み込める`gdk-pixbuf-thumbnailer`、ImageMagickの`convert`、GLibの`glib-compile-resources`も使います。
先に`npm ci`を実行してください。リポジトリのルートで、再利用可能な前提イメージを一度生成してから、全組み合わせのパッケージを生成します。

```sh
./prereq.sh
./build_package_all.sh
```

前提パッケージの構成を変更した場合は、イメージを再生成して下さい。

```sh
./prereq.sh --force
```

どちらのコマンドも、`--distro`、`--release`、`--arch`、`--jobs`による絞り込みに対応します。例えば、ホストと同じUbuntu 24.04 amd64向けだけを生成する場合は、次のように実行します。

```sh
./prereq.sh \
  --distro ubuntu --release 24.04 --arch amd64
./build_package.sh \
  --target deb --distro ubuntu --release 24.04 --arch amd64
```

アーキテクチャは`amd64`、`i386`、`armhf`、`aarch64`などの別名も使用出来ます。`noble`と`resolute`は、それぞれUbuntu 24.04と26.04として扱います。デバッグ情報を残す場合は`--debug`を指定して下さい。`--version`を指定しない場合は、`npx screw-up format -e '{version}' -f`でバージョンを決定します。

生成物は`artifacts/deb/`に保存されます。ファイル名は、例えば`elder-terms-VERSION-ubuntu-24.04-amd64.deb`です。各パッケージについて、controlメタデータ、実行時依存、インストール配置、シンボリックリンクの参照先、ELFアーキテクチャを検査します。その後、対象と同じディストリビューションとアーキテクチャの新しいコンテナへ実際にインストールし、必須ファイルと動的ライブラリの解決を検証します。
