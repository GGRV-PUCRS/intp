# In-tree backends for the seven core metrics (ADR-0001). Each backend
# registers into the "intp_backends" linker section; adding a file here
# is the whole integration step.
backend_sources = files(
        'blk/blk_stable.c',
        'common/perfev.c',
        'common/procutil.c',
        'common/resctrl.c',
        'cpu/cpu_procfs.c',
        'llcmr/llcmr_perf.c',
        'llcocc/llcocc_stable.c',
        'mbw/mbw_stable.c',
        'netp/netp_stable.c',
        'nets/nets_softirq.c',
)

# eBPF backends (ADR-0003): higher-priority candidates layered on top
# of the pure-C chains above. The subdir defines ebpf_sources (loader +
# five shims), ebpf_gen_sources (generated intp_agg.skel.h) and
# ebpf_inc; src/lib/meson.build folds them into intp_internal. The
# guard keeps a -Dwith_ebpf=disabled build entirely BPF-free.
ebpf_sources = []
ebpf_gen_sources = []
ebpf_inc = []
if ebpf_enabled
        subdir('ebpf')
endif

# Portable-metrics in-tree provider (ADR-0009): pure stable-ABI C, no
# new deps, folded into intp_internal unconditionally — it is inert
# unless opened with experimental+portable_metrics (ADR-0008). The
# sources live under src/plugins/portable-metrics/; meson's subdir()
# cannot traverse '..', so they are listed here by relative path.
portable_sources = files(
        '../plugins/portable-metrics/portable.c',
)
