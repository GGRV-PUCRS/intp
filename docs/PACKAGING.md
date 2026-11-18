# Packaging `intp` for distributions

This document is the playbook for getting `intp` from a tagged release
into a distribution's package repository. It is written from the
perspective of the upstream maintainer. If you are a distro packager
adopting `intp`, this is also the document that should answer your
questions about the project's packaging promises.

The big-picture roadmap (which distro, in what order, against which
gates) lives in [ROADMAP.md](ROADMAP.md). This document is the *how*.

---

## Project promises to packagers

We commit to:

- **Reproducible source releases.** Each tag produces a
  `intp-X.Y.Z.tar.gz` that builds identically from a clean checkout
  on any reasonable build host, for at least five years from the
  release date.
- **Stable build inputs.** No optional dependency surprises within a
  major version: if `1.0.0` builds against `libbpf ≥ 0.8`, every
  `1.x` release also does.
- **Conventional install layout.** `meson install` honours `--prefix`,
  `--libdir`, and the standard FHS paths. No "magic" install steps.
- **DEP-5 / SPDX-correct copyright** in the upstream tarball.
- **Manpages** at `share/man/man1/intp.1` and friends, for `intp`
  itself and any in-tree binary plugins.
- **A versioned `libintp` ABI** post-1.0, with `abidiff` gating in CI.
- **A maintained `MAINTAINERS` file** so packagers know who to ping.

We ask packagers to:

- Use the system `libbpf-dev`, `libmicrohttpd-dev`, etc. rather than
  vendoring; we have no out-of-tree forks of those.
- File any packaging quirks via the
  [distro-packaging issue template](../.github/ISSUE_TEMPLATE/distro-packaging.yml).
- Not ship `intp-plugin-*` packages for plugins we have not blessed in
  [PLUGINS.md](PLUGINS.md) under the same name; pick a vendor-prefixed
  name to avoid namespace collisions.

---

## Artefacts shipped per distro

The base build produces these artefacts (sub-packages where the distro
allows):

| Artefact                | Contents                                                        |
|-------------------------|-----------------------------------------------------------------|
| `intp`                  | The CLI binary and manpage. Depends on `libintp0`.              |
| `libintp0`              | The shared library `libintp.so.1`. Runtime-linkable.            |
| `libintp-dev`           | Public headers, `intp.pc`, static lib (if built).               |
| `intp-doc`              | The `docs/` tree as installed `share/doc/intp/`.                |
| `intp-plugin-bpftrace`  | The bpftrace research plugin.                                   |
| `intp-plugin-gpu-nvml`  | NVIDIA GPU plugin (often a *contrib* / *non-free* repo).        |
| `intp-plugin-gpu-rocm`  | AMD GPU plugin.                                                 |
| `intp-plugin-gpu-zes`   | Intel GPU plugin.                                               |
| `intp-plugin-db-*`      | Database plugins (one sub-package per database).                |

A static-binary release (`intp-static-x86_64.tar.gz`) is attached to
each GitHub release as a fallback for users on distros where `intp`
hasn't landed yet. It is not the recommended distribution path.

---

## Debian (and Ubuntu)

### Source layout

`packaging/debian/` follows standard `debhelper`-13 conventions:

```
packaging/debian/
├── control                # source + binary stanzas
├── changelog              # Debian changelog (separate from upstream CHANGELOG.md)
├── copyright              # DEP-5, lists vendored deps if any
├── rules                  # dh-based, very short
├── source/format          # 3.0 (quilt) or (native), TBD
├── intp.install
├── libintp0.install
├── libintp-dev.install
├── intp-doc.install
├── intp.manpages
├── intp-collector.service # NOT installed in 0.9.x — example only
│                          # (packaging/systemd/; ship + harden at 1.0)
├── intp.lintian-overrides # only with explicit justification
└── tests/                 # autopkgtest entries
```

