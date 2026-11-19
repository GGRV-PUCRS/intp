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
#     9 guest        release-blocking from 0.10.0 when INTP_GUEST_SSH is
#                    set (then part of the default run): inside the KVM
#                    guest, mbw must read unavailable, llcocc must be the
#                    miss-ratio proxy or unavailable (never an RDT
#                    reading, never a bare 0), and llcmr must name its
#                    event family (ADR-0012)
#
# Steps 1-6 (and 9 with a guest) green => the release may be tagged.
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
#   INTP_GUEST_SSH        step 9: command run ON THE HOST to reach the
#                         guest, e.g. "ssh root@192.168.122.10". The guest
#                         keeps an intp checkout of the commit under test
#                         at INTP_GUEST_SCRATCH (default /root/intp).

set -euo pipefail

TARGET="${1:?usage: run-remote-gate.sh user@host [step...]}"
shift
STEPS=("${@:-1 2 3 4 5 6}")
[ $# -eq 0 ] && STEPS=(1 2 3 4 5 6)
[ $# -eq 0 ] && [ -n "${INTP_GUEST_SSH:-}" ] && STEPS+=(9)
GUEST_SCRATCH="${INTP_GUEST_SCRATCH:-/root/intp}"

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
	# Preserve the exit code that triggered the trap: a passing run
	# must exit 0, not be clobbered by the cleanup ssh (which can
	# return 255 on a transient connection blip after a long run).
	local rc=$?
	log "cleanup: stray workloads + leaked mon_groups"
	remote "pkill -f 'stress-ng.*intp-gate' 2>/dev/null || true
		for g in /sys/fs/resctrl/mon_groups/intp_v2_*; do
			[ -d \"\$g\" ] && rmdir \"\$g\" 2>/dev/null || true
		done" >/dev/null 2>&1 || true
	exit "$rc"
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

step9_guest() {
	log "step 9: in-guest semantics (release-blocking with INTP_GUEST_SSH)"
	[ -n "${INTP_GUEST_SSH:-}" ] || {
		echo "ERROR: step 9 needs INTP_GUEST_SSH (host-side command to reach the guest)" >&2
		exit 1
	}
	# Build in the guest, then assert on the host side: the schema and
	# one short JSONL run come back over the nested ssh.
	remote "${INTP_GUEST_SSH} 'cd ${GUEST_SCRATCH} &&
			echo guest HEAD: \$(git rev-parse --short HEAD 2>/dev/null || echo ?) &&
			systemd-detect-virt &&
			rm -rf build && meson setup build >/dev/null &&
			meson compile -C build >/dev/null' &&
		${INTP_GUEST_SSH} '${GUEST_SCRATCH}/build/src/cli/intp --print-schema' \
			>/tmp/intp-guest-schema.json &&
		${INTP_GUEST_SSH} '${GUEST_SCRATCH}/build/src/cli/intp -d 3 -i 1 -f jsonl' \
			>/tmp/intp-guest-samples.jsonl &&
		python3 - /tmp/intp-guest-schema.json /tmp/intp-guest-samples.jsonl <<'PY'
import json, sys
schema = json.load(open(sys.argv[1]))['metrics']
rows = [json.loads(l) for l in open(sys.argv[2]) if l.strip()]
m = lambda k: schema['system.interference.' + k]
bad = []
if m('mbw').get('available'):
    bad.append('mbw available in a guest (backend %s)' % m('mbw').get('backend'))
occ = m('llcocc')
if occ.get('available') and occ.get('backend') != 'proxy_from_miss_ratio':
    bad.append('llcocc backend %s in a guest (expect proxy or unavailable)' % occ.get('backend'))
mr = m('llcmr')
if mr.get('available') and mr.get('event_family') not in ('arch_cache_refs', 'll_read', 'vendor_raw'):
    bad.append('llcmr without event_family')
for r in rows:
    vals = r.get('metrics', {})
    if 'system.interference.mbw' in vals:
        bad.append('mbw emitted in a guest sample: %r' % vals['system.interference.mbw'])
        break
print('guest llcmr family:', mr.get('event_family'), '| llcocc:', occ.get('backend'),
      '| mbw available:', m('mbw').get('available'), '| samples:', len(rows))
if not rows:
    bad.append('no JSONL samples from the guest')
for b in bad:
    print('FAIL:', b)
sys.exit(1 if bad else 0)
PY"
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
	9) step9_guest ;;
	*) echo "unknown step: $s" >&2; exit 2 ;;
	esac
done

log "requested steps complete"
