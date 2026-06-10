intp_cli_sources = files(
        'main.c',
)

# The CLI links the internal static archive rather than the shared
# library: it reaches the in-tree output-sink registry, which is not
# part of the public ABI (revisited in the sink-dispatch phase).
executable(
        'intp',
        intp_cli_sources,
        dependencies: intp_internal_dep,
        install: true,
)