`debian/rules` uses `dh $@ --buildsystem=meson --parallel`. No
patches needed for the upstream tarball — it builds straight.

### Build dependencies

`debian/control` `Build-Depends:` includes:

```
debhelper-compat (= 13),
meson,
ninja-build,
pkg-config,
clang,
libbpf-dev (>= 0.8),
linux-tools-generic | bpftool,
libelf-dev,
zlib1g-dev,
libmicrohttpd-dev,
python3,
```

Optional builds (`gpu-nvml`, `otel`, etc.) get their own
`Build-Depends:` lines under separate `Build-Profiles:` so they don't
gate the base build.

### Lintian

We aim for **zero errors** and minimal, justified warnings. The
acceptance bar for the personal PPA, mentors.debian.net, and the
NEW-queue review.

Common warnings we currently override:

- `package-contains-no-arch-dependent-files` for `intp-doc` (correct
  for a `Architecture: all` doc package).

Anything else gets an `intp.lintian-overrides` entry only if the
maintainer is confident it is wrong, with a comment explaining why.

### Personal PPA workflow

Human-gated: pushes, tags and uploads are performed by the maintainer,
never by automation. Two PPAs are in play:

| PPA | Purpose |
| --- | --- |
| `ppa:norohim/intp` | What users install. Receives only binaries copied from staging after they passed there. |
| `ppa:norohim/intp-staging` | Release candidates and every first build of a release. Version numbers are checked per PPA, so candidates never burn a version in the main PPA. |

Both build amd64 and arm64. One orig tarball serves three series; each
series gets its own source upload, versioned with a `~ppaN~ubuntuYY.MM.1`
suffix that sorts below the eventual archive version:

| Kind | Git tag | Source version (noble) |
| --- | --- | --- |
| Final | `v0.10.0` | `0.10.0-1~ppa1~ubuntu24.04.1` |
| Release candidate | `v0.10.0-rc1` | `0.10.0~rc1-1~ppa1~ubuntu24.04.1` (staging only) |
| Packaging-only fix | none | `0.10.0-1~ppa2~ubuntu24.04.1` |
| Other series | none | `...~ubuntu22.04.1` (jammy), `...~ubuntu26.04.1` (resolute) |

```bash
# one-time: dpkg expects debian/ at the source root
ln -s packaging/debian debian

# binary build + lintian locally
dpkg-buildpackage -us -uc
lintian -EvIL +pedantic ../intp_0.10.0-1_amd64.changes

# source packages for jammy, noble and resolute from one orig tarball
# (after the remote gate passes and the tag is stamped on main)
packaging/make-source-package.sh --signed
dput ppa:norohim/intp-staging ../intp_0.10.0-1~ppa1~ubuntu*_source.changes
```

**Rehearsal before staging.** `packaging/check-series-build.sh SERIES`
reproduces a PPA build without Launchpad: it cuts that series' source
package from committed `HEAD` with `make-source-package.sh`, rebuilds it
from the `.dsc` with first-alternative Build-Depends resolution, installs
the binaries and runs the autopkgtest smoke commands. It installs
packages, so it only runs as root in a throwaway container of the
target series:

```bash
podman run --rm -v /dev/null:/sys/kernel/btf/vmlinux:ro \
    -v "$PWD":/src:ro ubuntu:24.04 \
    bash -c 'cd /src && packaging/check-series-build.sh noble'
```

The `/dev/null` mount hides the host BTF, as on a builder. The
`ppa-rehearsal` workflow runs the same script for all six series x
architecture pairs on packaging changes, on `main` and on `v*` tags.
A green rehearsal does not replace staging (the archive snapshot and
builder differ), but it catches series-specific dependency failures
without burning a version.

Once all six builds (3 series x 2 architectures) succeed in staging and
pass the smoke test, copy the binaries to `ppa:norohim/intp` with
Launchpad's *Copy packages* (same series, no rebuild), so users get
exactly what was tested. Never copy a binary across series.

