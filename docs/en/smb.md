# Using SMB

SMB connects to folders shared by Windows or Samba. elder-terms supports SMB1, SMB2, and SMB3. Automatic selection of SMB2/3 is the default. SSH proxy is also available for SMB2/3 as an experimental feature. GVfs is not used. Set up the share and user access on the server first.

## Connect

Create a connection in the launcher and choose "SMB" as its type. Enter the server address on the SMB tab, then save. After connecting, `/` on the remote side lists the server's file shares. A share name identifies a folder published by the server.

The connection uses NTLM authentication with a username and password. When the window opens, enter the username and then the password. The configured username is the initial value; if it is empty, the current OS username is shown. A configured domain is used for authentication. Enter the password for each connection. It is never saved in the settings file.

Open a share to browse its files and folders. You can also expand the tree, enter a path, or move to the parent directory. Selecting "Up" at a share's root returns to the share list.

| Remote path | Location |
| --- | --- |
| `/` | Server's file share list |
| `/team` | Root of the team share |
| `/team/Documents` | Documents folder in the team share |

There is no separate share name setting. To open a particular share initially, set the remote directory to `/share-name/…`. The `\\server\share` format is not used. For existing connections that used the former share name setting, update the remote directory to include the share name at the beginning. Old settings are not migrated automatically.

The window shows local and remote folders side by side. Select an item and right-click to send or receive files and folders, rename or delete items, or create a folder. Folder transfers and deletion include their contents. If the destination already exists, choose whether to overwrite or skip it.

Before sending, select one folder or regular file in Remote; before receiving, select one in Local. Selecting a folder transfers into that folder. Selecting a file transfers into its containing folder. Send and Receive are unavailable when no destination is selected, a symbolic link is selected, or multiple items are selected. The path field is not used as the destination.

You cannot upload to or create folders at the share list `/`, or rename or delete shares themselves. Selecting a share in the list lets you send into it. Receiving a share and managing files and folders inside a share are also supported.

Disable "Close window when session ends" to keep the window open after disconnection and reconnect using "Reconnect". Canceling an operation in progress may also require reconnection. Interrupted operations are not retried automatically. After reconnecting, refresh the listing and check what the server completed before trying again.

## Select an SMB version

Choose Automatic, SMB1, SMB2, or SMB3 on the SMB tab. "Automatic (SMB 2/3)" does not include SMB1. Authentication or connection failures never trigger a retry with a different version. The window title and transfer jobs display the negotiated version.

SMB1 supports direct IPv4 connections on port 445, or port 139 with the server's NetBIOS name. This is the name used by the server, consisting of 1–15 ASCII letters, digits, hyphens, or underscores. IPv6, arbitrary ports, SSH proxy, and required signing or encryption are unsupported due to the [libdsm public connection API](https://github.com/videolabs/libdsm/blob/0f3edc6df6ab5950f2550d453035351440f71e31/include/bdsm/smb_session.h). Unsupported combinations produce validation errors in the settings.

The built-in signing default is disabled for SMB1. An explicit or inherited signing requirement is preserved and reported as an error. Requiring encryption with SMB2 is also a validation error.

