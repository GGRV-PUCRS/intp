test_unit_metrics = executable(
        'test-unit-metrics',
        'test_metrics.c',
        include_directories: [intp_include, test_common_inc],
        link_with: libintp,
        install: false,
)

test('metrics/core-metric-names',
     test_unit_metrics,
     suite: 'unit',
     env: { 'INTP_TEST_FIXTURE_DIR': meson.project_source_root() / 'tests/fixtures' },
)

test_unit_units = executable(
        'test-unit-units',
        'test_units.c',
        include_directories: [intp_include, test_common_inc],
        link_with: libintp,
        install: false,
)

test('metrics/unit-names',
     test_unit_units,
     suite: 'unit',
)

test_unit_version = executable(
        'test-unit-version',
        'test_version.c',
        include_directories: [intp_include, test_common_inc],
        link_with: libintp,
        install: false,
)

test('lib/version-string',
     test_unit_version,
     suite: 'unit',
)

# Registry/dispatcher tests link the internal static archive
# (link_whole) so their fake backends land in the same linker section
# the registry walks; the shared library cannot see test-registered
# backends across the .so boundary.
test_unit_registry = executable(
        'test-unit-registry',
        'test_registry.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('core/registry-selection',
     test_unit_registry,
     suite: 'unit',
)

test_unit_dispatcher = executable(
        'test-unit-dispatcher',
        'test_dispatcher.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('core/dispatcher-sampling',
     test_unit_dispatcher,
     suite: 'unit',
)

test_unit_procutil = executable(
        'test-unit-procutil',
        'test_procutil.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('backends/procutil-parsers',
     test_unit_procutil,
     suite: 'unit',
)

test_unit_detect = executable(
        'test-unit-detect',
        'test_detect.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('core/detect-invariants',
     test_unit_detect,
     suite: 'unit',
)

test_unit_tsv_legacy = executable(
        'test-unit-tsv-legacy',
        'test_tsv_legacy.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('output/tsv-legacy-golden',
     test_unit_tsv_legacy,
     suite: 'unit',
)

test_unit_target = executable(
        'test-unit-target',
        'test_target.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('core/target-parsing',
     test_unit_target,
     suite: 'unit',
)

test_unit_portable = executable(
        'test-unit-portable',
        'test_portable.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('plugins/portable-provider',
     test_unit_portable,
     suite: 'unit',
)

# Compiles the CLI's serve-URI parser directly (it is pure / standalone).
test_unit_serve_uri = executable(
        'test-unit-serve-uri',
        'test_parse_serve_uri.c',
        files(meson.project_source_root() / 'src/cli/serve_uri.c'),
        include_directories: [test_common_inc,
                              include_directories('../../src/cli')],
        install: false,
)

test('cli/parse-serve-uri',
     test_unit_serve_uri,
     suite: 'unit',
)

test_unit_jsonl = executable(
        'test-unit-jsonl',
        'test_jsonl_golden.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('output/jsonl-golden',
     test_unit_jsonl,
     suite: 'unit',
)

test_unit_prometheus_fmt = executable(
        'test-unit-prometheus-fmt',
        'test_prometheus_fmt.c',
        include_directories: [test_common_inc],
        dependencies: intp_internal_dep,
        install: false,
)

test('output/prometheus-fmt',
     test_unit_prometheus_fmt,
     suite: 'unit',
)
