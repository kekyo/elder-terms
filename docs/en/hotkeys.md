# Desktop-specific Hotkey Setup

See the [README hotkey settings](../../README.md#configuring-hotkeys) for normal setup.
On Sway and labwc, the launcher and `etctl setup` save hotkeys in your compositor configuration files.

## Hotkeys on Wayland

On Wayland, the launcher first tries the XDG Global Shortcuts portal. If it cannot register the configured hotkeys,
it detects a Sway or labwc session, saves the hotkeys to the user's compositor configuration, and requests a reload.

Because [Global Shortcuts has been officially supported since GNOME 48](https://release.gnome.org/48/developers/#global-shortcuts),
portal registration is unavailable in Wayland sessions on GNOME 47 or earlier.
Distributions using the standard GNOME desktop are supported starting with [Ubuntu 25.04](https://discourse.ubuntu.com/t/ubuntu-25-04-plucky-puffin-released/59303) and [Debian 13](https://www.debian.org/News/2025/20250809),
which adopted GNOME 48 or later.

The default GNOME Wayland sessions on Ubuntu 24.10 or earlier and Debian 12 or earlier have no automatic registration route through this portal or the Sway/labwc fallback.
Automatic configuration in other Wayland environments also requires the Global Shortcuts portal or the corresponding compositor settings.
In these environments, you may need to enable custom hotkeys (below).

## Manually Enabling Hotkeys

Saving application or connection hotkeys immediately updates the running launcher's registration when the desktop accepts the new bindings.
The Global Shortcuts portal may ask you to approve the changed bindings.
On Sway and labwc, the launcher also synchronizes its compositor bindings and requests a reload when direct registration is unavailable.
Use `etctl setup` to check registration or retry compositor synchronization after a failure.

Hotkeys are normally registered when the launcher starts, so no manual command is needed.
If hotkeys do not work, use `etctl setup` to check registration and retry.

The command checks the running launcher's X11 or Global Shortcuts portal registration result. If it starts the launcher,
the portal may display an approval screen. When registration fails in a Sway or labwc Wayland session,
it saves the current hotkeys to the user's configuration file and requests the compositor to reload them.

Since hotkey hooking is performed within the user session, do not run this command with `sudo`:

```sh
etctl setup
```

The command starts the launcher if needed.
Manually configured desktop shortcuts are managed in your desktop settings and are not changed by saving elder-terms settings.

Automatic setup and repeated command runs replace only the bindings managed by elder-terms. When there is no user compositor configuration,
they preserve the system configuration they find.

- For labwc, this copies the current system configuration into the user file, so later system configuration updates are not inherited automatically.
  See [labwc configuration search and reload rules](https://labwc.github.io/labwc-config.5.html).
- Sway sessions launched with a custom config path may require adding the generated bindings to that config for persistence.
  See the [Sway configuration reference](https://github.com/swaywm/sway/blob/master/sway/sway.5.scd).

Run `etctl unsetup` to remove hotkeys saved by the launcher or `etctl setup` in user Sway or labwc configuration.

It leaves other shortcuts and the configuration files in place.
The command works without a running launcher or graphical session.
In an active Sway or labwc session, it also reloads the compositor; otherwise, reload the compositor or log in again to apply the change.
X11 and Global Shortcuts portal registrations belong to the running launcher and are unaffected by this command.
If configured hotkeys remain enabled, the launcher may recreate Sway or labwc bindings the next time it starts.

```sh
etctl unsetup
```

## Custom Enabling Hotkeys

Below the version in the About panel, you can check the detected hotkey transport and whether `etctl` requests are available.
The transport label describes X11 or portal detection, so it may show "Unavailable" even when Sway or labwc bindings are active.

If `etctl setup` does not support your desktop, assign a desktop shortcut to one of these commands:

```sh
# Open elder-terms launcher window
etctl open-application

# open 'Local Terminal' connection window
etctl open-connection 'Local Terminal'
```

Replace `Local Terminal` with the exact saved connection name displayed in the launcher, without the `.ini` extension.
Quote names containing spaces or shell characters.

The launcher must already be running. Use its existing login autostart entry;
"Background only" is useful without a system tray.

Both processes must run as the same user with the same `XDG_RUNTIME_DIR`,
pointing to an existing private runtime directory.
`etctl` requires neither a display connection nor D-Bus.

It returns 0 when the launcher accepts a request, 1 for a communication or request error,
and 2 for invalid arguments.
Acceptance does not mean the terminal's connection to a remote host has succeeded.

For [labwc](https://labwc.github.io/labwc-actions.5.html), add this binding inside the existing `<keyboard>` section of `~/.config/labwc/rc.xml`,
then reload your configuration with `labwc --reconfigure`:

```xml
<keybind key="C-A-T">
  <action name="Execute" command="etctl open-application" />
</keybind>
```

For [Sway](https://github.com/swaywm/sway/blob/master/sway/sway.5.scd),
add this to your existing configuration and reload it:

```text
bindsym Ctrl+Mod1+t exec etctl open-application
```

For [Hyprland's Lua configuration](https://wiki.hypr.land/Configuring/Basics/Binds/):

```lua
hl.bind("CTRL + ALT + T", hl.dsp.exec_cmd("etctl open-application"))
```

For [Hyprland 0.54 and earlier using hyprlang](https://wiki.hypr.land/0.54.0/Configuring/Binds/):

```text
bind = CTRL ALT, T, exec, etctl open-application
```

Replace an existing binding for the same key combination if necessary.
The launcher and `etctl setup` modify the user's Sway or labwc configuration. Avoid adding the same binding again when configuring shortcuts manually.

Other desktops can use the same commands through their shortcut settings.
Whether a window moves to the foreground remains subject to your desktop environment's policy.
