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