**Burned versions.** Launchpad never accepts a version, or an orig
tarball name with different content, twice. `0.1.0~placeholder-1` (two
failed builds, bad orig tarball), `0.1.0~placeholder2-1` and
`0.2.0~placeholder-1` are used forever in the main PPA. `0.9.0~rc1-1`
and `0.9.0-1` were gated but never uploaded; their tags are
upstream-only history.

**Tarball discipline.** Cut the orig tarball once per upstream version
with `make-source-package.sh`, attach it to the GitHub release with
`SHA256SUMS`, and fetch it from there for later series or rebuilds:
`git archive` output is not guaranteed byte-identical across git
versions.

**libintp1 → libintp0.** The 0.2.0 placeholder published the binary
package `libintp1` on noble, with the same library paths as
`libintp0`. `libintp0` (the right name for SONAME 0) carries
`Breaks`/`Replaces: libintp1 (<< 0.10.0~)`, so `apt upgrade` swaps them.
Test that path in staging: install the placeholder from the main PPA,
add staging, upgrade.

**Tagging.** Every `chore(release)` commit on `main` gets an annotated
`v<version>` tag at release time; pre-releases use `-rc1`-style
suffixes because `~` is illegal in git refnames (`debian/watch`
mangles the suffix back to `~rc1` for Debian ordering). The tags
`v0.1.0-placeholder2` and `v0.2.0-placeholder` mark PPA states;
`v0.9.0-rc1` and `v0.9.0` mark gated commits that were never
published. `release.yml` fires on any `v*` tag.

Notes for the Launchpad build environment:

- builders have no `/sys/kernel/btf/vmlinux`; the checked-in
  `src/backends/ebpf/vmlinux/<arch>/vmlinux.h` fallbacks (selected by
  `scripts/gen-vmlinux.sh`) are what keep `-Dwith_ebpf=enabled`
  building there — test the masked-BTF path before uploading.
- `jammy` (22.04) cannot build eBPF from its archive (libbpf 0.5 <
  0.8, no standalone `bpftool`, and Launchpad resolves only the first
  alternative of a Build-Depends entry). `make-source-package.sh`
  applies a jammy variant to its temporary `debian/` copy: no clang,
  bpftool or libbpf-dev, and `-Dwith_ebpf=disabled`. The binary stays
  schema-compatible per ADR-0003.
- `noble` (24.04) has no real `bpftool` package: the name is virtual,
  provided by `linux-tools-common` and `linux-lowlatency-tools-common`,
  which carry at most the `/usr/sbin/bpftool` wrapper. The noble variant
  names `linux-tools-generic` alone, which installs a versioned
  `/usr/lib/linux-tools/<kver>/bpftool` that the meson probe finds.
  `packaging/debian/control` keeps `bpftool` first for Debian.

---

## Fedora / RHEL / CentOS Stream

### Source layout

`packaging/rpm/intp.spec` is a single-file spec.

```
%global meson_options -Dwith_tests=true -Dwith_examples=false

Name:           intp
Version:        0.1.0
Release:        1%{?dist}
Summary:        Linux interference profiler
License:        MIT
URL:            https://github.com/ggrv-intp/intp
Source0:        %{url}/archive/v%{version}/intp-%{version}.tar.gz

BuildRequires:  meson ninja-build clang
BuildRequires:  libbpf-devel bpftool
BuildRequires:  elfutils-libelf-devel zlib-devel
BuildRequires:  libmicrohttpd-devel
...

%package        libs
Summary:        Shared library for %{name}

%package        devel
Summary:        Development files for %{name}
```

Sub-packages mirror the Debian split (`intp-libs`, `intp-devel`,
`intp-doc`, plus per-plugin sub-packages).

### COPR → official Fedora

