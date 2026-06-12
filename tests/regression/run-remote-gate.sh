#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# run-remote-gate.sh — the release-blocking RDT validation gate.
#
# Runs the gates that only an RDT-capable host can close, against the
# Hetzner Sapphire Rapids testbed (or any resctrl-capable target).
#
#   DO NOT run this while the testbed is busy with a research
#   campaign. Coordinate with the operator first. Every step is
#   minutes, not hours; cleanup traps remove workloads, the scratch
#   tree, and any intp_v2_* resctrl mon_groups this run created.
#
# Usage:
#   tests/regression/run-remote-gate.sh user@host [step...]
#
#   steps (default: all release-blocking, i.e. 1-6):
#     1 preflight    capability matrix (expect resctrl/RDT, BTF, libbpf)
#     2 build        meson setup + compile + unit/integration suites
#     3 live         root live suite (eBPF load-attach + ctxsw
#                    amplification gate <= 1.10)
#     4 smoke        7/7-metric smoke + multi-domain resctrl sanity +
#                    RMID crash-reap check
#     5 parity       cross-variant parity vs the in-repo v2.1 / v3.3
#                    binaries (release-blocking; nets exempt
#                    per-cgroup; blk compared within model family)
#     6 attribution  per-cgroup attribution validation (heavy/idle
#                    ratio >= 8, idle <= 5%, conservation <= 25%)
#     7 ub22         OPTIONAL, human-coordinated: reboot to the
#                    kernel-5.15 leg and re-run 2-4
#     8 experimental OPTIONAL, non-blocking: portable-metrics + VM
#                    target first-pass validation (C26/C27)
#
# Steps 1-6 green => stamp v0.9.0 (tag + finalize debian/changelog).
# Any red => fix in intp (never in across-intp) and re-run the failed
# step; each step is independently re-runnable.
#
# Environment:
#   INTP_REMOTE_SCRATCH   remote scratch dir (default /root/intp-gate)
#   INTP_REMOTE_NO_PUSH   1 = do NOT rsync; validate an EXISTING checkout
#                         at ${SCRATCH}/intp that the operator keeps
#                         current by other means (e.g. git pull on the
#                         box). Saves the working-tree transfer when the
#                         remote already tracks the branch under test.
#                         Set SCRATCH=/root to point at /root/intp.
#   INTP_ACROSS_DIR       remote across-intp checkout for step 5
#                         (default /root/across-intp; read-only use)
#   INTP_SSH_OPTS         extra ssh options (e.g. -i ~/.ssh/id_ed25519)

set -euo pipefail

