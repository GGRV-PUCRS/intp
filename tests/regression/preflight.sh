#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# preflight.sh -- host capability matrix for the intp regression /
# validation tooling.
#
# Ported from intp-comparison:
#   shared/intp-preflight.sh @ 2e87b30fc762
# REDUCED to what intp needs: the per-variant v0..v3.3 BUILD/RUN verdict
# logic and the SystemTap/debuginfo/bench-env checks are dropped; the
# detection techniques for the remaining capabilities are kept verbatim.
#
# Checked capabilities (one OK/MISSING line each):
#   kernel version (>= 5.8: eBPF CO-RE + cgroup-v2 + perf cgroup-mode
#   baseline), BTF, libbpf, resctrl (compiled + mounted + mon_features +
#   num_rmids), perf_event_paranoid, cgroup v2 unified, stress-ng, plus
#   informational lines (CPU vendor, RDT cpuid flags).
#
# The script never installs anything, never mounts resctrl, never
# changes sysctls.
#
# Exit codes:
#   0  every required capability present
#   2  at least one capability MISSING

set -u
# Note: do NOT use `set -e`. Checks intentionally tolerate missing tools
# and account for that in the verdict (origin convention).

# -----------------------------------------------------------------------------
# Output helpers (origin emit/record, reduced to a MISSING counter)
# -----------------------------------------------------------------------------
if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'
    C_DIM=$'\033[2m';  C_BLD=$'\033[1m'; C_RST=$'\033[0m'
else
    C_RED=""; C_GRN=""; C_DIM=""; C_BLD=""; C_RST=""
fi

N_MISSING=0

emit() {
    local status="$1"; shift
    local color
    case "$status" in
        OK)      color="$C_GRN" ;;
        MISSING) color="$C_RED"; N_MISSING=$((N_MISSING + 1)) ;;
        *)       color="$C_DIM" ;;
    esac
    printf '  %s%-9s%s %s\n' "$color" "[$status]" "$C_RST" "$*"
}

section() { printf '\n%s== %s ==%s\n' "$C_BLD" "$1" "$C_RST"; }

# -----------------------------------------------------------------------------
# A. Kernel + CPU
# -----------------------------------------------------------------------------
KREL=$(uname -r 2>/dev/null || echo 0.0)
KMAJ=$(echo "$KREL" | awk -F'[.-]' '{print $1+0}')
KMIN=$(echo "$KREL" | awk -F'[.-]' '{print $2+0}')

kernel_ge() {
    local rmaj="$1" rmin="$2"
    if [ "$KMAJ" -gt "$rmaj" ]; then return 0; fi
    if [ "$KMAJ" -eq "$rmaj" ] && [ "$KMIN" -ge "$rmin" ]; then return 0; fi
    return 1
}

printf '%sintp preflight%s -- host=%s kernel=%s\n' \
    "$C_BLD" "$C_RST" "$(hostname 2>/dev/null || echo '?')" "$KREL"

section "Kernel + CPU"

# intp baseline: kernel >= 5.8 (eBPF CO-RE + cgroup v2 + PERF_FLAG_PID_CGROUP).
if kernel_ge 5 8; then
    emit OK "kernel $KREL >= 5.8 (CO-RE + cgroup-v2 + perf cgroup-mode baseline)"
else
    emit MISSING "kernel $KREL < 5.8 -- below the intp baseline"
fi

vendor=""
if [ -r /proc/cpuinfo ]; then
    vendor=$(awk -F: '/^vendor_id/ {gsub(/ /,"",$2); print $2; exit}' /proc/cpuinfo)
    [ -z "$vendor" ] && vendor=$(awk -F: '/^CPU implementer/ {print "ARM"; exit}' /proc/cpuinfo)
fi
vendor="${vendor:-unknown}"
emit INFO "cpu vendor $vendor"
emit INFO "arch $(uname -m 2>/dev/null || echo unknown)"

# perf_event_paranoid -- profilers that open IMC/uncore counters need <= 0,
# ideally -1. Root at runtime bypasses this, but the gate hosts pin -1.
if [ -r /proc/sys/kernel/perf_event_paranoid ]; then
    p=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 4)
    if [ "$p" -le 0 ]; then
        emit OK "perf_event_paranoid=$p (mbw/llcmr want -1 for full IMC uncore access)"
    else
        emit MISSING "perf_event_paranoid=$p (>=1 blocks PMU access; set to -1)"
    fi
else
    emit MISSING "perf_event_paranoid not readable"
fi

# -----------------------------------------------------------------------------
# B. Intel RDT / AMD PQoS / resctrl (mbw, llcocc)
# -----------------------------------------------------------------------------
section "Intel RDT / AMD PQoS / resctrl"

flags=""
[ -r /proc/cpuinfo ] && flags=$(awk -F: '/^flags/ {print $2; exit}' /proc/cpuinfo)
echo "$flags" | grep -qw cqm \
    && emit INFO "cpuid cqm flag present" \
    || emit INFO "cpuid cqm flag absent (no RDT exposed)"
