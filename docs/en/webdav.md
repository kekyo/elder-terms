# Using WebDAV

## File operations

WebDAV uses the same two-pane browser as SFTP, FTP and FTPS. Select items and use the context menu to send, receive,
rename or delete them. Folder transfers and deletions include their descendants.
New Folder is also available in an empty pane.

A blank size means the server did not report the file's size; it does not mean the file is empty.
Progress uses the available size or item information.
Reported modification times can be displayed, but remote POSIX permissions, symbolic links,
timestamp updates and remote hash calculation are unsupported.

Uploads first use an adjacent temporary file and commit the final name after the server confirms completion.
Transfers ask before replacing an existing destination.
Ordinary Rename refuses an existing destination.
Upload conflicts between a file and a folder fail while preserving the existing contents.
Matching folders are merged, with replacement decisions applied to their files.
A received regular file cannot replace an existing local folder; that conflict leaves the folder and its children intact.

If a connection closes before a mutation's final response, its server-side outcome can be uncertain.
The application reports failure without automatically repeating the mutation.
Refresh the listing before retrying. Check any reported temporary path when cleanup failed or the temporary file's ownership could not be established.

## Authentication

For Basic or Digest authentication, enter the user name in the first connection overlay and the password in the next one.
Passwords are not saved. A server-issued app password can use the same password field.

Auto selects only Basic or Digest.
None sends no authentication credentials. Windows integrated authentication, NTLM, Kerberos and OAuth-only sign-in are outside the supported scope.

## HTTPS certificate confirmation

HTTPS requires TLS 1.2 or newer and normally verifies both the certificate and server name.
You can supply a custom PEM CA bundle. Certificate verification failures reject the connection by default.

With confirmation enabled, an overlay shows the endpoint, reason, Subject, Issuer, validity period and full SHA-256 fingerprint.
Allow for this connection applies only to that window's certificate and reported verification failure.
A changed certificate or failure requires another decision. Exceptions are not written to settings or trust stores and do not carry into a new window.

Cancel is the default. Escape, cancellation and closing the window reject the connection.
Credentials and file data are not sent before approval. Approval can resume a read-only connection check or listing;
it does not automatically replay a failed upload, rename or deletion. The status bar indicates an active certificate exception.

Approval does not relax the TLS version or cipher requirements.
It cannot resolve a failed TLS negotiation or an unreadable CA file.

This confirmation mechanism requires libcurl and the application to use matching OpenSSL versions.
Other TLS backends can use normal certificate verification and rejection.
The [libcurl SSL context callback](https://curl.se/libcurl/c/CURLOPT_SSL_CTX_FUNCTION.html) exposes a backend-specific context,
which is why confirmation has this requirement.

## Supported servers and operations

Support covers standard WebDAV file operations.
Service-specific chunked uploads and login extensions,
Windows integrated authentication, LOCK/UNLOCK, arbitrary property updates, server-side COPY,
synchronization, transfer resumption, client certificates and proxy configuration are outside this scope.

Read-only endpoints can provide listings and downloads.
Access permissions, locks, capacity and server limits can reject individual operations; the response is shown as an error.

Read-only redirects stay within the same origin and base path.
Redirects to another host or port, HTTPS-to-HTTP redirects and automatic write redirects are refused.
Use the published WebDAV endpoint rather than an HTML sign-in URL.

Transfers request uncompressed HTTP responses.
A server that forces a compressed response despite this preference causes the receive operation to fail,
preserving any existing completed file.
 Use a WebDAV endpoint that does not force HTTP compression.

## Settings

| Setting and INI key | Default | Values |
| --- | --- | --- |
| Scheme `scheme` | `https` | `https` or `http`. |
| Server address `address` | Empty | Hostname, IPv4 or IPv6; required to connect. |
| Port `port` | 443 / 80 | HTTPS defaults to 443; HTTP defaults to 80. Range 1–65535. |
| Base path `base_path` | `/` | Absolute path from the published WebDAV URL, including URL escapes. |
| Authentication `authentication` | `auto` | `auto` selects Basic or Digest; explicit `basic`, `digest` and `none` are available. |
| Username `username` | Empty | Initial value in the connection's authentication overlay. |
| Initial local directory `local_directory` | Empty | Uses the shared transfer-directory and download-directory fallback. |
| Initial remote directory `remote_directory` | `/` | Ordinary display path within the base path. |
| CA certificate `ca_file` | System trust | Absolute path to a custom PEM CA bundle, or empty for system trust. |
| Certificate errors `certificate_error_action` | `reject` | Reject the connection, or use `prompt` for confirmation. |
| Connection timeout `connect_timeout_seconds` | 30 | Seconds for DNS, TCP and TLS connection setup; range 1–86400. |
| Idle timeout `idle_timeout_seconds` | 60 | Seconds without network progress; range 1–86400. |

Settings can inherit global values.
The port uses a connection override first, an explicitly configured global port second, and the scheme's default last.
Changing the scheme does not overwrite an explicit port.

For CA certificates, Inherit and System CA certificates are different choices.
An explicit System CA certificates selection replaces an inherited custom CA setting.
HTTPS fields are disabled under HTTP while retaining their entered values.

Connection conditions are read-only while connected.
Open a new connection to apply changes to its saved settings.
Waiting for authentication or certificate confirmation, and intentional pauses for local I/O, do not count as idle network time.

Use URL notation such as `%20` for spaces in `base_path`.
Enter ordinary names such as `/日本語の資料` in `remote_directory`. Queries, fragments, dot segments and invalid escapes are not accepted.

```ini
[general]
type=webdav
name=Team files

[webdav]
scheme=https
address=files.example.com
base_path=/dav/team/
authentication=auto
username=alice
remote_directory=/Documents
certificate_error_action=reject
```
