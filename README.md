# elder-terms

'90s, come back in this time. This is all we need.

![elder-terms](./images/elder-terms-128.png)

[![Project Status: Active – The project has reached a stable, usable state and is being actively developed.](https://www.repostatus.org/badges/latest/active.svg)](https://www.repostatus.org/#active)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

----

[(Japanese language is here/日本語はこちら)](./README_ja.md)

> Please note that this English version of the document was machine-translated and then partially edited, so it may contain inaccuracies.
> We welcome pull requests to correct any errors in the text.

## What Is This?

elder-terms is a GTK terminal for serial, TELNET, local shell, SSH, and SFTP
connections, inspired by personal computing in the 1990s.

TODO:

## OSC 8 Hyperlink Actions

Hold `Ctrl` and left-click an OSC 8 hyperlink to open it with an external
command. elder-terms matches the complete raw OSC 8 target against ordered
regular-expression rules and runs only the first full match.

When `~/.config/elder-terms/global.ini` has no hyperlink configuration, the
built-in rules open these targets with VS Code:

```text
vscode://file/absolute/path:line
vscode://file/absolute/path:line:column
```

They invoke `code --reuse-window --goto` with the decoded path, line, and
optional column. To use another command or target format, add global rules:

```ini
[hyperlink]
enabled=true

[hyperlink.custom-editor]
regex=^editor://open(?<path>/[^:]+):(?<line>[1-9][0-9]*)$
command=my-editor
arguments=--line;${line};${path|uri-decode};
```

`[hyperlink.<rule-id>]` sections are evaluated in file order. Rule IDs may
contain letters, digits, `-`, and `_`. `regex` must match the entire target.
`command` is a fixed executable name or path and is not expanded.

The semicolon-separated `arguments` values become separate argv elements.
Arguments support `${0}`, numbered captures such as `${1}`, named captures
such as `${path}`, `$$` for a literal dollar sign, and the
`${path|uri-decode}` transformation.

Any explicit hyperlink configuration replaces the built-in VS Code rules. To
disable the feature, use:

```ini
[hyperlink]
enabled=false
```

Hyperlink rules are global only; matching sections in connection or temporary
startup profiles are ignored. Commands are spawned directly without a shell,
so shell syntax is not interpreted and every configured argument remains one
argv element. Because a remote endpoint may supply OSC 8 targets, keep each
regular expression as restrictive as its intended format. Invalid rules,
unknown captures, and invalid URI escaping are not executed.
