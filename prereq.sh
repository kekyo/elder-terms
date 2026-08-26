#!/bin/sh

set -eu

PROJECT_ROOT=${ELDER_TERMS_PREREQ_PROJECT_ROOT:-${ELDER_TERMS_PACKAGE_PROJECT_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}}
PROJECT_ROOT=$(CDPATH= cd -- "$PROJECT_ROOT" && pwd)
ELDER_TERMS_PACKAGE_PROJECT_ROOT=$PROJECT_ROOT
ELDER_TERMS_PACKAGE_SOURCE_ONLY=1
. "$PROJECT_ROOT/build_package.sh"
unset ELDER_TERMS_PACKAGE_SOURCE_ONLY
unset ELDER_TERMS_PACKAGE_PROJECT_ROOT

PACKAGE_BUILD_ROOT="$PROJECT_ROOT/.build/package"
DEFAULT_PREREQ_PARALLEL_JOB_CAP=4

print_usage() {
	cat <<'EOF'
Usage: ./prereq.sh [options]

Options:
  --distro <list>   Comma-separated distro filter.
  --release <list>  Comma-separated release filter.
  --arch <list>     Comma-separated architecture filter.
  --jobs <count>    Maximum concurrent image builds. Defaults to auto (up to 4).
  --force           Rebuild images even when a tagged image already exists.
  --help            Show this help.
EOF
}

container_packages() {
	cat <<'EOF'
binutils
build-essential
ca-certificates
dbus-user-session
dpkg-dev
gettext
hicolor-icon-theme
libcanberra-dev
libcanberra-pulse
libgtk-3-dev
libssh-dev
libudev-dev
liburing-dev
libvte-2.91-dev
libx11-dev
libxkbcommon-dev
meson
ninja-build
nodejs
npm
pkg-config
EOF
}

write_containerfile() {
	containerfile=$1

	{
		cat <<'EOF'
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
EOF
		container_packages | while IFS= read -r package_name; do
			[ -n "$package_name" ] || continue
			printf '      %s \\\n' "$package_name"
		done
		cat <<'EOF'
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/elder-terms-node
COPY package.json package-lock.json ./
COPY shared/package.json shared/package.json
COPY elder-terms/package.json elder-terms/package.json
COPY elder-terms-vte/package.json elder-terms-vte/package.json
RUN npm ci --ignore-scripts

ENV PATH="/opt/elder-terms-node/node_modules/.bin:${PATH}"
WORKDIR /workspace
EOF
	} >"$containerfile"
}

build_prereq_image() {
	distro=$1
	release=$2
	arch=$3
	platform=$4
	base_image=$(container_image_for_target "$distro" "$release" "$arch")
	prereq_image=$(prereq_image_for_target "$distro" "$release" "$arch")
	work_dir="$TMP_ROOT/deb/$distro/$release/$arch"
	containerfile="$work_dir/Containerfile"

	if [ "$FORCE" -eq 0 ] &&
		"$CONTAINER_ENGINE_BIN" image exists "$prereq_image" >/dev/null 2>&1; then
		printf '%s\n' "[prereq:deb] exists $prereq_image"
		return 0
	fi

	printf '%s\n' "[prereq:deb] build $prereq_image ($platform, $base_image)"
	rm -rf "$work_dir"
	mkdir -p "$work_dir"
	write_containerfile "$containerfile"

	if [ "$FORCE" -eq 1 ]; then
		set -- --no-cache
	else
		set --
	fi

	"$CONTAINER_ENGINE_BIN" build "$@" \
		--platform "$platform" \
		--pull=missing \
		--build-arg "BASE_IMAGE=$base_image" \
		-t "$prereq_image" \
		-f "$containerfile" \
		"$PROJECT_ROOT"
}

wait_for_oldest_job() {
	[ "$ACTIVE_JOB_COUNT" -gt 0 ] || return 0
	set -- $ACTIVE_JOB_PIDS
	wait_pid=$1
	shift
	if ! wait "$wait_pid"; then JOB_FAILURE=1; fi
	ACTIVE_JOB_PIDS=$*
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT - 1))
}

run_parallel_job() {
	while [ "$ACTIVE_JOB_COUNT" -ge "$PARALLEL_JOBS" ]; do
		wait_for_oldest_job
	done
	[ "$JOB_FAILURE" -eq 0 ] ||
		fail 'One or more prerequisite image builds failed'
	"$@" &
	ACTIVE_JOB_PIDS="${ACTIVE_JOB_PIDS}${ACTIVE_JOB_PIDS:+ }$!"
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT + 1))
}

wait_for_all_jobs() {
	while [ "$ACTIVE_JOB_COUNT" -gt 0 ]; do
		wait_for_oldest_job
	done
	[ "$JOB_FAILURE" -eq 0 ] ||
		fail 'One or more prerequisite image builds failed'
}

schedule_image_builds() {
	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		SCHEDULED_TASK_COUNT=$((SCHEDULED_TASK_COUNT + 1))
		run_parallel_job build_prereq_image "$distro" "$release" "$arch" "$platform"
	done <<EOF
$LINUX_MATRIX
EOF
}

main() {
	DISTRO_FILTER=''
	RELEASE_FILTER=''
	ARCH_FILTER=''
	PARALLEL_JOBS=''
	FORCE=0

	while [ "$#" -gt 0 ]; do
		case $1 in
		--distro)
			[ "$#" -ge 2 ] || fail 'Missing value for --distro'
			DISTRO_FILTER=$(normalize_filter_list distro "$2")
			shift 2
			;;
		--release)
			[ "$#" -ge 2 ] || fail 'Missing value for --release'
			RELEASE_FILTER=$(normalize_filter_list release "$2")
			shift 2
			;;
		--arch)
			[ "$#" -ge 2 ] || fail 'Missing value for --arch'
			ARCH_FILTER=$(normalize_filter_list arch "$2")
			shift 2
			;;
		--jobs)
			[ "$#" -ge 2 ] || fail 'Missing value for --jobs'
			PARALLEL_JOBS=$2
			shift 2
			;;
		--force)
			FORCE=1
			shift
			;;
		--help)
			print_usage
			exit 0
			;;
		*) fail "Unknown argument: $1" ;;
		esac
	done

	if [ -n "$PARALLEL_JOBS" ]; then
		validate_positive_integer 'Parallel job count' "$PARALLEL_JOBS"
	else
		PARALLEL_JOBS=$(min_int "$(detect_processor_count)" "$DEFAULT_PREREQ_PARALLEL_JOB_CAP")
	fi

	CONTAINER_ENGINE_BIN=$(choose_container_engine)
	RUN_ID="prereq-$(date +%Y%m%d%H%M%S)-$$"
	TMP_ROOT="$PACKAGE_BUILD_ROOT/tmp/$RUN_ID"
	ACTIVE_JOB_PIDS=''
	ACTIVE_JOB_COUNT=0
	JOB_FAILURE=0
	SCHEDULED_TASK_COUNT=0
	mkdir -p "$TMP_ROOT"

	printf '%s\n' "Using up to $PARALLEL_JOBS prerequisite image jobs"
	schedule_image_builds
	[ "$SCHEDULED_TASK_COUNT" -gt 0 ] ||
		fail 'No prerequisite image targets matched'
	wait_for_all_jobs
	printf '%s\n' 'Prerequisite images are ready.'
}

if [ "${ELDER_TERMS_PREREQ_SOURCE_ONLY:-0}" = '1' ]; then
	return 0 2>/dev/null || exit 0
fi

main "$@"