SMB1 filenames use Unicode, including Japanese characters and emoji. The runtime environment requires the C.UTF-8 locale. There is no code page setting such as CP932, and servers supporting only legacy OEM strings are outside the supported scope. The bundled libdsm includes a patch to use UTF-16 in its [conversion code](https://github.com/videolabs/libdsm/blob/0f3edc6df6ab5950f2550d453035351440f71e31/src/smb_utils.c). An unconvertible name makes the listing fail instead of displaying a successful listing with missing entries.

## Find a host with IP scan

Use the "IP scan" button beside the address field on the SMB tab to find a host.
A check mark in SMB (445) identifies a candidate that accepted a TCP connection.
Connect to the server to verify SMB support, authentication, and access to its shares.

Double-click a row to fill the address field with its resolved host name, or its numeric IP address.
The port, domain, user name, remote directory, signing and encryption settings are preserved.
Check the connection settings and save them. The scan button is unavailable in the read-only
settings of an active connection.

See [Finding a Host with IP Scan](../../README.md#finding-a-host-with-ip-scan)
for scan ranges and name lookup shared with the other connection types.

## When shares cannot be listed or opened

A listed share does not necessarily permit access by your user. If access is denied, an error is shown for that share and other shares remain available.

Even when the server refuses share enumeration, a known share may still be accessible. Enter a path such as `/team/Documents` directly or set it as the connection's remote directory. Opening a particular share does not require successful enumeration.

An incomplete enumeration result is reported as an error. The bundled libsmb2 enumeration API does not let callers specify a continuation position, so partial results are not displayed as a complete list. You can still specify a known share by its path. See the [libsmb2 enumeration API](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/include/smb2/libsmb2-share-enum.h) and [implementation](https://github.com/sahlberg/libsmb2/blob/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64/lib/smb2-share-enum.c).

SMB1 also reports incomplete share listings as errors. If this happens, for example on a server with many shares, enter a known share by its path. The bundled [libdsm share enumeration](https://github.com/videolabs/libdsm/blob/0f3edc6df6ab5950f2550d453035351440f71e31/src/smb_share.c) includes a fix to discard partial results.

## Settings

Connection settings are saved under `[smb]`. A connection can inherit each value from "Connection defaults".

| Setting and INI key | Default | Value |
| --- | --- | --- |
| Version `version` | `auto` | `auto` selects SMB2/3; `1`, `2`, or `3` restricts the version. |
| Address `address` | Empty | Hostname, IPv4, or IPv6. SMB1 requires IPv4. Required to connect. |
| Port `port` | `445` | 1–65535 for SMB2/3; 445 or 139 for SMB1. |
| NetBIOS name `netbios_name` | Empty | Server NetBIOS name, required for SMB1 on port 139. |
| Domain `domain` | Empty | NTLM authentication domain; empty leaves the domain unspecified. |
| User name `username` | Empty | Initial value in the authentication prompt. |
| Local directory `local_directory` | Empty | Initial left-side directory; empty uses the common transfer destination setting. |
| Remote directory `remote_directory` | `/` | `/` lists shares; `/share-name/…` opens a particular share. |
| Require signing `require_signing` | `false` for SMB1; otherwise `true` | Require SMB message signing. |
| Require encryption `require_encryption` | `false` | `true` rejects connections without encryption. `false` still allows encryption required by the server. |
| Connection timeout `connect_timeout_seconds` | `30` | Seconds allowed for SMB connection, 1–86400. Does not apply to SSH gateway connection or authentication. |
| Idle timeout `idle_timeout_seconds` | `60` | Seconds allowed without network progress, 1–86400. |

For example, these settings initially open the Documents folder in the team share. The password is not recorded.

```ini
[general]
type=smb
name=Team files

[smb]
address=files.example.com
domain=OFFICE
username=alice
remote_directory=/team/Documents
require_signing=true
require_encryption=false
```

## Connect through SSH proxy (experimental)

Available for SMB2/3. Enable the gateway on the connection's SSH proxy tab to let it connect to the server and port configured on the SMB tab. Authenticate separately to the gateway and the SMB share. The gateway must be able to reach the share server and permit TCP forwarding. See [Using SSH proxy](ssh-proxy.md) for setup.

Share enumeration, browsing, and transfers across multiple shares also work through SSH proxy. Moving between shares does not require authenticating to the gateway again.

SSH encrypts traffic between the client and the gateway. SMB signing and encryption settings apply to traffic between the gateway and the share server.
The SMB connection timeout does not apply while connecting to or authenticating with the gateway. Close the connection window to cancel the wait.

## File operation notes

Modification times can be displayed and set with SMB2/3; SMB1 supports display only. Remote POSIX permissions, symbolic link creation, and remote hash calculation are unsupported. SMB2/3 does not follow links or Windows junctions. The SMB1 [attribute API](https://github.com/videolabs/libdsm/blob/0f3edc6df6ab5950f2550d453035351440f71e31/include/bdsm/smb_stat.h) cannot identify links, so items are treated as the files or directories returned by the server.

An SMB2/3 upload first creates a temporary file in the share and changes it to its final name after completion. Ordinary renaming does not replace an existing item. If a connection ends before it is known whether the server created a temporary file, the error shows its path. A temporary file with uncertain ownership is not deleted automatically. Reconnect, inspect the displayed path, and delete the file yourself if it is unnecessary.

SMB1 stores uploads in a dedicated temporary directory because the [libdsm file API](https://github.com/videolabs/libdsm/blob/0f3edc6df6ab5950f2550d453035351440f71e31/include/bdsm/smb_file.h) cannot request exclusive file creation. An overwrite moves the existing file to `previous` inside that directory, then moves the new file to its destination. Replacement is not atomic: the destination is temporarily absent. If restoration after interruption has an uncertain result, the backup is retained and the error identifies the temporary directory. Canceling communication does not undo server-side changes.

On a read-only share, you can list and receive files. Attempts to send or change items display the server's refusal.

## Bundled library source and licenses

The deb package includes the complete source of [libsmb2](https://github.com/sahlberg/libsmb2), used for SMB communication, at `/usr/share/doc/elder-terms/licenses/libsmb2/libsmb2-source.tar.xz`. In the same directory, `COPYING` describes each component's license and `LICENCE-LGPL-2.1.txt` contains the full library license.

The corresponding source of [libdsm](https://github.com/videolabs/libdsm), used for SMB1, is included at `/usr/share/doc/elder-terms/licenses/libdsm/libdsm-source.tar.xz`. The same directory contains the license in `COPYING` and the applied bug fixes in `libdsm-correctness.patch`.

The bundled libdsm and this project's modifications are provided under LGPL 2.1 or later. Original copyright notices are preserved, and each modified file records the changes and their dates. Elder-terms itself remains under the MIT license. Following Sections 2, 4, and 6 of [LGPL 2.1](https://opensource.org/license/lgpl-2-1), the package includes the modified source and build definitions and allows replacement of the shared library.

Each library's source is provided as a separate tar.xz archive. Use tar and xz to extract it. On Debian-based distributions, install `xz-utils`.

You can rebuild libdsm using only the bundled source. Install a C compiler, Meson, Ninja, pkg-config, the libtasn1 development files, and `asn1Parser`. On Debian-based distributions, the libtasn1 packages are `libtasn1-6-dev` and `libtasn1-bin`.

```sh
mkdir libdsm-rebuild
cd libdsm-rebuild
tar -xJf /usr/share/doc/elder-terms/licenses/libdsm/libdsm-source.tar.xz
meson setup build source --prefix="$PWD/sdk" --libdir=lib \
  -Ddefault_library=shared -Dbinaries=false
meson compile -C build dsm
meson install -C build --no-rebuild --tags=runtime,devel
```

The rebuilt library is at `libdsm-rebuild/sdk/lib/libdsm.so.3`. You can use a modified version that preserves public interface compatibility. For the standard deb package, close elder-terms before replacing `/usr/lib/elder-terms/libdsm.so.3`. No additional terms prohibit modification for your own use or reverse engineering to debug those modifications.
