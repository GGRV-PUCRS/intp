# tests/integration/meson.build — CLI-level smoke tests against the
# `intp` binary as built. Each test invokes the binary with specific
# flags and asserts on its exit code (and, optionally, its output).

# `intp --version` exits 0 and prints something sensible.
test('cli/version-exits-0',
     find_program('sh'),
     args: ['-c', '@0@ --version | grep -q "^intp "'.format(meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
     depends: intp_exe,
)

# `intp --help` exits 0 and describes the tool.
test('cli/help-exits-0',
     find_program('sh'),
     args: ['-c',
            '@0@ --help | grep -q "interference profiler"'.format(
                meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
     depends: intp_exe,
)

# `intp --print-schema` exits 0 and lists the seven core metrics.
test('cli/print-schema-lists-7-core-metrics',
     find_program('sh'),
     args: ['-c',
            '@0@ --print-schema | grep -c "system.interference\\." | grep -qx 7'.format(
                meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
     depends: intp_exe,
)

# Plugin loader (ADR-0004): with the hello-world example built, loading it
# via --plugin-dir publishes its metric (app.hello.tick, starting at 0,
# proving init + private storage + the sample loop), and --disable-plugin
# suppresses it. Gated on with_examples (the .so must exist).
if get_option('with_examples')
        test('cli/plugin-loads-and-publishes',
             find_program('sh'),
             args: ['-c', '''
                BIN=@0@
                DIR=@1@
                OUT=$($BIN --plugin-dir "$DIR" -i 50ms -d 160ms -f jsonl 2>/dev/null)
                echo "$OUT" | grep -q '"app.hello.tick":0' || exit 1
                $BIN --plugin-dir "$DIR" --disable-plugin hello-world -i 20ms -d 1ms -f jsonl 2>/dev/null | grep -q "app.hello.tick" && exit 1
                exit 0
             '''.format(meson.project_build_root() / 'src/cli/intp',
                        meson.project_build_root() / 'examples/plugin-hello-world')],
             suite: 'integration',
             depends: [intp_exe, hello_world_plugin],
        )
endif

# Live tests (require root + real kernel) live under live/ and are
# opted into with `meson test --suite live`. They are intentionally
# *not* added to the default suite. Until live/ has cases this is
# a placeholder.
# subdir('live')

# Graceful degradation: on a host WITHOUT resctrl, the RDT-backed
# metrics must be (a) absent from the JSONL stream — never zero — and
# (b) explained in --print-schema. On resctrl-capable hosts the test
# SKIPs (exit 77): the positive RDT path belongs to the remote gate.
# mbw is pinned to its RDT backend: as root the uncore memory-controller
# counters (perf_uncore_imc and its AMD/ARM peers) would otherwise serve
# it, and the test is about the RDT path, not about mbw as a whole.
test('cli/degradation-no-resctrl',
     find_program('sh'),
     args: ['-c', '''
        BIN=@0@
        [ -d /sys/fs/resctrl/mon_groups ] && exit 77
        RDT_ONLY="--disable-backend perf_uncore_imc --disable-backend perf_amd_df --disable-backend perf_arm_cmn"
        OUT=$($BIN $RDT_ONLY -i 200ms -d 1 2>/dev/null)
        echo "$OUT" | grep -q "system.interference.cpu" || exit 1
        echo "$OUT" | grep -q "system.interference.mbw" && exit 1
        SCHEMA=$($BIN $RDT_ONLY --print-schema)
        echo "$SCHEMA" | grep -A4 "system.interference.mbw" | grep -q "available.: false" || exit 1
        exit 0
     '''.format(meson.project_build_root() / 'src/cli/intp')],
     suite: 'integration',
     depends: intp_exe,
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
     depends: intp_exe,
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
     depends: intp_exe,
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
     depends: intp_exe,
)
