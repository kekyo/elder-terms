# Building from Source and Creating Packages

This guide covers building for personal use, development, and distribution. To use a prebuilt deb package, see [installation in the README](../../README.md#installation).

## Building from Source

NFS requires system [libnfs](https://github.com/sahlberg/libnfs) 5.0.2 or later and its development headers.

WebDAV requires libcurl 7.88.1 or newer with HTTP/HTTPS and asynchronous DNS, plus [libxml2](https://gnome.pages.gitlab.gnome.org/libxml2/) 2.9.14 or newer.

The FTP client requires [libcurl](https://curl.se/libcurl/) 7.88.1 or newer
with FTP and asynchronous DNS support. FTPS additionally requires a
[TLS-enabled libcurl build](https://curl.se/libcurl/c/libcurl-tutorial.html).
The Debian/Ubuntu packages use OpenSSL-backed libcurl and depend on
`ca-certificates` for the default trust store. Source builds also need OpenSSL
headers and libraries. Certificate confirmation must use the same OpenSSL
version as libcurl; the application checks this before connecting.

To build on Ubuntu or Debian, install a C++20-capable compiler, Meson, Ninja,
gettext, and the development packages for the libraries used by elder-terms:

Rendering requires GTK3 3.24 or later, Pango 1.50 or later, Cairo 1.18 or later, IBus 1.5.29 or later, and Wayland client 1.22 or later. See the [boxrig build requirements](https://github.com/kekyo/boxrig/blob/0.12.0/docs/en/building.md).

```bash
sudo apt update
sudo apt install build-essential git meson ninja-build pkg-config gettext imagemagick libgdk-pixbuf2.0-bin librsvg2-common \
  libglib2.0-dev libgtk-3-dev libgdk-pixbuf-2.0-dev libcanberra-dev libx11-dev libxfixes-dev \
  libxkbcommon-dev liburing-dev libudev-dev libsystemd-dev libssh-dev libnfs-dev libcurl4-openssl-dev \
  libcairo2-dev libpango1.0-dev libfontconfig1-dev libepoxy-dev libibus-1.0-dev \
  libwayland-dev libgl-dev libegl-dev libssl-dev libxml2-dev libtasn1-6-dev libtasn1-bin doxygen patch ca-certificates xz-utils xdg-utils
```

Node.js 22.18 or later is also required for
[direct TypeScript execution](https://nodejs.org/docs/latest-v22.x/api/typescript.html).

Material Icons are rendered to PNG using `gdk-pixbuf-thumbnailer` with SVG support
and ImageMagick's `convert` during the build.
The generated images are included in the distribution; these generation tools
are not needed at runtime.

Check the installed Node.js version when using your distribution's package:

```bash
sudo apt install nodejs
node --version
```

If the distribution's Node.js is too old, use an
[official Node.js distribution](https://nodejs.org/en/download) or a version
manager of your choice, such as [nvm](https://github.com/nvm-sh/nvm).

When cloning the source for the first time, include the submodules to obtain
the [cardio](https://github.com/kekyo/cardio/),
[libxyzm](https://github.com/kekyo/libxyzm/), and [boxrig](https://github.com/kekyo/boxrig/) dependencies:

```bash
git clone --recurse-submodules https://github.com/kekyo/elder-terms.git
cd elder-terms
```

If you have already cloned the repository, initialize and update its
submodules with:

```bash
git submodule update --init --recursive
```

Install the npm dependencies and build all workspaces:

```bash
npm ci
npm run build
```

Run the built launcher from the repository root with the following command.
The launcher automatically detects the terminal and shared SFTP/FTP file
transfer executables in the same build directory.

```bash
./.build/elder-terms/elder-terms
```

Alternatively:

```bash
npm run dev
```

To run the tests (which take a very long time because they include UI tests):

```bash
npm run test
```

## Building deb Packages

The package-building scripts combine the launcher, terminal, and shared
SFTP/FTP file transfer executables; dedicated shared library and UI data;
translation catalogs; desktop and XDG autostart entries; icons;
documentation; and license notices into a single `elder-terms` deb package.

The following package combinations are supported:

| Distribution | Release | Architectures |
| :--- | :--- | :--- |
| Debian | trixie | amd64, i386, arm64, armhf, riscv64 |
| Ubuntu | 24.04 | amd64, arm64 |
| Ubuntu | 26.04 | amd64, arm64 |

Debian bookworm is excluded because its standard [Cairo 1.16](https://packages.debian.org/bookworm/libcairo2-dev) and [IBus 1.5.27](https://packages.debian.org/bookworm/libibus-1.0-dev) are below the rendering library's required versions.

The host requires Podman, binfmt/QEMU for non-native targets, `dpkg-deb`,
`readelf`, GNU Make, Node.js 22.18 or later, npm, and `npx`.
Preparing preset images also requires `gdk-pixbuf-thumbnailer` with SVG support,
ImageMagick's `convert`, and GLib's `glib-compile-resources`. Run `npm ci` first.
From the repository root, generate the
reusable prerequisite images once, then build packages for every combination:

```sh
./prereq.sh
./build_package_all.sh
```

Regenerate the images after changing the prerequisite package configuration:

```sh
./prereq.sh --force
```

Both commands support filtering with `--distro`, `--release`, `--arch`, and
`--jobs`. For example, to build only for Ubuntu 24.04 amd64, matching the host:

```sh
./prereq.sh \
  --distro ubuntu --release 24.04 --arch amd64
./build_package.sh \
  --target deb --distro ubuntu --release 24.04 --arch amd64
```

Architecture aliases such as `amd64`, `i386`, `armhf`, and `aarch64` are also
accepted. `noble` and `resolute` are treated as Ubuntu 24.04 and 26.04,
respectively. Pass `--debug` to preserve debugging information. If `--version`
is omitted, the version is determined by
`npx screw-up format -e '{version}' -f`.

Artifacts are stored in `artifacts/deb/`, with names such as
`elder-terms-VERSION-ubuntu-24.04-amd64.deb`. Each package is inspected for its
control metadata, runtime dependencies, installed layout, symbolic-link
targets, and ELF architecture. It is then installed in a fresh container for
the same distribution and architecture to verify the required files and
dynamic-library resolution.
