# Connection Icons and Dock Integration

Open "Choose icon…" on a connection's "General" tab to select a preset or a PNG/JPEG file.
Icons are specific to each connection and do not inherit from "Connection defaults".

![Connection icon picker with presets and an image file option](../../images/connection-icon-picker.png)

## Image File Requirements

Images are read from their original paths. Keep the file available for subsequent launches.
Both the file and decoded RGBA image are limited to 64 MiB, with each dimension limited to 16384 pixels.
Images retain their aspect ratio within a square, with transparent padding.

If an image cannot be read, the default icon is displayed and the connection can still start.
Check the settings dialog for the reason. Missing files and unknown preset IDs do not automatically clear the saved selection.

## Dock Display

Windows opened from the same saved connection form one dock group.
Renaming a connection keeps the same group; duplicating it creates a separate group.
Launching unsaved edits creates a temporary group. Pinning and relaunching that group opens the launcher.

Dock presentation depends on the desktop environment; see the [GTK icon documentation](https://docs.gtk.org/gtk3/method.Window.set_icon_list.html).
GNOME can show the previous image for a few seconds while its [cache updates](https://github.com/GNOME/gnome-shell/blob/46.0/src/shell-app-cache.c).
If the old image remains, reopen the connection window.

Dock integration files are saved under `$XDG_DATA_HOME/applications/` and `$XDG_DATA_HOME/icons/hicolor/256x256/apps/`.
When `XDG_DATA_HOME` is unset, the usual base directory is `~/.local/share/`.
These files remain after deleting a connection to preserve pinned entries and icons for running windows.
A pinned icon cannot launch a deleted connection. Unpin entries you no longer need.

## INI Settings

Set `icon` in the connection INI file's `[general]` section.

| Value | Image |
| --- | --- |
| `default` or omitted | elder-terms default |
| `noto:emoji_u1f44d` | Example Noto Color Emoji preset |
| `material:action/home` | Black Material Icons glyph with a white outline |
| `material:white:action/home` | White Material Icons glyph with a black outline |
| `file:/home/user/Pictures/server.png` | PNG or JPEG specified by absolute path |

See [artwork sources and licenses](../ja/connection-icon-artwork.md) for attribution.