TARGET="${1:?usage: run-remote-gate.sh user@host [step...]}"
shift
STEPS=("${@:-1 2 3 4 5 6}")
[ $# -eq 0 ] && STEPS=(1 2 3 4 5 6)

SCRATCH="${INTP_REMOTE_SCRATCH:-/root/intp-gate}"
ACROSS="${INTP_ACROSS_DIR:-/root/across-intp}"
SSH="ssh ${INTP_SSH_OPTS:-} ${TARGET}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

log() { printf '\n=== [remote-gate] %s ===\n' "$*"; }

remote() { ${SSH} "set -euo pipefail; $*"; }

push_tree() {
	if [ "${INTP_REMOTE_NO_PUSH:-0}" = "1" ]; then
		log "no-push: validating existing ${TARGET}:${SCRATCH}/intp"
		remote "test -e ${SCRATCH}/intp/meson.build" || {
			echo "ERROR: ${SCRATCH}/intp is not an intp checkout" >&2
			exit 1
		}
		remote "cd ${SCRATCH}/intp && \
			echo 'checkout HEAD:' \$(git rev-parse --short HEAD 2>/dev/null || echo '?') \
			'clean='\$(test -z \"\$(git status --porcelain 2>/dev/null)\" && echo yes || echo no)"
		return 0
	fi
	log "rsync working tree -> ${TARGET}:${SCRATCH}/intp"
	rsync -a --delete --exclude=.git --exclude=build --exclude='build-*' \
		-e "ssh ${INTP_SSH_OPTS:-}" \
		"${REPO_ROOT}/" "${TARGET}:${SCRATCH}/intp/"
}

cleanup_remote() {
	log "cleanup: stray workloads + leaked mon_groups"
	remote "pkill -f 'stress-ng.*intp-gate' 2>/dev/null || true
		for g in /sys/fs/resctrl/mon_groups/intp_v2_*; do
			[ -d \"\$g\" ] && rmdir \"\$g\" 2>/dev/null || true
		done"
}
trap cleanup_remote EXIT

step1_preflight() {
	log "step 1: preflight"
	remote "uname -r
		test -f /sys/kernel/btf/vmlinux && echo 'BTF: ok'
		mount | grep -q resctrl || mount -t resctrl resctrl /sys/fs/resctrl
		grep -q . /sys/fs/resctrl/info/L3_MON/mon_features
		echo 'resctrl mon_features:' \
			\$(cat /sys/fs/resctrl/info/L3_MON/mon_features | tr '\n' ' ')
		echo 'num_rmids:' \$(cat /sys/fs/resctrl/info/L3_MON/num_rmids)
		pkg-config --modversion libbpf
		command -v clang bpftool meson ninja stress-ng >/dev/null &&
			echo 'toolchain: ok'
		# leftover state check before measuring anything
		ls /sys/fs/resctrl/mon_groups/ | grep -v '^\$' || echo 'mon_groups: clean'"
}

step2_build() {
	log "step 2: build + default test suites"
	push_tree
	remote "cd ${SCRATCH}/intp
		rm -rf build
		meson setup build
		meson compile -C build
		meson test -C build --print-errorlogs"
}

step3_live() {
	log "step 3: live suite (eBPF load-attach + amplification gate)"
	remote "cd ${SCRATCH}/intp
		meson test -C build --suite live --print-errorlogs"
}

step4_smoke() {
	log "step 4: 7/7 smoke + resctrl sanity + RMID crash-reap"
	remote "cd ${SCRATCH}/intp
		BIN=./build/src/cli/intp
		\$BIN --print-schema
		OUT=\$(\$BIN -d 10 -i 1 -f jsonl)
		for m in netp nets blk mbw llcmr llcocc cpu; do
			echo \"\$OUT\" | grep -q \"system.interference.\$m\" ||
				{ echo \"MISSING \$m\"; exit 1; }
		done
		echo '7/7 metrics present'
		echo \"domains: \$(ls /sys/fs/resctrl/mon_data | grep -c mon_L3)\"
		# RMID crash-reap: kill a -p run mid-flight, confirm the next
		# run reaps the leaked group
		stress-ng --cpu 1 --timeout 30 &
		SPID=\$!
		\$BIN -p \$SPID -i 1 -d 30 -f jsonl >/dev/null &
		IPID=\$!
		sleep 3 && kill -9 \$IPID
		sleep 1
		LEAKED=\$(ls /sys/fs/resctrl/mon_groups/ | grep -c intp_v2_ || true)
		echo \"leaked groups after SIGKILL: \$LEAKED (expect >= 1)\"
		\$BIN -p \$SPID -i 1 -d 3 -f jsonl >/dev/null
		AFTER=\$(ls /sys/fs/resctrl/mon_groups/ | grep -c intp_v2_ || true)
		kill \$SPID 2>/dev/null || true
		echo \"groups after reaping run: \$AFTER (expect 0)\"
		[ \"\$AFTER\" -eq 0 ]"
}

step5_parity() {
	log "step 5: cross-variant parity vs v2.1 / v3.3 (release-blocking, cgroup mode)"
	# v3.3 (ebpf-core-cgroup) only counts under a cgroup target -- it
	# silent-zeros system-wide -- and per-cgroup resctrl monitoring
	# cannot run concurrently across arms (RMID contention), so xval runs
	# the arms sequentially against ONE workload cgroup. Set it up here.
	remote "cd ${SCRATCH}/intp
		test -d ${ACROSS} || { echo 'across-intp checkout missing'; exit 1; }
		CG=/sys/fs/cgroup/intp-xval-gate
		mkdir -p \$CG
		echo '+cpu +memory +io' > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
		trap 'rmdir \$CG 2>/dev/null || true' EXIT
		INTP_XVAL_ACROSS_DIR=${ACROSS} INTP_XVAL_CGROUP=\$CG \
			tests/regression/xval.sh --cross ./build/src/cli/intp"
}

step6_attribution() {
	log "step 6: per-cgroup attribution validation"
	remote "cd ${SCRATCH}/intp
		tests/regression/validate-attribution.sh ./build/src/cli/intp"
}

step7_ub22() {
	log "step 7: ub22 5.15 leg — HUMAN-COORDINATED REBOOT REQUIRED"
	echo "Reboot the box to the Ubuntu 22.04 / kernel 5.15 entry, then"
	echo "re-run: $0 ${TARGET} 2 3 4"
}

step8_experimental() {
	log "step 8: experimental (non-blocking): portable metrics + VM target"
	remote "cd ${SCRATCH}/intp
		BIN=./build/src/cli/intp
		\$BIN --experimental --portable-metrics -d 10 -i 1 -f jsonl |
			tail -3
		echo 'portable smoke done (adjudication: bench/analyze-portable.py in across-intp)'"
}

for s in "${STEPS[@]}"; do
	case "$s" in
	1) step1_preflight ;;
	2) step2_build ;;
	3) step3_live ;;
	4) step4_smoke ;;
	5) step5_parity ;;
	6) step6_attribution ;;
	7) step7_ub22 ;;
	8) step8_experimental ;;
	*) echo "unknown step: $s" >&2; exit 2 ;;
	esac
done

log "requested steps complete"
