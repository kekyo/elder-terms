# ホットキーの環境別設定

通常の設定方法は[READMEのホットキー設定](../../README_ja.md#ホットキーの設定)を参照してください。
Swayとlabwcでは、ランチャーと`etctl setup`が利用者のコンポジター設定ファイルにホットキーを保存します。

ランチャーの「About」で、検出したホットキーの方式と`etctl`の利用可否を確認できます。画像はX11環境での表示例です。

![X11のホットキー検出とetctlの利用可否](../../images/about-dialog.png)

## Wayland環境でのホットキー

Waylandセッションでは、ランチャーが最初にXDG Global Shortcutsポータルへの登録を試みます。
設定済みホットキーを登録出来なかった場合は、Swayまたはlabwcのセッションを検出し、利用者のコンポジター設定にホットキーを保存して再読み込みを要求します。

[GNOME 48からGlobal Shortcutsが正式にサポートされた](https://release.gnome.org/48/developers/#global-shortcuts)ため、
GNOME 47以前のWaylandセッションではポータルによる登録は使用出来ません。
標準のGNOMEデスクトップを使用するディストリビューションでは、GNOME 48を採用した[Ubuntu 25.04](https://discourse.ubuntu.com/t/ubuntu-25-04-plucky-puffin-released/59303)および[Debian 13](https://www.debian.org/News/2025/20250809)以降が対応します。

Ubuntu 24.10以前またはDebian 12以前の標準GNOME Waylandセッションでは、ポータルとSway/labwcのどちらによる自動登録も利用出来ません。
その他のWayland環境での自動設定も、Global Shortcutsポータルまたは対応するコンポジター設定が必要です。
これらの環境では、カスタムホットキー有効化が必要かもしれません（後述）。

## 手動でのホットキー有効化

アプリケーションまたは接続のホットキーを保存すると、デスクトップが新しいキー設定を受け付けた場合、動作中のランチャーの登録が即時更新されます。
Global Shortcutsポータルを使う環境では、変更後のキー設定の承認を求められることがあります。
直接登録出来ないSwayとlabwcでは、ランチャーが管理するコンポジターのキー設定も同期して再読み込みを要求します。
登録結果の確認や、失敗後の再試行には`etctl setup`を使用出来ます。

通常はランチャー起動時に自動登録されるため、手動でコマンドを実行する必要はありません。
ホットキーが動作しない場合は、`etctl setup`で登録結果を確認して再試行できます。

このコマンドは、動作中のランチャーによるX11またはGlobal Shortcutsポータルへの登録結果を確認します。
ランチャーを起動する場合は、ポータルの承認画面が表示されることがあります。
SwayまたはlabwcのWaylandセッションで登録に失敗した場合は、利用者の設定ファイルに現在のホットキーを保存し、コンポジターに再読み込みを要求します。

ホットキーは利用者のデスクトップへ登録するため、`sudo`を使わずに実行してください。

```sh
etctl setup
```

ランチャーが起動していなければ、このコマンドが起動します。
デスクトップ側で手動設定したショートカットは、elder-termsの設定保存では変更されません。必要に応じてデスクトップ側で更新して下さい。

Swayとlabwcでは既存の利用者設定を残し、起動時の自動設定やコマンドの再実行時はelder-termsが追加したキー設定だけを置き換えます。
利用者設定がまだない場合は、見つかったシステム設定を基にします。

- labwcではその時点のシステム設定を利用者設定に複製するため、以後のシステム設定の更新は自動的には反映されません
  [(labwcの設定検索と再読み込み)](https://labwc.github.io/labwc-config.5.html)。
- Swayを独自の設定パスで起動している場合、次回以降も有効にするには生成されたキー設定をその設定ファイルへ追加する必要があります
  [(Swayの設定仕様)](https://github.com/swaywm/sway/blob/master/sway/sway.5.scd)。

`etctl unsetup`は、ランチャーまたは`etctl setup`がSwayまたはlabwcの利用者設定に保存したホットキーを解除します。

ランチャーや画面セッションが動作していなくても実行出来ます。
Swayまたはlabwcのセッション中ならコンポジターにも再読み込みを要求します。
それ以外の場合、変更を反映するにはコンポジターを再読み込みするか、再ログインして下さい。
X11とGlobal Shortcutsポータルの登録は動作中のランチャーが管理するため、このコマンドでは解除しません。
設定済みホットキーが有効なままなら、次にランチャーが起動したときにSwayまたはlabwcのキー設定が再作成される場合があります。

```sh
etctl unsetup
```

## カスタムホットキー有効化

「情報」パネルのバージョン表示の下で、ホットキーの検出方式と `etctl` の受付可否を確認出来ます。
検出方式はX11またはポータルの状態を示すため、Swayやlabwcのキー設定が有効でも「利用不可」と表示される場合があります。

`etctl setup`が対応しない環境では、デスクトップ側のショートカットに次のコマンドを登録してください。

```sh
# elder-termsランチャーを開く
etctl open-application

# 指定した'Local Terminal'接続を開く
etctl open-connection 'Local Terminal'
```

`Local Terminal` は、ランチャーに表示される保存済み接続名に置き換えて下さい。
`.ini` 拡張子は付けません。空白やシェルの特殊文字を含む名前は引用して下さい。

ランチャーは起動済みである必要があります。既存のログイン時自動起動を利用して下さい。
システムトレイがない環境では「バックグラウンドのみ」が便利です。

ランチャーとコマンドは同じユーザーで実行し、同じ `XDG_RUNTIME_DIR` を使用します。
この変数は、所有者だけがアクセス出来る既存のランタイムディレクトリを指す必要があります。
`etctl` 自体には画面接続もD-Busも不要です。

終了コードは要求を受理した場合に0、通信や要求のエラーに1、引数の誤りに2となります。
要求の受理は、接続先ホストへの接続成功を意味するものではありません。

[labwc](https://labwc.github.io/labwc-actions.5.html)を使用する場合は、既存の `~/.config/labwc/rc.xml` の `<keyboard>` 内に以下を追加し、`labwc --reconfigure` で設定を再読み込みします。

```xml
<keybind key="C-A-T">
  <action name="Execute" command="etctl open-application" />
</keybind>
```

[Sway](https://github.com/swaywm/sway/blob/master/sway/sway.5.scd)を使用する場合は、既存の設定に以下を追加して再読み込みします。

```text
bindsym Ctrl+Mod1+t exec etctl open-application
```

[HyprlandのLua設定](https://wiki.hypr.land/Configuring/Basics/Binds/)を使用する場合は、以下の形式です。

```lua
hl.bind("CTRL + ALT + T", hl.dsp.exec_cmd("etctl open-application"))
```

[Hyprland 0.54以前のhyprlang設定](https://wiki.hypr.land/0.54.0/Configuring/Binds/)を使用する場合は、以下の形式です。

```text
bind = CTRL ALT, T, exec, etctl open-application
```

同じキーに既存の割り当てがある場合は置き換えて下さい。
ランチャーと`etctl setup`はSwayとlabwcの利用者設定ファイルを変更します。手動で設定する場合も、同じキー設定を重複して追加しないで下さい。

他のデスクトップでも、ショートカット設定から同じコマンドを使用出来ます。
ウインドウが前面に移動するかどうかは、デスクトップ環境の方針に従います。
