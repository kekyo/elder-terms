# FTP/SFTP details

## Using FTP with TLS (FTPS)

To use [FTPS](https://everything.curl.dev/ftp/ftps.html), choose FTPS (explicit TLS)
or FTPS (implicit TLS) under Encryption in the FTP tab. The equivalent INI values
are `tls_mode=explicit` and `tls_mode=implicit` in the `[ftp]` section.
Explicit FTPS upgrades the control connection with AUTH TLS and defaults to port
21. Implicit FTPS starts TLS immediately and defaults to port 990. An explicitly
configured port, including one inherited from global settings, takes precedence.
FTPS requires TLS 1.2 or newer for both control and data connections by default.
It verifies the server certificate and host name and rejects validation failures.
For a private CA, set `ca_file=/absolute/path/company-ca.pem`; an empty value uses
the system CA store. Both FTPS modes support active and passive data connections,
IPv4 and IPv6, and servers requiring TLS session reuse for data transfers.
FTPS never falls back to plain FTP.

Active FTPS requires libcurl 8.0.0 or newer. Earlier supported libcurl versions,
including Debian 12's 7.88.1, have an [active data TLS defect](https://curl.se/ch/8.0.0.html)
and are rejected before connecting in Active mode. Select Passive on those
systems or update libcurl.

Active FTP and FTPS open a fresh control connection for each operation to avoid an
[upstream data-handshake race](https://github.com/curl/curl/blob/curl-8_14_1/lib/ftp.c#L3647).
The previous connection is closed before the next operation starts.
Passive FTPS also opens fresh control
connections with libcurl versions before 8.9.0 when TLS 1.3 is allowed: older
upload shutdown can lose the session ticket needed by the next data connection.
See the [upstream resumption report](https://github.com/curl/curl/issues/4654) and
[8.9.0 shutdown changes](https://curl.se/ch/8.9.0.html). Passive retains connection
reuse with newer libcurl, or when the maximum TLS version is explicitly 1.2 or below.

The FTP tab also offers TLS version bounds, AUTH
preference, compatibility, CA certificates, cipher lists, and the certificate
failure policy. Scroll down to reach the TLS details. Choose Apply to save the
connection before connecting.

Settings can inherit connection defaults or use a connection-specific value.
For CA and cipher fields, choose Custom to enter a value; the CA field also has
a file picker. Choose System CA certificates or Library default to override an
inherited custom value with the normal default. Invalid setting formats and
reversed version bounds prevent Apply until corrected. Cipher availability is
checked when connecting. Connection-time TLS settings are
read-only in an open transfer window; edit the saved connection and reconnect
to apply changes.

The following INI keys correspond to the TLS controls in the FTP tab:

| Key | Default | Choices |
| --- | --- | --- |
| `tls_min_version` | `1.2` | `1.0`, `1.1`, `1.2`, `1.3` |
| `tls_max_version` | `default` | `default` (backend maximum), or the same version values |
| `tls_auth_order` | `tls` | `tls`, `ssl`, `default` (explicit FTPS only) |
| `tls_compatibility` | `standard` | `standard`, `openssl_legacy` |
| `tls_cipher_list` | empty | OpenSSL cipher expression for TLS 1.2 and earlier |
| `tls13_cipher_list` | empty | Colon-separated TLS 1.3 cipher names |

Set both version bounds to the same value to require that version. For example,
`tls_min_version=1.0`, `tls_max_version=1.0`, and
`tls_compatibility=openssl_legacy` enable a TLS 1.0-only legacy connection when
supported by the installed OpenSSL. The compatibility option explicitly uses
[OpenSSL security level 0](https://docs.openssl.org/3.0/man3/SSL_CTX_set_security_level/)
for this connection. It does not change certificate validation or other
connections. The standard setting keeps the backend's security policy, which
can reject old protocols even when the requested version range includes them.

SSLv2 and SSLv3 are unsupported. AUTH SSL is an alternative FTP command and does
not select SSLv3; the configured TLS bounds still apply. Cipher expressions and
the legacy compatibility option require an OpenSSL-backed libcurl. Unknown-only
cipher lists and unsupported settings fail the connection; they are not silently
ignored. Anonymous and unencrypted cipher suites are excluded, including the
[integrity-only TLS 1.3 suites](https://docs.openssl.org/3.5/man3/SSL_CTX_set_cipher_list/)
introduced in OpenSSL 3.5. Use the
compatibility setting to change the security level; `@SECLEVEL` directives in
cipher expressions are rejected. See the official
[libcurl TLS version](https://curl.se/libcurl/c/CURLOPT_SSLVERSION.html) and
[cipher selection](https://curl.se/libcurl/c/CURLOPT_SSL_CIPHER_LIST.html) documentation
for backend limitations.

## FTPS Certificate Errors

Under Certificate validation failure, choose Reject connection (the default)
or Confirm in an overlay. The corresponding INI values are
`certificate_error_action=reject` and `certificate_error_action=prompt`.
The confirmation overlay shows the
requested server and port, control/data channel, validation reason, certificate
subject and issuer, validity dates, and full SHA-256 fingerprint. Cancel is the
default action; Escape and closing the window also refuse the connection.

Allowing an exception applies only to the displayed certificate and failure in
this connection window. It is not saved to the connection file or system trust
store. Another failure or a changed certificate requires confirmation again;
a new window has no previous approvals. The status bar indicates when an
exception is active. A valid certificate does not show this overlay.

Initial login can continue after approval. If a certificate fails during a file
operation, that operation remains failed after approval; retry it explicitly.
The application does not automatically repeat uploads, renames, or deletions.
Confirmation covers untrusted issuers, certificate validity dates, and host-name
failures. Other certificate errors are rejected. Certificate approval does not
relax TLS versions, cipher requirements, or cryptographic policy failures. Use the
separate legacy compatibility setting when an old server requires it.

Certificate confirmation requires libcurl and the application to use the same
OpenSSL version. Other TLS backends can use the default rejection policy but
cannot use this confirmation mode. This follows the backend-specific context
contract of [libcurl's TLS context callback](https://curl.se/libcurl/c/CURLOPT_SSL_CTX_FUNCTION.html).

## FTP Data Connections

With FTPS, the control connection is encrypted, so a firewall or NAT device
cannot inspect FTP commands to discover the data ports. Configure the server's
passive port range or the client's active connection route explicitly when
needed. See the [FTPS connection guide](https://everything.curl.dev/ftp/ftps.html).

FTP uses a control connection and creates a separate data connection
for each directory listing or file transfer. The `Data connection mode`
setting chooses which side initiates that data connection:

- `Passive (recommended)`: elder-terms connects to a port selected by the
  server. It tries `EPSV` first and, on IPv4, falls back to `PASV` when the
  server does not support `EPSV`. This normally works best through client-side
  NAT and firewalls because both connections are outbound. For `PASV`, the
  advertised host address is ignored and the control-connection peer is used,
  following the FTP security guidance in
  [RFC 2577](https://www.rfc-editor.org/rfc/rfc2577).
- `Active`: elder-terms listens on a local port and the server connects back to
  it. It tries `EPRT` first and, on IPv4, falls back to `PORT`. An inbound data
  connection must reach the client, so firewall and NAT configuration may be
  required. A data connection from a host other than the control-connection
  peer is rejected.

`EPSV` and `EPRT` are the IPv4/IPv6-capable extended commands defined by
[RFC 2428](https://www.rfc-editor.org/rfc/rfc2428). `PASV` and `PORT` are the
traditional IPv4 fallbacks. These are command variants within passive and
active operation, not additional data connection modes. The IP family is
selected when the server address is resolved. Proxy traversal and configurable
data-port ranges are not separate options in elder-terms.

## FTP Operation Ordering and Compatibility

Opening a directory node and every other remote operation starts
asynchronously, so the GTK window remains responsive. Each FTP window has its
own authenticated session and reuses its control connection when possible.
Operations wait in FIFO order, and one operation retains its turn through its
data transfer and the server's final completion reply. A directory request
made while another request is active is therefore queued instead of being
interleaved with it.

elder-terms negotiates binary transfer mode and prefers `MLSD` listings when
the server advertises the standardized `MLST`/`MLSD` extensions from
[RFC 3659](https://www.rfc-editor.org/rfc/rfc3659). It falls back to common
Unix-style and DOS-style `LIST` output for older servers. Unusual
server-specific `LIST` formats may not be recognized. FTP does not expose the
SFTP features for symbolic links, POSIX permissions, or timestamp updates.
