# Using SSH proxy

Use an SSH gateway to reach SSH, SFTP, Telnet, WebDAV and passive FTP/FTPS destinations.
Keep the destination address and login in its protocol tab. Enter the gateway details in the separate SSH proxy tab and enable it.
SSH and SFTP authenticate the gateway and destination separately and verify both host keys.
Gateway prompts include the SSH proxy label.

## Settings and inheritance

Configure a shared gateway in global settings. Select Disabled in a connection to override an inherited gateway; select Inherit to restore the global choice.
Local shells and serial connections ignore these settings.
Connection routes are read-only while connected. Reconnect or open a new window after changing saved settings.

| Key in `[ssh_proxy]` | Default | Meaning |
| --- | --- | --- |
| `enabled` | `false` | Set to `true` to use the gateway. |
| `address` | Empty | Gateway hostname, IPv4 or IPv6 address, without a URL scheme or port. |
| `port` | `22` | Gateway SSH port, from 1 to 65535. |
| `username` | Empty | Initial value for the gateway user-name prompt. |
| `identity_file` | Empty | Gateway private key. Empty uses the existing SSH key selection behavior. |

Passwords and private-key passphrases are requested when needed and are never saved in the connection file.

```ini
[general]
type=telnet
name=Internal console

[telnet]
address=console.internal.example
port=23

[ssh_proxy]
enabled=true
address=bastion.example.com
port=22
username=operator
identity_file=/home/operator/.ssh/bastion_key
```

The client resolves the gateway name; the gateway resolves the destination name. This supports internal names unavailable to the client.
Failed gateway connections, authentication or forwarding never fall back to a direct connection.
Invalid enable flags and invalid enabled gateway settings are rejected even when loaded from a manually edited INI file.
WebDAV and FTP/FTPS use the selected SSH proxy even when `NO_PROXY` or `no_proxy` matches the destination.

## Connections and encryption

The gateway must permit TCP forwarding. For OpenSSH, check `AllowTcpForwarding`, `PermitOpen` and per-key restrictions in the [server configuration](https://man.openbsd.org/sshd_config).

SSH/SFTP retain SSH to the final destination. HTTPS and explicit/implicit FTPS retain end-to-end TLS and certificate name verification.
For Telnet, HTTP and plain FTP, SSH encrypts the connection only as far as the gateway; the gateway-to-destination segment is plaintext.

Select Passive in the FTP tab. Both the control connection and each listing/upload/download data connection pass through the gateway.
The gateway must be able to reach the server's control port and passive data ports.
EPSV is preferred; PASV also uses the configured destination, ignoring any different host advertised by the server.
Combining SSH proxy with Active mode is rejected in settings and at connection time. The mode is never changed automatically.
See [FTP/SFTP details](ftp-sftp.md).

SFTP opened from an SSH terminal shares the authenticated destination connection. Either window can remain usable after closing the other.
Standalone SFTP and reconnection use the same gateway settings.

One gateway is supported. Multiple hops, arbitrary ProxyCommand execution and a general-purpose SOCKS service for external clients are outside this scope.