echo "$flags" | grep -qw cqm_occup_llc \
    && emit INFO "cqm_occup_llc present (llcocc capable)" \
    || emit INFO "cqm_occup_llc absent (no native llcocc)"
echo "$flags" | grep -qw cqm_mbm_total \
    && emit INFO "cqm_mbm_total present (mbw via resctrl MBM capable)" \
    || emit INFO "cqm_mbm_total absent (mbw must use perf uncore)"

if grep -q resctrl /proc/filesystems 2>/dev/null; then
    emit OK "resctrl compiled into kernel (CONFIG_X86_CPU_RESCTRL)"
else
    emit MISSING "resctrl not compiled in (missing from /proc/filesystems)"
fi

if mountpoint -q /sys/fs/resctrl 2>/dev/null; then
    emit OK "/sys/fs/resctrl mounted"
    if [ -r /sys/fs/resctrl/info/L3_MON/mon_features ]; then
        feat=$(tr '\n' ' ' < /sys/fs/resctrl/info/L3_MON/mon_features)
        emit OK "L3_MON mon_features: $feat"
    else
        emit MISSING "resctrl mounted but L3_MON/mon_features unreadable"
    fi
    if [ -r /sys/fs/resctrl/info/L3_MON/num_rmids ]; then
        emit OK "num_rmids: $(cat /sys/fs/resctrl/info/L3_MON/num_rmids)"
    else
        emit MISSING "resctrl mounted but L3_MON/num_rmids unreadable"
    fi
else
    emit MISSING "/sys/fs/resctrl not mounted (run 'mount -t resctrl resctrl /sys/fs/resctrl')"
    emit MISSING "L3_MON mon_features unavailable (resctrl not mounted)"
    emit MISSING "num_rmids unavailable (resctrl not mounted)"
fi

# -----------------------------------------------------------------------------
# C. BTF (eBPF CO-RE)
# -----------------------------------------------------------------------------
section "BTF (eBPF CO-RE)"
if [ -f /sys/kernel/btf/vmlinux ]; then
    emit OK "/sys/kernel/btf/vmlinux present"
else
    emit MISSING "/sys/kernel/btf/vmlinux missing (kernel needs CONFIG_DEBUG_INFO_BTF=y) -- eBPF backends cannot load"
fi

# -----------------------------------------------------------------------------
# D. libbpf
# -----------------------------------------------------------------------------
section "libbpf"
if pkg-config --exists libbpf 2>/dev/null; then
    emit OK "libbpf $(pkg-config --modversion libbpf 2>/dev/null)"
elif [ -f /usr/include/bpf/libbpf.h ] || [ -f /usr/local/include/bpf/libbpf.h ]; then
    emit OK "libbpf headers present (no pkg-config entry)"
else
    emit MISSING "libbpf-dev not installed (apt install libbpf-dev)"
fi

# -----------------------------------------------------------------------------
# E. cgroup v2 unified hierarchy (per-cgroup attribution)
# -----------------------------------------------------------------------------
section "cgroup v2 unified hierarchy"
is_v2=0
if grep -Eq '(^| )cgroup2 /sys/fs/cgroup ' /proc/mounts 2>/dev/null \
        || grep -Eq 'cgroup2 /sys/fs/cgroup ' /proc/self/mounts 2>/dev/null; then
    is_v2=1
fi
if [ "$is_v2" -eq 1 ] && [ -r /sys/fs/cgroup/cgroup.controllers ]; then
    ctrl=$(tr '\n' ' ' < /sys/fs/cgroup/cgroup.controllers 2>/dev/null)
    emit OK "cgroup2 at /sys/fs/cgroup (controllers: ${ctrl% })"
elif [ "$is_v2" -eq 1 ]; then
    emit MISSING "cgroup2 mounted but cgroup.controllers unreadable"
elif [ -d /sys/fs/cgroup/unified ] || grep -q cgroup2 /proc/mounts 2>/dev/null; then
    emit MISSING "hybrid cgroup layout -- intp -c needs cgroup v2 UNIFIED (systemd.unified_cgroup_hierarchy=1)"
else
    emit MISSING "no cgroup v2 mount (host is cgroup v1 only)"
fi

# -----------------------------------------------------------------------------
# F. Workload generator
# -----------------------------------------------------------------------------
section "Workload generator"
if command -v stress-ng >/dev/null 2>&1; then
    emit OK "stress-ng ($(command -v stress-ng))"
else
    emit MISSING "stress-ng not in PATH (apt install stress-ng)"
fi

# -----------------------------------------------------------------------------
# Summary + exit
# -----------------------------------------------------------------------------
printf '\n%s== Summary ==%s\n' "$C_BLD" "$C_RST"
if [ "$N_MISSING" -eq 0 ]; then
    printf '  all required capabilities present\n'
    exit 0
fi
printf '  %d capability check(s) MISSING\n' "$N_MISSING"
exit 2
