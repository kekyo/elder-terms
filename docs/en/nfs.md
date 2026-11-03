# Using NFS

NFS connects to directories published by a server, called exports. elder-terms supports browsing and transferring files and folders over NFS v2, v3, and v4.0 using TCP. It uses the system installation of libnfs 5.0.2 or later.

## Connect

Choose "NFS" as the connection type in the launcher, then specify the server address and version on the NFS tab. The default is v3. Select a version accepted by the server; a failed connection never falls back to another version.

Authentication uses AUTH_SYS with numeric user and group IDs. No password is requested. UID and GID default to the current process's IDs and can be set to match server permissions. Up to 16 supplementary groups are supported. UDP, RPCSEC_GSS, NFS v4.1/v4.2, and SSH proxy are outside the supported scope.

For v2 and v3, `/` on the remote side lists exports. Register known export paths when the server does not allow enumeration. Registered paths remain usable when enumeration fails.

For v4.0, the application explores the server's public directory tree. This differs from the v2/v3 export list. For example, if a server exposes v3's `/srv/public` as `/public` in v4, enter `/public` in the dedicated v4 path settings. Known public paths can be registered even when intermediate directories cannot be listed.

Public paths are grouped by their common parent directories. For example, expand `/storage1` at the root to see `/storage1/home` and `/storage1/public`. Registered public paths remain browsable when their parent is not exported.

Open a public root to browse its contents. "Up" returns to its parent in the tree. The path field displays the current location and cannot be edited. `/srv/public:/Documents`, for example, means Documents inside that export.

## Transfers and file operations

Right-click an item to send, receive, rename, delete, or create a folder. Folder transfers and deletion include descendants. Symbolic links are copied as links without recursively following their targets. Permissions and access/modification timestamps are preserved where allowed by the server.

Public roots cannot be renamed or deleted, and items cannot be created in the root list `/`. Unexported parents shown to group public paths cannot be modified either. Nested public roots are also protected. Renaming across public roots is unavailable; receive and send the item separately if needed.

When a destination already exists, choose overwrite or skip. Uploads use a temporary location and install the final name only after all data has been sent. For v2, this temporary location is a folder. If a lost connection prevents ownership or cleanup from being confirmed, the job result retains the temporary path. Reconnect, inspect its contents, and delete it if no longer needed.

Read-only exports allow listing and downloads. Attempts to upload or modify items report the server's refusal. Server policies also determine user access and root squash, which maps root requests to an unprivileged identity.

Disable "Close window when session ends" to keep the window after a disconnect and use "Reconnect". Failed mutations are not replayed automatically. Check the refreshed listing before selecting the operation again.

## Filename encoding

The default is UTF-8. Set "Filename encoding" to CP932, Shift_JIS, EUC-JP, or another supported encoding when appropriate for the server's names. File contents are never converted. CP932 and Shift_JIS are treated as distinct encodings.

Each setting can inherit from global connection defaults or be overridden for one connection. Theme inheritance is not yet supported. Saving a new filename encoding during a connection leaves that connection unchanged; reconnect to apply it. Existing job labels retain their original display names.

Clear a text or numeric entry to display and use its inherited value. Version and enabled/disabled selectors include a default option to restore inheritance. Select "Custom" to edit a multiline list. An empty custom list overrides any inherited entries with an empty list.

Names are not automatically detected, including in directories containing mixed encodings. Undecodable names display their original bytes, for example `[undecodable: FF]`. Different original names remain separate entries even when their display names match. Downloads request a decision when a local filename cannot be formed or would collide. Skipping the item lets other items continue.

Creating or renaming an item fails when the new name cannot be represented exactly. The application does not substitute characters or normalize Unicode names. Link targets are copied only when they can also be converted exactly.

NFS v4 is based on UTF-8. Use a non-UTF-8 encoding only with servers that accept it. Rejected names are never retried in another encoding. See the [string requirements in RFC 7530](https://www.rfc-editor.org/rfc/rfc7530.html#section-12.4).

## Settings

Settings are stored in the `[nfs]` INI section. Separate lists of paths or supplementary groups with semicolons.

| INI key | Default | Meaning |
| --- | --- | --- |
| `address` | Empty | Hostname, IPv4, or IPv6, without a URL or path |
| `version` | `3` | `2`, `3`, or `4.0` |
| `nfs_port` | `0` | Automatic; use 1–65535 for a fixed port |
| `mount_port` | `0` | v2/v3 MOUNT port; 0 discovers it automatically |
| `export_path` | Empty | Initial v2/v3 export; empty shows the list |
| `known_exports` | Empty | Known absolute v2/v3 export paths |
| `discover_exports` | `true` | Request the v2/v3 export list |
| `namespace_path` | Empty | Initial v4.0 public path; empty shows the list |
| `known_namespace_paths` | Empty | Known v4.0 public paths |
| `discover_namespace` | `true` | Explore the v4.0 public tree |
| `filename_encoding` | `UTF-8` | Encoding of filenames and link targets |
| `local_directory` | Empty | Initial local folder; empty uses the common transfer destination setting |
| `uid`, `gid` | `-1` | Current process ID; fixed values range from 0 to 4,294,967,295 |
| `supplementary_gids` | Empty | Up to 16 supplementary group IDs |
| `connect_timeout_seconds` | `30` | Connection deadline in seconds, 1–86400 |
| `request_timeout_seconds` | `60` | Deadline for each request in seconds, 1–86400 |

This example connects to a registered v3 export using CP932 without requesting the export list.

```ini
[general]
type=nfs
name=Archive files
auto_close=false

[nfs]
address=files.example.com
version=3
export_path=/srv/archive
discover_exports=false
filename_encoding=CP932
uid=1000
gid=1000
```

## Connection and size limits

A server requiring a reserved source port needs the application to have permission to use that port. The connection fails without it. Check server export policies and numeric user IDs when troubleshooting access. Reserved-port requirements are described in [nfs-utils exports(5)](https://man7.org/linux/man-pages/man5/exports.5.html).

NFS v2 represents file sizes and offsets with 32 bits. To avoid ambiguous boundary responses, elder-terms rejects uploads of 4,294,967,295 bytes or more and downloads whose reported size equals that boundary. Inconsistent received data and size metadata also cause an error. Use v3 or v4.0 for large files. See [RFC 1094](https://www.rfc-editor.org/rfc/rfc1094.html) for protocol limits.
