#!/bin/bash
# SPDX-License-Identifier: MIT
#
# make-source-package.sh — build the signed (or unsigned) Debian SOURCE
# packages for PPA upload, one per Ubuntu series.
#
# Why this exists: the canonical packaging lives in packaging/debian/
# and day-to-day binary builds use a gitignored `debian ->
# packaging/debian` symlink. dpkg-source's 3.0 (quilt) packer cannot
# build debian.tar.xz through a symlinked debian/ (dh tolerates it;
# dpkg-source does not), so the source build temporarily swaps in a
# real copy and restores the symlink afterwards — packaging/debian/
# itself stays pristine.
#
# Per-series builds: Launchpad builds for the distribution named in the
# uploaded changelog, so each series gets a throwaway top changelog
# entry in the temporary copy, versioned
#   <committed version>~ppa<N>~ubuntu<YY.MM>.1
# e.g. 0.10.0-1~ppa1~ubuntu24.04.1. The suffix keeps every PPA build
# below the eventual archive version 0.10.0-1, and the numbers grow with
# the series, so a release upgrade always moves the package forward.
# One orig tarball serves every series.
#
# jammy (22.04) variant: jammy ships libbpf 0.5 (Build-Depends needs
# >= 0.8) and no standalone bpftool, and Launchpad resolves only the
# first alternative of a Build-Depends entry. The jammy copy drops
# clang, bpftool and libbpf-dev and configures -Dwith_ebpf=disabled:
# the pure-C engine only, same output schema (ADR-0003).
#
# noble (24.04) variant: noble has no real bpftool package. The name is
# only a virtual package provided by linux-tools-common and
# linux-lowlatency-tools-common, which ship at most the /usr/sbin/bpftool
# wrapper; with first-alternative resolution the builder ends up without
# a working bpftool and meson stops at configure. The noble copy names
# linux-tools-generic first, which pulls a versioned
# /usr/lib/linux-tools/<kver>/bpftool that the meson probe finds.
#
# Usage: packaging/make-source-package.sh [options]
#   (run from the repo root, clean tree)
#
#   --series LIST   comma-separated series (default: jammy,noble,resolute);
#                   "none" builds the committed changelog version as is
#   --ppa-rev N     the ~ppaN counter (default 1); bump it for a
#                   packaging-only re-upload of the same upstream version
#   --signed        sign the .dsc/.changes (default: unsigned test mode)
#   --key KEYID     signing key, passed to dpkg-buildpackage --sign-key
#   --no-orig       upload without the orig tarball for every series
#                   (-sd); by default the first series carries it (-sa)
#
# Produces in ../ : intp_<uv>.orig.tar.gz and, per series,
# intp_<v>.debian.tar.xz, intp_<v>.dsc, intp_<v>_source.changes
# (+ .buildinfo). Upload to staging first:
#   dput ppa:norohim/intp-staging ../intp_<v>_source.changes

set -eu

SERIES_LIST="jammy,noble,resolute"
PPA_REV=1
SIGNED=0
SIGN_KEY=""
FIRST_ORIG="-sa"

while [ $# -gt 0 ]; do
	case "$1" in
	--series) SERIES_LIST="${2:?--series needs a value}"; shift 2 ;;
	--ppa-rev) PPA_REV="${2:?--ppa-rev needs a value}"; shift 2 ;;
	--signed) SIGNED=1; shift ;;
	--key) SIGN_KEY="${2:?--key needs a value}"; SIGNED=1; shift 2 ;;
	--no-orig) FIRST_ORIG="-sd"; shift ;;
	-h | --help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "ERROR: unknown option $1" >&2; exit 2 ;;
	esac
done

series_version() {
	case "$1" in
	jammy) echo 22.04 ;;
	noble) echo 24.04 ;;
	resolute) echo 26.04 ;;
	*) return 1 ;;
	esac
}

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
MAINT=$(dpkg-parsechangelog -l packaging/debian/changelog -S Maintainer)

SIGN_ARGS=("-us" "-uc")
if [ "$SIGNED" = 1 ]; then
	SIGN_ARGS=()
	[ -n "$SIGN_KEY" ] && SIGN_ARGS=("--sign-key=${SIGN_KEY}")
fi

echo "==> source packages for intp ${VERSION} (upstream ${UPSTREAM})"

