#!/bin/bash
# SPDX-License-Identifier: MIT
#
# check-series-build.sh — rehearse a Launchpad PPA build for one Ubuntu
# series without Launchpad: cut the source package exactly as
# make-source-package.sh does for an upload, rebuild the binaries from
# that .dsc the way a PPA builder would, install them, and run the
# autopkgtest smoke commands.
#
# What it mimics:
#   - the per-series source variant (jammy, noble) from
#     make-source-package.sh, built from committed HEAD only;
#   - first-alternative Build-Depends resolution: Launchpad's builders
#     ignore every alternative after the first, so this script strips
#     them before installing build dependencies;
#   - a clean build root holding only build-essential and the declared
#     build dependencies.
# What it cannot mimic: the builder's missing /sys/kernel/btf/vmlinux.
# Containers see the host's BTF; mask it from outside (see below) or the
# checked-in vmlinux.h fallback is not exercised. The script warns.
#
# It installs packages, so it refuses to run outside a throwaway
# container. Run it as root inside a container of the target series,
# from the repo root:
#
#   podman run --rm -v /dev/null:/sys/kernel/btf/vmlinux:ro \
#       -v "$PWD":/src:ro ubuntu:24.04 \
#       bash -c 'cd /src && packaging/check-series-build.sh noble'
#
# Usage: packaging/check-series-build.sh SERIES
#   SERIES  jammy, noble or resolute; must match the container's release
#
# Environment:
#   INTP_CHECK_WORKDIR  scratch directory (default /tmp/intp-series-check)
#   INTP_CHECK_OUTDIR   copy the source package and .debs here at the end

set -eu

SERIES="${1:?usage: check-series-build.sh SERIES}"
WORK="${INTP_CHECK_WORKDIR:-/tmp/intp-series-check}"
OUTDIR="${INTP_CHECK_OUTDIR:-}"
SRC=$(pwd)

[ -f "$SRC/packaging/make-source-package.sh" ] || {
	echo "ERROR: run from the repo root" >&2
	exit 1
}
if [ "$(id -u)" != 0 ] || { [ ! -f /.dockerenv ] && [ ! -f /run/.containerenv ]; }; then
	echo "ERROR: this script installs packages; run it as root in a" \
		"throwaway container (see the header)" >&2
	exit 1
fi

# shellcheck source=/dev/null
. /etc/os-release
if [ "${VERSION_CODENAME:-}" != "$SERIES" ]; then
	echo "ERROR: container is ${VERSION_CODENAME:-unknown}, not $SERIES" >&2
	exit 1
fi

if [ -r /sys/kernel/btf/vmlinux ] && [ -s /sys/kernel/btf/vmlinux ]; then
	echo "WARNING: host BTF is visible; the build uses it instead of the" \
		"checked-in vmlinux.h that Launchpad builders fall back to"
fi

step() { echo; echo "==> $*"; }

export DEBIAN_FRONTEND=noninteractive

step "tooling"
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
	build-essential ca-certificates debhelper devscripts dpkg-dev \
	equivs git >/dev/null

# 1. Source package from a clean clone of HEAD, so the working tree that
#    called us is never touched and only committed files are packaged.
step "source package for $SERIES"
rm -rf "$WORK"
mkdir -p "$WORK"
git config --global --add safe.directory "$SRC"
git config --global --add safe.directory "$SRC/.git"
git clone -q --no-local "$SRC" "$WORK/repo"
(cd "$WORK/repo" && packaging/make-source-package.sh --series "$SERIES")

DSC=$(ls "$WORK"/intp_*~ubuntu*.dsc)
echo "    $(basename "$DSC")"

# 2. Unpack it as a builder would and keep only the first alternative of
#    each Build-Depends entry.
step "unpack and resolve Build-Depends (first alternative only)"
dpkg-source -x "$DSC" "$WORK/build" >/dev/null
cd "$WORK/build"
sed -i -E 's/^( [^|,]+)\|[^,]*,/\1,/' debian/control
sed -n '/^Build-Depends:/,/^[A-Z]/p' debian/control | sed '$d'
mk-build-deps -i -r -t 'apt-get -y -qq --no-install-recommends' \
	debian/control >/dev/null
rm -f intp-build-deps_*

# 3. Binary build, with the test suite (dh_auto_test).
step "binary build"
dpkg-buildpackage -b -us -uc -j"$(nproc)"

# 4. Install what was built and run the autopkgtest smoke commands.
step "install and smoke-test"
apt-get install -y -qq --no-install-recommends "$WORK"/*.deb python3 >/dev/null
status=0
while IFS= read -r cmd; do
	echo "  \$ $cmd"
	sh -c "$cmd" >/dev/null || {
		echo "  FAILED"
		status=1
	}
done < <(sed -n 's/^Test-Command: //p' debian/tests/control)
intp --version

if [ -n "$OUTDIR" ]; then
	mkdir -p "$OUTDIR"
	cp "$WORK"/*.dsc "$WORK"/*.debian.tar.xz "$WORK"/*_source.changes \
		"$WORK"/*.deb "$OUTDIR"/
fi

[ "$status" = 0 ] || {
	echo "ERROR: smoke tests failed" >&2
	exit 1
}
echo
echo "OK: $SERIES ($(dpkg --print-architecture)) builds from its source package"
