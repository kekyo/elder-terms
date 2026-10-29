# Using SMB

SMB connects to folders shared by Windows or Samba. elder-terms connects directly to the server's TCP port using SMB2/3. It does not use GVfs or an SSH proxy. Set up the share and user access on the server first.

## Connect

Create a connection in the launcher and choose "SMB" as its type. Enter the server address and share name on the SMB tab, then save. The share name is one name published by the server. Do not enter a full path such as `\\server\share`.

The connection uses NTLM authentication with a username and password. When the window opens, enter the username and then the password. The configured username is the initial value; if it is empty, the current OS username is shown. A configured domain is used for authentication. Enter the password for each connection. It is never saved in the settings file.

After connecting, the window shows local and shared folders side by side. Select an item and right-click to send or receive files and folders, rename or delete items, or create a folder. Folder transfers and deletion include their contents. If the destination already exists, choose whether to overwrite or skip it.

If the connection ends, select "Reconnect" in the window. Canceling an operation in progress may also require reconnection. Interrupted operations are not retried automatically. After reconnecting, refresh the listing and check what the server completed before trying again.

## Settings

Connection settings are saved under `[smb]`. A connection can inherit each value from "Connection defaults".

| Setting and INI key | Default | Value |
| --- | --- | --- |
| Address `address` | Empty | Hostname, IPv4, or IPv6. Required to connect. |
| Port `port` | `445` | Direct SMB TCP port, 1–65535. |
| Share name `share` | Empty | One share name without a path separator. Required to connect. |
| Domain `domain` | Empty | NTLM authentication domain; empty leaves the domain unspecified. |
| User name `username` | Empty | Initial value in the authentication prompt. |
| Local directory `local_directory` | Empty | Initial left-side directory; empty uses the common transfer destination setting. |
| Remote directory `remote_directory` | `/` | Initial absolute path within the share. |
| Require signing `require_signing` | `true` | Require SMB message signing. |
| Require encryption `require_encryption` | `false` | `true` rejects connections without encryption. `false` still allows encryption required by the server. |
| Connection timeout `connect_timeout_seconds` | `30` | Seconds allowed for name resolution and connection, 1–86400. |
| Idle timeout `idle_timeout_seconds` | `60` | Seconds allowed without network progress, 1–86400. |

For example, these settings open `/Documents` within a share. The password is not recorded.

```ini
[general]
type=smb
name=Team files

[smb]
address=files.example.com
share=team
domain=OFFICE
username=alice
remote_directory=/Documents
require_signing=true
require_encryption=false
```

## File operation notes

Modification times can be displayed and set within an SMB share. Remote POSIX permissions, symbolic links, and remote hash calculation are not supported. Links and Windows junctions that lead to another location are not followed.

An upload first creates a temporary file in the share and changes it to its final name after completion. Ordinary renaming does not replace an existing item. If a connection ends before it is known whether the server created a temporary file, the error shows its path. A temporary file with uncertain ownership is not deleted automatically. Reconnect, inspect the displayed path, and delete the file yourself if it is unnecessary.

On a read-only share, you can list and receive files. Attempts to send or change items display the server's refusal.

## Bundled library source and licenses

The deb package includes the complete source of [libsmb2](https://github.com/sahlberg/libsmb2), used for SMB communication, at `/usr/share/doc/elder-terms/licenses/libsmb2/libsmb2-source.tar.gz`. In the same directory, `COPYING` describes each component's license and `LICENCE-LGPL-2.1.txt` contains the full library license.
