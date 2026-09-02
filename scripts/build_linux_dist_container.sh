#!/bin/sh

set -eu

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

require_env() {
	var_name=$1
	eval "var_value=\${$var_name:-}"
	[ -n "$var_value" ] ||
		fail "Missing required environment variable: $var_name"
}

require_command() {
	command -v "$1" >/dev/null 2>&1 ||
		fail "Missing required command: $1"
}

validate_positive_integer() {
	value_name=$1
	value=$2
	case $value in
	'' | *[!0-9]*) fail "$value_name must be a positive integer: $value" ;;
	esac
	[ "$value" -gt 0 ] ||
		fail "$value_name must be a positive integer: $value"
}

assert_file() {
	[ -f "$1" ] || fail "Missing expected file: $1"
}

assert_symlink() {
	link_path=$1
	expected_target=$2
	[ -L "$link_path" ] || fail "Missing expected symbolic link: $link_path"
	actual_target=$(readlink "$link_path")
	[ "$actual_target" = "$expected_target" ] ||
		fail "Unexpected symbolic link target for $link_path: $actual_target"
}

write_control_file() {
	control_path=$1
	depends_value=$2
	{
		printf 'Package: %s\n' "$ELDER_TERMS_PACKAGE_NAME"
		printf 'Version: %s\n' "$ELDER_TERMS_PACKAGE_VERSION"
		printf 'Section: x11\n'
		printf 'Priority: optional\n'
		printf 'Architecture: %s\n' "$deb_arch"
		printf 'Maintainer: %s\n' "$ELDER_TERMS_PACKAGE_MAINTAINER"
		printf 'Depends: %s\n' "$depends_value"
		printf 'Homepage: https://github.com/kekyo/elder-terms\n'
		printf 'Description: %s\n' "$ELDER_TERMS_PACKAGE_DESCRIPTION"
	} >"$control_path"
}

calculate_shlibdeps() {
	tmp_dir=$(mktemp -d)
	private_dir="$stage_dir/usr/lib/elder-terms"
	mkdir -p "$tmp_dir/debian"
	{
		printf 'Source: elder-terms\n'
		printf 'Section: x11\n'
		printf 'Priority: optional\n'
		printf 'Maintainer: %s\n' "$ELDER_TERMS_PACKAGE_MAINTAINER"
		printf 'Standards-Version: 4.6.2\n\n'
		printf 'Package: %s\n' "$ELDER_TERMS_PACKAGE_NAME"
		printf 'Architecture: %s\n' "$deb_arch"
		printf 'Description: temporary metadata for dependency calculation\n'
	} >"$tmp_dir/debian/control"

	depends_value=$(
		cd "$tmp_dir"
		dpkg-shlibdeps \
			--ignore-missing-info \
			-O \
			-l"$private_dir" \
			"$private_dir/libelder-terms.so" \
			"$private_dir/launcher/elder-terms" \
			"$private_dir/elder-terms-vte/elder-terms-vte" \
			"$private_dir/elder-terms-vte/elder-terms-file-transfer" |
			sed -n 's/^shlibs:Depends=//p'
	)
	rm -rf "$tmp_dir"
	[ -n "$depends_value" ] ||
		fail 'dpkg-shlibdeps did not calculate runtime dependencies'
	printf '%s\n' "$depends_value"
}

validate_dynamic_links() {
	for executable_path in \
		/usr/lib/elder-terms/launcher/elder-terms \
		/usr/lib/elder-terms/elder-terms-vte/elder-terms-vte \
		/usr/lib/elder-terms/elder-terms-vte/elder-terms-file-transfer; do
		link_report=$(ldd "$executable_path")
		case $link_report in
		*'not found'*)
			printf '%s\n' "$link_report" >&2
			fail "Installed executable has unresolved libraries: $executable_path"
			;;
		esac
	done
}

validate_installed_package() {
	package_path=$1
	require_env ELDER_TERMS_PACKAGE_VERSION
	require_env ELDER_TERMS_PACKAGE_NAME
	require_command dpkg
	require_command dpkg-query
	require_command ldd
	require_command readlink
	assert_file "$package_path"

	dpkg \
		"--path-include=/usr/share/doc/$ELDER_TERMS_PACKAGE_NAME/*" \
		"--path-include=/usr/share/locale/*/LC_MESSAGES/$ELDER_TERMS_PACKAGE_NAME.mo" \
		-i "$package_path"
	installed_status=$(dpkg-query -W -f='${Status}' "$ELDER_TERMS_PACKAGE_NAME")
	[ "$installed_status" = 'install ok installed' ] ||
		fail "Package installation did not complete: $installed_status"
	installed_version=$(dpkg-query -W -f='${Version}' "$ELDER_TERMS_PACKAGE_NAME")
	[ "$installed_version" = "$ELDER_TERMS_PACKAGE_VERSION" ] ||
		fail "Unexpected installed package version: $installed_version"

	for installed_file in \
		/usr/lib/elder-terms/libelder-terms.so \
		/usr/lib/elder-terms/launcher/elder-terms \
		/usr/lib/elder-terms/launcher/main-window.ui \
		/usr/lib/elder-terms/elder-terms-vte/elder-terms-vte \
		/usr/lib/elder-terms/elder-terms-vte/elder-terms-file-transfer \
		/usr/lib/elder-terms/elder-terms-vte/main-window.ui \
		/usr/lib/elder-terms/elder-terms-vte/green-on.png \
		/usr/lib/elder-terms/elder-terms-vte/green-off.png \
		/usr/share/applications/net.kekyo.elder-terms.desktop \
		/usr/share/applications/net.kekyo.elder-terms-vte.desktop \
		/etc/xdg/autostart/net.kekyo.elder-terms.desktop \
		/usr/share/icons/hicolor/256x256/apps/elder-terms.png \
		/usr/share/locale/ja/LC_MESSAGES/elder-terms.mo \
		/usr/share/doc/elder-terms/copyright; do
		assert_file "$installed_file"
	done
	assert_symlink /usr/bin/elder-terms \
		'../lib/elder-terms/launcher/elder-terms'
	assert_symlink /usr/bin/elder-terms-vte \
		'../lib/elder-terms/elder-terms-vte/elder-terms-vte'
	assert_symlink /usr/bin/elder-terms-file-transfer \
		'../lib/elder-terms/elder-terms-vte/elder-terms-file-transfer'
	validate_dynamic_links
	printf '%s\n' "Installed package validated: $package_path"
}