# 1. orig tarball from HEAD (tracked files only — the debian symlink
#    is untracked and the checked-in vmlinux.h fallbacks ARE tracked).
#    Cut once per upstream version: Launchpad rejects a changed tarball
#    under the same name, and git archive output is not guaranteed
#    byte-identical across git versions. For a later series or a
#    rebuild, fetch the tarball attached to the GitHub release into ../
#    first; it is reused as is.
ORIG="../intp_${UPSTREAM}.orig.tar.gz"
if [ ! -f "$ORIG" ]; then
	git archive --format=tar.gz --prefix="intp-${UPSTREAM}/" \
		-o "$ORIG" HEAD
	echo "==> wrote $ORIG"
else
	echo "==> reusing existing $ORIG"
fi

# 2. swap the symlink for a real copy per series; restore on any exit.
restore_symlink() {
	rm -rf debian
	ln -s packaging/debian debian
}
trap restore_symlink EXIT

# Drop the eBPF toolchain from Build-Depends and turn the backends off.
apply_jammy_variant() {
	local d="$1"

	sed -i -E '/^ (clang|bpftool|libbpf-dev)[ ,(\[]/d' "$d/control"
	sed -i 's/-Dwith_ebpf=enabled/-Dwith_ebpf=disabled/' "$d/rules"
	if grep -q -E '^ (clang|bpftool|libbpf-dev)[ ,(\[]' "$d/control" ||
		! grep -q -- '-Dwith_ebpf=disabled' "$d/rules"; then
		echo "ERROR: jammy variant did not apply cleanly" >&2
		exit 1
	fi
}

# Name linux-tools-generic as the only bpftool provider.
apply_noble_variant() {
	local d="$1"

	sed -i -E 's/^ bpftool \[linux-any\] \| linux-tools-generic \[linux-any\],$/ linux-tools-generic [linux-any],/' \
		"$d/control"
	if grep -q -E '^ bpftool[ ,(\[]' "$d/control" ||
		! grep -q -E '^ linux-tools-generic \[linux-any\],$' "$d/control"; then
		echo "ERROR: noble variant did not apply cleanly" >&2
		exit 1
	fi
}

# Prepend a throwaway changelog entry naming the series.
add_series_entry() {
	local d="$1" series="$2" ver="$3" note="$4"
	local tmp

	tmp=$(mktemp)
	{
		printf 'intp (%s) %s; urgency=medium\n\n' "$ver" "$series"
		printf '  * PPA build for %s.%s\n\n' "$series" "$note"
		printf ' -- %s  %s\n\n' "$MAINT" "$(date -R)"
		cat "$d/changelog"
	} >"$tmp"
	mv "$tmp" "$d/changelog"
}

build_one() {
	local series="$1" orig_flag="$2"
	local ver note=""

	rm -rf debian
	cp -a packaging/debian debian

	if [ "$series" = none ]; then
		ver="$VERSION"
	else
		ver="${VERSION}~ppa${PPA_REV}~ubuntu$(series_version "$series").1"
		if [ "$series" = jammy ]; then
			apply_jammy_variant debian
			note=" The eBPF backends are not built on 22.04 (libbpf 0.5);"
			note+=$'\n'"    the pure-C engine serves all seven metrics."
		elif [ "$series" = noble ]; then
			apply_noble_variant debian
			note=" bpftool comes from linux-tools-generic on 24.04."
		fi
		add_series_entry debian "$series" "$ver" "$note"
	fi

	echo "==> ${series}: intp ${ver} (${orig_flag})"
	# Source-only. -d skips build-dep checks: source packages need only
	# the packaging tools, and dev hosts may carry meson via pipx.
	dpkg-buildpackage -S "$orig_flag" "${SIGN_ARGS[@]}" -d
	BUILT+=("../intp_${ver}_source.changes")
}

BUILT=()
orig_flag="$FIRST_ORIG"
IFS=',' read -r -a SERIES <<<"$SERIES_LIST"
for s in "${SERIES[@]}"; do
	if [ "$s" != none ] && ! series_version "$s" >/dev/null; then
		echo "ERROR: unknown series '$s' (jammy, noble, resolute, none)" >&2
		exit 2
	fi
	build_one "$s" "$orig_flag"
	orig_flag="-sd"
done

echo
echo "==> artifacts:"
ls -la "$ORIG" "${BUILT[@]}"
echo
echo "==> next: lintian -EvIL +pedantic <changes>; then"
echo "    dput ppa:norohim/intp-staging ${BUILT[*]}"
[ "$SIGNED" = 1 ] || echo "==> NOTE: built UNSIGNED (test mode); rerun with --signed for upload"
