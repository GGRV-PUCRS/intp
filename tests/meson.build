# tests/meson.build — top-level test orchestration.
#
# Three suites, all run by `meson test -C build` by default:
#
#   - unit         hermetic, fast, no privileges, no real hardware.
#   - plugin-abi   loads examples/plugin-hello-world via dlopen and
#                  asserts the descriptor contract.
#   - integration  CLI-level smoke tests that exercise the actual
#                  intp binary against canned fixtures.
#
# A fourth suite (`live`) lives under tests/integration/live/ and
# requires root + a real kernel; it is opt-in via
# `meson test --suite live` and is not run by default.

# Common helper sources (fixture loader, tiny assertion macros).
test_common_inc = include_directories('common')

subdir('unit')
subdir('plugin-abi')
subdir('integration')

# live suite: the eBPF regression gates ported from intp-comparison
# v3.3 (tests/regression/*.sh). They need root, a BTF kernel, and
# stress-ng, so the root meson.build registers a default test setup
# that excludes them — `meson test` stays green on laptops and CI.
# Run them explicitly with
#     meson test -C build --setup= --suite live
# (the empty --setup= bypasses the excluding default setup), or via
# the remote RDT gate. INTP_AMPL_SECONDS shortens the amplification
# window for local runs (default 90 s).
test('live/ebpf-load-attach',
     find_program('regression/test-load-attach.sh'),
     args: [meson.project_build_root() / 'src/cli/intp'],
     suite: 'live',
     timeout: 300,
)

test('live/ebpf-no-ctxsw-amplification',
     find_program('regression/test-no-ctxsw-amplification.sh'),
     args: [meson.project_build_root() / 'src/cli/intp'],
     suite: 'live',
     timeout: 300,
)
