#!/bin/bash
# test-suite.sh [target] [host-if] [peer]
# Runs everything on the build box: builds the normal and fault-injection
# variants, runs gate-test.sh and every test in scripts/tests/ctutest.py
# against the QEMU target, and keeps the output in build/test-results-*.txt.
# With a peer (a second target started with scripts/qemu-can.sh on the same
# host interface) the two-node test runs too; without one it is skipped.
# Needs: scripts/host-vcan.sh done, target started with scripts/qemu-can.sh.
set -uo pipefail
TGT=${1:-tc}; IF=${2:-vcan0}; PEER=${3:-}
D=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$D/build"
OUT=$D/build/test-results-$(date -u +%Y%m%dT%H%M%SZ).txt
exec > >(tee "$OUT") 2>&1
echo "# $(date -u '+%F %T UTC') target $TGT, peer ${PEER:-none}, host interface $IF, commit ${COMMIT:-unknown}"
ip -d link show "$IF" | sed -n 1p
source ~/qnx800/qnxsdp-env.sh >/dev/null
make -s -C "$D" && make -s -C "$D" FI=1 || { echo "FAIL: build"; exit 2; }
${QTARGET:-~/bin/qtarget} "$TGT" ssh "uname -a"
[ -z "$PEER" ] || ${QTARGET:-~/bin/qtarget} "$PEER" ssh "uname -a"
rc=0
"$D/scripts/gate-test.sh" "$TGT" "$IF" || rc=1
"$D/scripts/tests/ctutest.py" --target "$TGT" --if "$IF" ${PEER:+--peer "$PEER"} || rc=1
echo "# results: $OUT, exit $rc"
exit $rc