1. Push the spec + tarball to a COPR project owned by the maintainer.
2. Once COPR builds clean for current Fedora and EPEL, file a
   [Fedora package review](https://docs.fedoraproject.org/en-US/packaging-guidelines/).
3. Address review feedback; on approval, the package enters Fedora
   rawhide and stable releases follow.

EPEL builds (RHEL/AlmaLinux/Rocky) are derived from the Fedora
package via the standard `fedpkg` workflow.

---

## Arch Linux

### Source layout

`packaging/arch/PKGBUILD` is the standard PKGBUILD.

```bash
pkgname=intp
pkgver=0.1.0
pkgrel=1
pkgdesc="Linux interference profiler"
arch=('x86_64' 'aarch64')
url='https://github.com/ggrv-intp/intp'
license=('MIT')
depends=('libbpf' 'libelf' 'zlib' 'libmicrohttpd')
makedepends=('meson' 'ninja' 'clang' 'bpf')
source=("$url/archive/v$pkgver/$pkgname-$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
    cd "$pkgname-$pkgver"
    meson setup build --buildtype=release --prefix=/usr
    meson compile -C build
}

check() {
    cd "$pkgname-$pkgver"
    meson test -C build
}

package() {
    cd "$pkgname-$pkgver"
    meson install -C build --destdir="$pkgdir"
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
```

### AUR → official `extra`

1. Submit `intp` to AUR.
2. After it has been on AUR with active users for a release cycle or
   two, request a Trusted User to adopt it into the `extra`
   repository. Some TU activity is unavoidable here; the
   [Arch packaging guidelines](https://wiki.archlinux.org/title/Arch_package_guidelines)
   are the binding spec.

---

## Alpine

`packaging/alpine/APKBUILD` follows Alpine conventions. Two
gotchas:

- **musl-clean:** any glibc-only call site (e.g. `strlcpy` was
  glibc-only until recently) breaks here first. CI runs an Alpine
  build to catch this.
- **No SystemTap:** confirm at the `APKBUILD` level that we never
  pull in stap-related deps even transitively.

Submit via the standard `aports` workflow.

---

## Nix / NixOS

`packaging/nix/default.nix` is a standard `mkDerivation`. A `flake.nix`
lands post-1.0. The hermetic nature of Nix builds is a forcing
function for build-determinism cleanups; we welcome bug reports here.

---

## Static-binary release

GitHub Actions release workflow builds:

- `intp-static-x86_64-linux.tar.gz` — fully static, glibc-linked.
- `intp-static-aarch64-linux.tar.gz` — same, cross-built.
- `intp-static-x86_64-linux-musl.tar.gz` — musl-static.

These are signed with a release-time GPG key (the public key is in
the GitHub release notes) and are intended as *fallbacks*. Users
running these miss out on distro-managed updates; we recommend the
distro package whenever possible.

---

## When the upstream version doesn't match the distro version

Distros do not always update at the same cadence. We commit to:

- Backporting *security fixes* to the previous LTS-compatible major
  version for the lifetime of the supported Debian / Ubuntu LTS.
- Clearly tagging which `intp` version corresponds to which kernel
  ABI assumptions in [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) and
  [`docs/METRICS.md`](METRICS.md).
- Not requiring a kernel newer than what the distro itself ships in
  any given LTS.

If a distro's shipped `intp` is incompatible with a feature you need,
the static binary or building from source remains an option. The
project will not ship "newer than the distro's kernel" requirements
in a way that breaks the distro package.

---

## Reach goals (post-1.0)

- **Snap** package — for users who want auto-updating but live on
  older LTS releases.
- **Flatpak** — less obviously a fit for a tool that needs host
  capabilities, but the integration model with Flatpak's permission
  system is worth exploring.
- **Container image** — a minimal `intp:latest` for use as a
  Kubernetes DaemonSet image, mirroring the Helm chart in
  `integrations/k8s/`.

These are not on the critical path; they will land as PRs from
maintainers who care about a specific channel.
