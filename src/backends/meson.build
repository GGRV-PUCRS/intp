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