if [ "${1:-}" = '--validate-package' ]; then
	[ "$#" -eq 2 ] ||
		fail 'Usage: build_linux_dist_container.sh --validate-package <path>'
	validate_installed_package "$2"
	exit 0
fi

[ "$#" -eq 0 ] || fail "Unknown argument: $1"

require_env ELDER_TERMS_WORK_DIR
require_env ELDER_TERMS_PACKAGE_VERSION
require_env ELDER_TERMS_PACKAGE_NAME
require_env ELDER_TERMS_PACKAGE_DESCRIPTION
require_env ELDER_TERMS_PACKAGE_MAINTAINER
require_env ELDER_TERMS_BUILD_TYPE

ELDER_TERMS_MAKE_JOBS=${ELDER_TERMS_MAKE_JOBS:-1}
validate_positive_integer 'ELDER_TERMS_MAKE_JOBS' "$ELDER_TERMS_MAKE_JOBS"
case $ELDER_TERMS_BUILD_TYPE in
release | debug) ;;
*) fail "Unsupported Meson build type: $ELDER_TERMS_BUILD_TYPE" ;;
esac

work_dir=$ELDER_TERMS_WORK_DIR
build_dir="$work_dir/build"
stage_dir="$work_dir/stage/$ELDER_TERMS_PACKAGE_NAME"

require_command dpkg-architecture
require_command dpkg-shlibdeps
require_command gestament-config
require_command make
require_command meson
require_command pkg-config

for pkg_config_module in \
	gio-2.0 \
	gdk-pixbuf-2.0 \
	gtk+-3.0 \
	libcanberra \
	libssh \
	libudev \
	liburing \
	vte-2.91 \
	x11 \
	xkbcommon; do
	pkg-config --exists "$pkg_config_module" ||
		fail "Missing required pkg-config module: $pkg_config_module"
done

rm -rf "$build_dir" "$stage_dir"
mkdir -p "$work_dir"
meson setup "$build_dir" . \
	--buildtype="$ELDER_TERMS_BUILD_TYPE" \
	--prefix=/usr \
	--libdir=lib \
	-Dautostartdir=/etc/xdg/autostart \
	-Dapplication_version="$ELDER_TERMS_PACKAGE_VERSION" \
	-Dbuild_tests=false \
	-Dwerror=false
meson compile -C "$build_dir" -j "$ELDER_TERMS_MAKE_JOBS"
mkdir -p "$stage_dir"
if [ "$ELDER_TERMS_BUILD_TYPE" = 'release' ]; then
	DESTDIR="$stage_dir" meson install -C "$build_dir" --no-rebuild --strip
else
	DESTDIR="$stage_dir" meson install -C "$build_dir" --no-rebuild
fi

deb_arch=$(dpkg-architecture -qDEB_HOST_ARCH)
control_dir="$stage_dir/DEBIAN"
mkdir -p "$control_dir"
shlib_depends=$(calculate_shlibdeps)
runtime_depends='dbus-user-session, hicolor-icon-theme, libcanberra-pulse, openssh-client'
write_control_file "$control_dir/control" "$shlib_depends, $runtime_depends"
chmod 0644 "$control_dir/control"

for staged_file in \
	usr/lib/elder-terms/libelder-terms.so \
	usr/lib/elder-terms/launcher/elder-terms \
	usr/lib/elder-terms/launcher/main-window.ui \
	usr/lib/elder-terms/elder-terms-vte/elder-terms-vte \
	usr/lib/elder-terms/elder-terms-vte/elder-terms-file-transfer \
	usr/lib/elder-terms/elder-terms-vte/main-window.ui \
	usr/lib/elder-terms/elder-terms-vte/green-on.png \
	usr/lib/elder-terms/elder-terms-vte/green-off.png \
	usr/share/applications/net.kekyo.elder-terms.desktop \
	usr/share/applications/net.kekyo.elder-terms-vte.desktop \
	etc/xdg/autostart/net.kekyo.elder-terms.desktop \
	usr/share/icons/hicolor/256x256/apps/elder-terms.png \
	usr/share/locale/ja/LC_MESSAGES/elder-terms.mo \
	usr/share/doc/elder-terms/copyright; do
	assert_file "$stage_dir/$staged_file"
done
assert_symlink "$stage_dir/usr/bin/elder-terms" \
	'../lib/elder-terms/launcher/elder-terms'
assert_symlink "$stage_dir/usr/bin/elder-terms-vte" \
	'../lib/elder-terms/elder-terms-vte/elder-terms-vte'
assert_symlink "$stage_dir/usr/bin/elder-terms-file-transfer" \
	'../lib/elder-terms/elder-terms-vte/elder-terms-file-transfer'
