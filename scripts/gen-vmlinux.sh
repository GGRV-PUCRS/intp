#!/bin/sh
# SPDX-License-Identifier: MIT
#
# gen-vmlinux.sh — produce the vmlinux.h the BPF objects compile
# against (step 1 of the v3.3 build pipeline, ported from
# across-intp variants/v3.3-ebpf-core-cgroup/Makefile @ 2e87b30fc762).
#
# Prefer dumping the build host's own BTF (exact match for the running
# kernel); fall back to the checked-in dump for hosts without
# /sys/kernel/btf/vmlinux (packaging chroots, Launchpad builders).
# CO-RE relocations make either source load-time portable.
#
# usage: gen-vmlinux.sh BPFTOOL FALLBACK_VMLINUX_H OUTPUT

set -eu

bpftool="$1"
fallback="$2"
output="$3"

if [ -r /sys/kernel/btf/vmlinux ] &&
	"$bpftool" btf dump file /sys/kernel/btf/vmlinux format c \
		> "$output" 2>/dev/null; then
	exit 0
fi

cp "$fallback" "$output"
