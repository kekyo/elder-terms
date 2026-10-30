# Using SMB

SMB connects to folders shared by Windows or Samba. elder-terms connects to the server's TCP port using SMB2/3. Direct connections are supported, and SSH proxy is available as an experimental feature. GVfs is not used. Set up the share and user access on the server first.

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

You cannot upload to or create folders at the share list `/`, or rename or delete shares themselves. Receiving a share and managing files and folders inside a share are supported.

Disable "Close window when session ends" to keep the window open after disconnection and reconnect using "Reconnect". Canceling an operation in progress may also require reconnection. Interrupted operations are not retried automatically. After reconnecting, refresh the listing and check what the server completed before trying again.

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

## Settings

Connection settings are saved under `[smb]`. A connection can inherit each value from "Connection defaults".

| Setting and INI key | Default | Value |
| --- | --- | --- |
| Address `address` | Empty | Hostname, IPv4, or IPv6. Required to connect. |
| Port `port` | `445` | SMB server TCP port, 1–65535. |
| Domain `domain` | Empty | NTLM authentication domain; empty leaves the domain unspecified. |
| User name `username` | Empty | Initial value in the authentication prompt. |
| Local directory `local_directory` | Empty | Initial left-side directory; empty uses the common transfer destination setting. |
| Remote directory `remote_directory` | `/` | `/` lists shares; `/share-name/…` opens a particular share. |
| Require signing `require_signing` | `true` | Require SMB message signing. |
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

Enable the gateway on the connection's SSH proxy tab to let it connect to the server and port configured on the SMB tab. Authenticate separately to the gateway and the SMB share. The gateway must be able to reach the share server and permit TCP forwarding. See [Using SSH proxy](ssh-proxy.md) for setup.

Share enumeration, browsing, and transfers across multiple shares also work through SSH proxy. Moving between shares does not require authenticating to the gateway again.

SSH encrypts traffic between the client and the gateway. SMB signing and encryption settings apply to traffic between the gateway and the share server.
The SMB connection timeout does not apply while connecting to or authenticating with the gateway. Close the connection window to cancel the wait.

## File operation notes

Modification times can be displayed and set within an SMB share. Remote POSIX permissions, symbolic links, and remote hash calculation are not supported. Links and Windows junctions that lead to another location are not followed.

An upload first creates a temporary file in the share and changes it to its final name after completion. Ordinary renaming does not replace an existing item. If a connection ends before it is known whether the server created a temporary file, the error shows its path. A temporary file with uncertain ownership is not deleted automatically. Reconnect, inspect the displayed path, and delete the file yourself if it is unnecessary.

On a read-only share, you can list and receive files. Attempts to send or change items display the server's refusal.

## Bundled library source and licenses

The deb package includes the complete source of [libsmb2](https://github.com/sahlberg/libsmb2), used for SMB communication, at `/usr/share/doc/elder-terms/licenses/libsmb2/libsmb2-source.tar.gz`. In the same directory, `COPYING` describes each component's license and `LICENCE-LGPL-2.1.txt` contains the full library license.
