# Receiving files over HTTP/HTTPS

HTTP/HTTPS connections show links from an HTML page in the file-transfer window. Select files from a published directory or a simple link page to download them. The remote side is read-only.

## Configure the connection

Choose “HTTP/HTTPS” as the connection type and enter the base URL on its tab. The base URL defines the permitted directory. It must end with `/` and must not contain credentials, a query or a fragment.

For example, `https://example.com/pub/` permits `/pub/file.zip`, but excludes `/public/file.zip`, `/other/` and links using another host, port or scheme. Redirects follow the same restriction.

Use “Start page URL” for a different initial page. It may be relative to the base URL; an empty value uses the base itself. To start at `index.html`, set the base to `https://example.com/pub/` and the start page to `index.html`. The start page must remain within the base URL.

## Expand links

Connecting fetches only the start page. Its links appear as unresolved items. Selecting rows or displaying icons does not fetch their targets.

Expanding an item fetches its response and determines its type. HTML exposes its links as children; other responses become files. Neither an extension nor a trailing `/` determines the type. Unresolved children remain unfetched until needed.

Listings are cached for the connection. Collapsing and reopening an item reuses the listing; “Refresh” fetches only the current page again.

The path field omits the scheme, hostname, and port. For example, `https://example.com/pub/docs/` appears as `/pub/docs/`. Enter a path and press Enter to navigate there. Queries and trailing slashes remain visible because they may identify different pages. Percent escapes for reserved characters, such as `%3F`, are also preserved.

Full URLs are also accepted. Navigation through the path field and “Up” remains restricted to the configured base URL. It cannot switch to a different host, port, or scheme.

Responses with `text/html` or `application/xhtml+xml` are normally expanded. Attachments and responses without a recognized type are treated as content. Choose “Expand as HTML” from the context menu when a server reports an incorrect type.

## Receive files and directories

Select one or more items and choose the receive mode from the context menu.

| Operation | Result |
| --- | --- |
| Receive as file | Saves the selected response unchanged, including original HTML, without following its links. |
| Receive as directory | Follows HTML links incrementally and saves them under a local directory. Non-HTML responses are saved as files. |

Directory reception follows links incrementally from the selected items as needed. It does not scan ahead to calculate totals. URLs already processed by the job and links to ancestors are not repeatedly traversed.

Local names come from URL paths. Names that cannot be decoded for display retain their percent escapes. Query variants or other name collisions receive numeric suffixes. Display names never replace the original request URL. Existing local files use the shared overwrite confirmation.

Item counts and total bytes remain unknown while discovery continues. Responses without a length are streamed, and the job list shows bytes received. Cancellation or an interrupted download removes incomplete files; temporary files that could not be removed are listed in the job details.

Upload, new folder, rename, delete and attribute changes are unavailable.

## Limits and supported pages

| Resource | Limit |
| --- | --- |
| HTML body per page | 2 MiB |
| Redirects per fetch | 5 |
| Recursive receive depth | 32 |
| HTML pages per recursive receive | 1,000 |
| Links processed per page or recursive receive | 10,000 |

Reaching a limit displays its reason and is not reported as complete success. Select a smaller part to receive. Previously completed files are retained.

HTML encoding is selected from the BOM, HTTP charset, then a meta declaration, falling back to UTF-8. A body that cannot be decoded using its declared encoding fails. Percent-encoded URL bytes are maintained independently of the HTML encoding.

To preserve file bytes, the connection requests uncompressed HTTP responses. Servers that ignore this request and return compressed content cause the receive operation to fail.

Only `a href` links are followed. JavaScript execution, login forms, and collecting images or styles to reproduce a web page are unsupported. The [libxml2 HTML parser](https://gnome.pages.gitlab.gnome.org/libxml2/html/HTMLparser_8h.html) does not guarantee the same HTML5 behavior as a browser.

## Authentication, certificates and SSH proxy

Choose no authentication, Basic, Digest or automatic selection. Authentication prompts for a username and password when connecting. Passwords are not saved. Automatic selection covers Basic and Digest.

HTTPS uses TLS 1.2 or later and verifies the certificate and destination name. Supply a PEM CA file for a private CA. Certificate failures reject the connection by default. “Confirm in an overlay” permits approval of the displayed certificate for that connection only; exceptions are not saved. The confirmation policy and TLS backend restrictions are shared with [WebDAV certificate confirmation](webdav.md#https-certificate-confirmation).

[SSH proxy](ssh-proxy.md) supports connections through a gateway. Destination name resolution happens at the gateway, while HTTPS certificates are checked against the original destination name.

## Configuration file

Each setting inherits connection defaults and can be overridden per connection.

| `[http]` key | Default | Meaning |
| --- | --- | --- |
| `base_url` | Empty | Required HTTP or HTTPS directory URL defining the allowed scope. |
| `start_url` | Empty | Initial page within the base URL; empty uses the base itself. |
| `authentication` | `none` | `none`, `basic`, `digest` or `auto`. |
| `username` | Empty | Initial username for the runtime prompt. |
| `local_directory` | Empty | Initial local directory; empty uses the shared fallback. |
| `ca_file` | Empty | Absolute path to a PEM CA file; empty uses system CAs. |
| `certificate_error_action` | `reject` | `reject` refuses the connection; `prompt` asks in an overlay. |
| `connect_timeout_seconds` | `30` | Connection deadline, from 1 to 86400 seconds. |
| `idle_timeout_seconds` | `60` | Deadline without network progress, from 1 to 86400 seconds. |

```ini
[general]
type=http
name=Published files

[http]
base_url=https://example.com/pub/
start_url=index.html
authentication=none
local_directory=/home/user/Downloads
```
