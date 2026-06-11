#!/bin/bash
# SPDX-License-Identifier: MIT
#
# make-source-package.sh — build the signed (or unsigned) Debian SOURCE
# package for PPA upload.
#
# Why this exists: the canonical packaging lives in packaging/debian/
# and day-to-day binary builds use a gitignored `debian ->
# packaging/debian` symlink. dpkg-source's 3.0 (quilt) packer cannot
# build debian.tar.xz through a symlinked debian/ (dh tolerates it;
# dpkg-source does not), so the source build temporarily swaps in a
# real copy and restores the symlink afterwards — packaging/debian/
# itself stays pristine.
#
# Usage: packaging/make-source-package.sh [--signed]
#   (run from the repo root, clean tree)
#
# Produces in ../ : intp_<uv>.orig.tar.gz, intp_<v>.debian.tar.xz,
# intp_<v>.dsc, intp_<v>_source.changes (+ .buildinfo). Upload with:
#   dput ppa:norohim/intp ../intp_<v>_source.changes

set -eu

SIGN_ARGS=("-us" "-uc")
[ "${1:-}" = "--signed" ] && SIGN_ARGS=()

[ -f packaging/debian/changelog ] || {
	echo "ERROR: run from the repo root" >&2
	exit 1
}

if [ -n "$(git status --porcelain)" ]; then
	echo "ERROR: working tree not clean — the orig tarball is cut" \
		"from HEAD and must match it" >&2
	exit 1
fi

VERSION=$(dpkg-parsechangelog -l packaging/debian/changelog -S Version)
UPSTREAM=${VERSION%-*}

echo "==> source package for intp ${VERSION} (upstream ${UPSTREAM})"

# 1. orig tarball from HEAD (tracked files only — the debian symlink
#    is untracked and the checked-in vmlinux.h fallback IS tracked).
ORIG="../intp_${UPSTREAM}.orig.tar.gz"
if [ ! -f "$ORIG" ]; then
	git archive --format=tar.gz --prefix="intp-${UPSTREAM}/" \
		-o "$ORIG" HEAD
	echo "==> wrote $ORIG"
else
	echo "==> reusing existing $ORIG"
fi

# 2. swap the symlink for a real copy; restore on any exit.
restore_symlink() {
	rm -rf debian
	ln -s packaging/debian debian
}
trap restore_symlink EXIT
rm -f debian
cp -a packaging/debian debian

# 3. build (source-only). -d skips build-dep checks: source packages
#    need only the packaging tools, and dev hosts may carry meson via
#    pipx rather than apt.
dpkg-buildpackage -S "${SIGN_ARGS[@]}" -d

echo
echo "==> artifacts:"
ls -la "../intp_${VERSION}.dsc" "../intp_${VERSION}_source.changes"
echo
echo "==> next: lintian -EvIL +pedantic ../intp_${VERSION}_source.changes"
[ ${#SIGN_ARGS[@]} -eq 0 ] || echo "==> NOTE: built UNSIGNED (test mode); rerun with --signed for upload"
