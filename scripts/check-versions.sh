#!/bin/bash
# SPDX-License-Identifier: MIT
#
# check-versions.sh — fail when the four version sources disagree:
# meson.build, packaging/debian/changelog (upstream part), the newest
# released CHANGELOG.md heading, and CITATION.cff. Pre-release suffixes
# compare in git-tag form (0.10.0-rc1 == Debian 0.10.0~rc1).

set -eu
cd "$(dirname "$0")/.."

meson=$(sed -n "s/^[[:space:]]*version: '\([^']*\)',/\1/p" meson.build | head -1)
deb=$(sed -n '1s/^intp (\([^)]*\)).*/\1/p' packaging/debian/changelog)
deb=${deb%-*}
deb=${deb//\~/-}
changelog=$(sed -n 's/^## \[\([0-9][^]]*\)\].*/\1/p' CHANGELOG.md | head -1)
citation=$(sed -n 's/^version: "\(.*\)"/\1/p' CITATION.cff)

printf 'meson.build       %s\ndebian/changelog  %s\nCHANGELOG.md      %s\nCITATION.cff      %s\n' \
	"$meson" "$deb" "$changelog" "$citation"

if [ "$meson" = "$deb" ] && [ "$meson" = "$changelog" ] &&
	[ "$meson" = "$citation" ]; then
	echo "OK: all four version sources agree on $meson"
else
	echo "ERROR: version sources disagree" >&2
	exit 1
fi
