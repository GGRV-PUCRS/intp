# eBPF backends (ADR-0003): opt-in higher-priority candidates for
# netp/nets/blk/cpu/llcmr. Only entered when ebpf_enabled (see
# src/backends/meson.build). Build pipeline ported from across-intp
# variants/v3.3-ebpf-core-cgroup/Makefile @ 2e87b30fc762:
#
#   1. vmlinux.h     bpftool btf dump of the build host's BTF, or the
#                    checked-in vmlinux/x86_64/vmlinux.h fallback
#                    (scripts/gen-vmlinux.sh decides at build time).
#   2. BPF object    clang -g -O2 -Wall -target bpf -D__TARGET_ARCH_*
#   3. skeleton      bpftool gen skeleton -> intp_agg.skel.h, consumed
#                    by ebpf_loader.c.
#
# The loader + shim sources are appended to backend_sources, so they
# land in intp_internal exactly like the pure-C backends.

bpf_clang = find_program('clang')

# bpftool: Ubuntu's /usr/sbin/bpftool is a wrapper that fails when the
# linux-tools package for the RUNNING kernel is absent, so probe each
# candidate with `bpftool version` (the v3.3 Makefile's BPFTOOL
# detection) and fall back to the newest versioned bpftool under
# /usr/lib/linux-tools/.
bpftool_probe = run_command(
        'sh', '-c',
        '''for c in bpftool /usr/sbin/bpftool \
                $(ls -1 /usr/lib/linux-tools/*/bpftool 2>/dev/null \
                        | sort -V | tail -1); do
                if "$c" version >/dev/null 2>&1; then
                        command -v "$c"
                        exit 0
                fi
        done
        exit 1''',
        check: true,
)
bpftool = find_program(bpftool_probe.stdout().strip())
gen_vmlinux = find_program('../../../scripts/gen-vmlinux.sh')

# Map the host CPU to the BPF target arch define (v3.3 Makefile's
# UNAME_M switch).
bpf_arch_map = {
        'x86_64'  : 'x86',
        'aarch64' : 'arm64',
        'arm'     : 'arm',
}
bpf_arch = bpf_arch_map.get(host_machine.cpu_family(),
                            host_machine.cpu_family())

ebpf_vmlinux_h = custom_target(
        'vmlinux.h',
        input: 'vmlinux' / host_machine.cpu_family() / 'vmlinux.h',
        output: 'vmlinux.h',
        command: [gen_vmlinux, bpftool, '@INPUT@', '@OUTPUT@'],
)

# BPF CFLAGS taken from the v3.3 Makefile (BPF_CFLAGS = -g -O2 -Wall
# plus -target bpf -D__TARGET_ARCH_$(BPF_ARCH) and the include paths).
ebpf_bpf_obj = custom_target(
        'intp_agg.bpf.o',
        input: 'intp_agg.bpf.c',
        output: 'intp_agg.bpf.o',
        depend_files: files('intp_agg.bpf.h'),
        depends: ebpf_vmlinux_h,
        command: [
                bpf_clang, '-g', '-O2', '-Wall',
                '-target', 'bpf',
                '-D__TARGET_ARCH_' + bpf_arch,
                '-I', meson.current_build_dir(),
                '-I', meson.current_source_dir(),
                '-c', '@INPUT@', '-o', '@OUTPUT@',
        ],
)

ebpf_skel_h = custom_target(
        'intp_agg.skel.h',
        input: ebpf_bpf_obj,
        output: 'intp_agg.skel.h',
        command: [bpftool, 'gen', 'skeleton', '@INPUT@'],
        capture: true,
)

# include_directories('.') exposes both the source dir (intp_agg.bpf.h)
# and its build-dir mirror, where the generated intp_agg.skel.h lands.
ebpf_inc = include_directories('.')

ebpf_sources = files(
        'blk_ebpf.c',
        'cpu_ebpf.c',
        'ebpf_loader.c',
        'llcmr_ebpf.c',
        'netp_ebpf.c',
        'nets_ebpf.c',
)

# The generated skeleton header rides along as a source so ninja orders
# the bpftool steps before compiling ebpf_loader.c.
ebpf_gen_sources = [ebpf_skel_h]
