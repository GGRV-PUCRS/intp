libintp_sources = files(
        'libintp.c',
        'probe.c',
        'prometheus.c',
)

# Conditional link deps: libbpf for the eBPF runtime probe, libmicrohttpd
# for the Prometheus exposition server. Both populate libintp1's runtime
# Depends automatically via dh_shlibdeps when the corresponding option
# is enabled.
libintp_deps = []
# dlopen(3) for the plugin loader (ADR-0004).
libintp_deps += cc.find_library('dl', required: false)
if ebpf_enabled
        libintp_deps += libbpf_dep
endif
if prometheus_enabled
        libintp_deps += mhd_dep
        # The serve path runs the sampling loop on a background thread.
        libintp_deps += dependency('threads')
endif

# Internal static archive: the whole implementation (public wrappers,
# core registry/dispatcher, backends, output sinks). It exists so that
# (a) the shared library link_whole's it, and (b) unit tests and the
# CLI can link it directly — link_whole keeps the otherwise-unreferenced
# backend registration objects, whose linker-section entries the
# registry walks at runtime (ADR-0001).
intp_internal = static_library(
        'intp-internal',
        libintp_sources + core_sources + backend_sources + output_sources
                + portable_sources + ebpf_sources + ebpf_gen_sources,
        include_directories: [intp_include] + (ebpf_enabled ? [ebpf_inc] : []),
        c_args: ['-fvisibility=hidden'],
        pic: true,
        dependencies: libintp_deps,
)

# meson accepts only numeric X.Y.Z here; a pre-release suffix such as
# 0.10.0-beta1 names the same library file as the release it precedes.
libintp = library(
        'intp',
        version: meson.project_version().split('-')[0],
        soversion: '0',
        include_directories: intp_include,
        gnu_symbol_visibility: 'hidden',
        link_whole: intp_internal,
        dependencies: libintp_deps,
        install: true,
)

libintp_dep = declare_dependency(
        link_with: libintp,
        include_directories: intp_include,
)

# For the CLI and registry unit tests: full internal symbol access plus
# retained backend registration sections.
intp_internal_dep = declare_dependency(
        link_whole: intp_internal,
        include_directories: intp_include,
        dependencies: libintp_deps,
)
