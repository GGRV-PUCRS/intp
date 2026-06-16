intp_cli_sources = files(
        'main.c',
        'serve_uri.c',
)
intp_cli_deps = [intp_internal_dep]

# The OTLP/HTTP+JSON exporter is compiled into the CLI only, so libcurl
# stays out of libintp.so's dependency closure (ADR-0011).
if otel_enabled
        intp_cli_sources += files('otlp.c', 'otlp_json.c')
        intp_cli_deps += curl_dep
endif

# The CLI links the internal static archive rather than the shared
# library: it reaches the in-tree output-sink registry, which is not
# part of the public ABI (revisited in the sink-dispatch phase).
# The target object is named so the integration tests can declare a
# build dependency on it (they invoke the binary by path, which meson
# cannot see — without `depends:` a fresh `meson test` would run them
# against a not-yet-built binary).
intp_exe = executable(
        'intp',
        intp_cli_sources,
        dependencies: intp_cli_deps,
        install: true,
)
