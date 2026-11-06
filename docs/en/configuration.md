# Configuring INI Files

To edit a saved connection, right-click it in the launcher and select "Edit in text editor".
See the [README](../../README.md#configuration-structure) for file locations and the relationship between defaults and connection settings.
This reference covers the settings introduced in the README.

## Inheritance

Omitting a connection setting inherits connection defaults, followed by the built-in default.
Application-wide settings belong in `global.ini`.
See the individual guides for settings specific to a connection, including icons, macros, and link rules.

An empty value and an omitted key can have different meanings. For example, `font_families=` explicitly selects the built-in font list.

## The New Window Connection

Set the name of a saved terminal connection in the `[general]` section of `global.ini`.

```ini
[general]
new_window=Local terminal
```

Renaming the connection in the launcher updates this setting.
Deleting it or changing it to a file transfer connection clears the selection.
Without a selection, the terminal uses its default settings. Startup fails if the selected connection is missing, unreadable, or not a terminal connection.

## Windows and Notifications

```ini
[general]
auto_close=false
compact_mode=true
show_border=true
border_width=4
indicator_color=#3584E4
indicator_off_color=#808080
receive_effect=strong

[terminal]
visual_bell=disabled
```

| Key | Values |
| --- | --- |
| `auto_close` | `true` closes the window when a session ends normally; `false` keeps it open |
| `compact_mode` | `true` enables compact mode; built-in default: `false` |
| `show_border` | `true` shows borders; built-in default: `false` |
| `border_width` | 1–1000 pixels; built-in default: 4 |
| `indicator_color`, `indicator_off_color` | An RGB color in `#RRGGBB` format, or `default` for the original green and gray respectively |
| `receive_effect`, `visual_bell` | `disabled`, `weak`, `medium`, or `strong`; built-in default: `medium` |

Invalid indicator colors produce a warning and use a valid inherited value or the built-in default.
For `icon`, see [connection icon settings](connection-icons.md#ini-settings).

## Terminal Controls

```ini
[terminal]
accessibility_enabled=true
return_code=crlf
send_break_key=F12
scrollback_lines=20000
bell_sound=/home/user/.local/share/sounds/terminal-bell.oga
block_copy_trim_trailing_spaces=false
```

| Key | Values |
| --- | --- |
| `accessibility_enabled` | `true` exposes terminal text to screen readers; built-in default: `false` |
| `return_code` | `auto`, `cr`, `lf`, or `crlf` |
| `send_break_key` | The BREAK shortcut key combination; empty disables it |
| `scrollback_lines` | 1000–100000 lines; built-in default: 10000 |
| `bell_sound` | `default` uses the desktop beep; a custom sound is an absolute path to an Ogg Vorbis or WAV (PCM) file |
| `block_copy_trim_trailing_spaces` | `false` preserves trailing spaces in block copies; built-in default: `true` |

See [selection and copying](text-selection.md) for whitespace and clipboard behavior.

## Fonts and Character Width

```ini
[terminal]
font_size_dip=14.5
ambiguous_width=wide
font_families=DejaVu Sans Mono;Noto Sans Mono CJK JP;Monospace;
```

Text size uses DIPs, based on 1/96 inch. The built-in default is 12 DIP.
For `ambiguous_width`, `narrow` means one column, `wide` means two, and `font` means font dependent. The built-in default is `narrow`.
Save and reopen the terminal after changing the width policy.
Invalid width values produce a warning and use a valid inherited value or the built-in default.

`font_families` is a semicolon-delimited list in priority order. Omit the key to inherit, or use `font_families=` to select `Noto Sans Mono`, then `Monospace`.
A connection's list replaces the entire inherited list.
Names are trimmed and must be non-empty and unique, with no commas or control characters.
Escape semicolons and backslashes inside names as `\;` and `\\`, following the [GLib KeyFile rules](https://docs.gtk.org/glib/struct.KeyFile.html).
Invalid lists produce a warning and use a valid inherited value or the built-in default.

## Background and Color Correction

This example combines a vertical cursor, a blurred background, a background image, and text contrast correction.

```ini
[terminal]
cursor_shape=bar
cursor_blink=false
background_mode=blur
background_opacity=40
cell_background_mode=opaque
background_tint_color=#203040
background_tint_opacity=20
background_image=/home/user/Pictures/terminal.png
background_image_opacity=70
background_image_fit=cover
color_filter=oklch
```

Opacity values range from 0 to 100%. Specify an absolute path to a PNG or JPEG image.
Both the file and decoded RGBA image are limited to 64 MiB, with each dimension limited to 16384 pixels.
Keep the image at the same path so it remains available on the next launch.
See the [README background settings](../../README.md#configuring-the-cursor-and-background) for transparency and blur requirements.

`cell_background_mode` controls transparency for backgrounds assigned to individual terminal cells.
The default, `transparent`, applies `background_opacity` to cell backgrounds as well.
`opaque` makes backgrounds specified with 16 colors, 256 colors, or RGB opaque. This includes explicit colors matching the default background, blank cells, and backgrounds left after erasing text.
The default background keeps its existing opacity. SGR 49, the escape sequence that restores the default background, also restores normal transparency.
For inverse text, the color moved into the background becomes opaque. Color correction is applied before displaying that color.

This setting works with background modes and images. Selection, cursor, and IME opacity retain their own settings.
An unset value inherits the connection defaults. Applying a change during a session also updates existing text and history.

`color_filter` accepts the following values. See the [README color filters](../../README.md#reversing-background-and-text-polarity) for how the modes look.

| Value | Correction |
| --- | --- |
| `none` | No correction |
| `oklch` | Improve visibility |
| `reverse-lightness` | Reverse lightness (A) |
| `reverse-contrast` | Reverse contrast ratio (B) |
| `reverse-roles` | Reverse text and background (C) |
| `reverse-cam16` | Reverse appearance (D) |
| `reverse-palette` | Reverse optimized palette (E) |

Color discrimination assistance can be combined with a color filter.

```ini
[terminal]
color_vision=deutan
color_vision_strength=60
color_filter=oklch
```

`color_vision` accepts `none`, `protan`, `deutan`, or `tritan`.
`color_vision_strength` ranges from 0 to 100; 0 disables assistance. The default strength when a profile is selected is 100.
See [terminal link actions](terminal-links.md) for link display settings.

## Local Startup Command

```ini
[local]
command_line=/bin/sh -lc 'exec tmux new-session -A -s elder-terms'
```

Commands run without a shell. Explicitly start a shell as in this example when you need pipes, redirection, or environment-variable expansion.
An empty value inherits the default. The built-in default uses `SHELL`, or `/bin/sh` if `SHELL` is unset or empty.

## Serial Connections

```ini
[serial]
device_match_mode=by-id
device=/dev/serial/by-id/usb-Example_Serial_Device_1234-if00
device_usb_serial=1234
carrier_detect=ignore
```

`device_match_mode` accepts `path`, `by-id`, or `by-path`; omitting it selects `by-id`.
`device_usb_serial` is supplementary information saved when the settings dialog can obtain a USB serial number.
If multiple candidates share that number, the connection waits.
Existing INI files with a device path but no `device_match_mode` continue to use that path.
`carrier_detect=ignore` selects "Ignore (do not monitor)" for the connection monitoring signal.

## Sending and Pasting Text

```ini
[transfer]
text_send_follow_return_code=true
```

`true` converts newlines in text sends, pastes, and macro sends according to `return_code`; `auto` uses CR.
`false` preserves the original CR, LF, and CRLF sequences. The built-in default is `true`.

## Connection Types and Automation

See the guides for [FTP/FTPS](ftp-sftp.md), [HTTP](http.md), [WebDAV](webdav.md), [SMB](smb.md), [NFS](nfs.md), and [SSH proxy](ssh-proxy.md).
Examples of macros and custom link rules are in the [terminal links and macros guide](terminal-links.md).
