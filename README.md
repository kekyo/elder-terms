# elder-terms

'90s, come back in this time.

![elder-terms](./images/elder-terms-120.png)

[![Project Status: Active – The project has reached a stable, usable state and is being actively developed.](https://www.repostatus.org/badges/latest/active.svg)](https://www.repostatus.org/#active)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

----

[(Japanese language is here/日本語はこちら)](./README_ja.md)

> Please note that this English version of the document was machine-translated and then partially edited, so it may contain inaccuracies.
> We welcome pull requests to correct any errors in the text.

## What Is This?

elder-terms is a GTK terminal for serial, TELNET, local shell, SSH, and SFTP
connections, inspired by personal computing in the 1990s.

## Serial Device Selection

The Serial settings page discovers available devices and lets you select one
from a list instead of typing a device path. It supports three identification
modes:

- **Stable device identity** (default) uses `/dev/serial/by-id` to follow the
  same USB serial device when it is moved to another USB port.
- **Physical USB port** uses `/dev/serial/by-path` to follow whichever device
  is attached to the same physical port.
- **Device path** uses the current node, such as `/dev/ttyUSB0` or
  `/dev/ttyACM0`.

The list refreshes when devices are attached or detached and shows the stable
ID, USB serial number, and current device node. A disconnected session keeps
its selected identity and reconnects when the target reappears. In stable-ID
mode, the remembered USB serial number can recover a renamed `by-id` link when
it identifies exactly one device; ambiguous matches are rejected.

The corresponding INI settings are:

```ini
[serial]
device_match_mode=by-id
device=/dev/serial/by-id/usb-Example_Serial_Device_1234-if00
device_usb_serial=1234
```

`device_match_mode` accepts `path`, `by-id`, or `by-path` and defaults to
`by-id`. Existing configurations that only contain a direct `device` path
remain usable.
