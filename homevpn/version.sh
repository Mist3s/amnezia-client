#!/bin/bash
# HomeVPN version for a build number. Usage: homevpn/version.sh <build number>
# Version = upstream version with the last number multiplied by 100 plus the build number:
# upstream 5.1.1.11, build 1 -> 5.1.1.1101. The build number only grows and is never reset.
set -e

BUILD_NUMBER="${1:?build number}"
[[ "$BUILD_NUMBER" =~ ^[1-9][0-9]*$ ]] || { echo "the build number must be a number without leading zeros" >&2; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM_VERSION="$(sed -n 's/^ *set(AMNEZIAVPN_VERSION \([0-9.]*\) .*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$UPSTREAM_VERSION" ] || { echo "cannot read the upstream version" >&2; exit 1; }

echo "${UPSTREAM_VERSION%.*}.$(( ${UPSTREAM_VERSION##*.} * 100 + BUILD_NUMBER ))"
