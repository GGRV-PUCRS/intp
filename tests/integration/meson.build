# tests/integration/meson.build — CLI-level smoke tests against the
# `intp` binary as built. Each test invokes the binary with specific
# flags and asserts on its exit code (and, optionally, its output).

# `intp --version` exits 0 and prints something sensible.
test('cli/version-exits-0',
     find_program('sh'),
     args: ['-c', '@0@ --version | grep -q "^intp "'.format(meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
)

# `intp --help` exits 0 and describes the tool.
test('cli/help-exits-0',
     find_program('sh'),
     args: ['-c',
            '@0@ --help | grep -q "interference profiler"'.format(
                meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
)

# `intp --print-schema` exits 0 and lists the seven core metrics.
test('cli/print-schema-lists-7-core-metrics',
     find_program('sh'),
     args: ['-c',
            '@0@ --print-schema | grep -c "system.interference\\." | grep -qx 7'.format(
                meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
)

# Live tests (require root + real kernel) live under live/ and are
# opted into with `meson test --suite live`. They are intentionally
# *not* added to the default suite. Until live/ has cases this is
# a placeholder.
# subdir('live')

# Graceful degradation: on a host WITHOUT resctrl, the RDT-backed
# metrics must be (a) absent from the JSONL stream — never zero — and
# (b) explained in --print-schema. On resctrl-capable hosts the test
# SKIPs (exit 77): the positive RDT path belongs to the remote gate.
test('cli/degradation-no-resctrl',
     find_program('sh'),
     args: ['-c', '''
        BIN=@0@
        [ -d /sys/fs/resctrl/mon_groups ] && exit 77
        OUT=$($BIN -i 200ms -d 1 2>/dev/null)
        echo "$OUT" | grep -q "system.interference.cpu" || exit 1
        echo "$OUT" | grep -q "system.interference.mbw" && exit 1
        SCHEMA=$($BIN --print-schema)
        echo "$SCHEMA" | grep -A4 "system.interference.mbw" | grep -q "available.: false" || exit 1
        exit 0
     '''.format(meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
)

# Experimental fences (ADR-0008): --target-vm and --portable-metrics
# refuse to run without --experimental, exit 2, and the refusal names
# the gate flag on stderr.
test('cli/target-vm-fenced-without-experimental',
     find_program('sh'),
     args: ['-c', '''
        BIN=@0@
        ERR=$($BIN --target-vm tap0 2>&1 >/dev/null)
        RC=$?
        [ $RC -eq 2 ] || exit 1
        echo "$ERR" | grep -q ..experimental || exit 1
        exit 0
     '''.format(meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
)

test('cli/portable-metrics-fenced-without-experimental',
     find_program('sh'),
     args: ['-c', '''
        BIN=@0@
        ERR=$($BIN --portable-metrics 2>&1 >/dev/null)
        RC=$?
        [ $RC -eq 2 ] || exit 1
        echo "$ERR" | grep -q ..experimental || exit 1
        exit 0
     '''.format(meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
)

# Contract guard (ADR-0008 no-contamination): with the portable set
# enabled, tsv-legacy-v1 still emits EXACTLY the seven frozen columns —
# the portable values ride only in the namespaced formats.
test('cli/portable-metrics-keep-tsv-legacy-7-columns',
     find_program('sh'),
     args: ['-c', '''
        BIN=@0@
        OUT=$($BIN --experimental --portable-metrics -i 200ms -d 1 -f tsv-legacy-v1 2>/dev/null)
        RC=$?
        [ $RC -eq 0 ] || exit 1
        [ -n "$OUT" ] || exit 1
        echo "$OUT" | awk -F"	" "NF != 7 { bad = 1 } END { exit bad }" || exit 1
        exit 0
     '''.format(meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
)
