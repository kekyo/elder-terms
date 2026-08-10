#!/bin/bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_SCRIPT="${BUILD_PACKAGE_SCRIPT:-${ROOT_DIR}/build_package.sh}"
FORWARDED_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --refresh-base)
            echo "Warning: --refresh-base is ignored. Run ./prereq.sh --force to rebuild prerequisite images." >&2
            shift
            ;;
        *)
            FORWARDED_ARGS+=("$1")
            shift
            ;;
    esac
done

exec "${PACKAGE_SCRIPT}" --target all "${FORWARDED_ARGS[@]}"
