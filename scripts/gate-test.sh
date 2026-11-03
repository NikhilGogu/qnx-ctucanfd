#!/bin/bash
# gate-test.sh [target] [host-if]
# Run on the build box after `make` and scripts/qemu-can.sh: deploys the
# driver to a qtarget target and exchanges CAN / CAN FD frames with the host.
set -uo pipefail
TGT=${1:-tc}; IF=${2:-vcan0}; N=${BURST:-400}
Q=~/bin/qtarget
B=$(cd "$(dirname "$0")/.." && pwd)/build/x86_64
fail() { echo "FAIL: $*"; exit 1; }

# a running binary cannot be overwritten (EBUSY), so stop the old driver first
$Q "$TGT" ssh "slay -f dev-can-ctucanfd canfd-test >/dev/null 2>&1; sleep 0.3"
# the old binary stays busy until its process has exited, so retry the copy
for f in dev-can-ctucanfd canfd-test; do
    for i in 1 2 3 4 5; do $Q "$TGT" put "$B/$f" /tmp/$f && break; sleep 1; done || fail "put $f"
done
$Q "$TGT" ssh "/tmp/dev-can-ctucanfd >/tmp/drv.log 2>&1 </dev/null; cat /tmp/drv.log" || fail "driver start"

echo "== QNX -> host ($IF)"
LOG=$(mktemp); timeout 10 candump -x -n 2 "$IF" > "$LOG" 2>&1 & CD=$!
sleep 0.5
$Q "$TGT" ssh "/tmp/canfd-test send /dev/can1/tx1 123 64 && /tmp/canfd-test send /dev/can1/tx1 7DF 8 0201050000000000" >/dev/null || fail "send"
wait $CD; cat "$LOG"
grep -q " 123  \[64\]  00 01 02" "$LOG" || fail "64-byte CAN FD frame not seen on $IF"
grep -q "7DF   \[8\]  02 01 05" "$LOG" || fail "classic frame not seen on $IF"

echo "== host -> QNX (/dev/can1/rx0)"
$Q "$TGT" ssh "rm -f /tmp/rx.log; (/tmp/canfd-test recv /dev/can1/rx0 2 >/tmp/rx.log 2>&1 &)"
sleep 0.5
cansend "$IF" "456##1$(printf '%02X' $(seq 192 255))"
cansend "$IF" 1ABCDEF0#DEADBEEF
sleep 0.5
R=$($Q "$TGT" ssh "cat /tmp/rx.log"); echo "$R"
echo "$R" | grep -q "456  \[64\]  C0 C1" || fail "64-byte CAN FD frame not received on QNX"
echo "$R" | grep -q "1ABCDEF0  \[04\]  DE AD BE EF" || fail "extended-id frame not received on QNX"

echo "== paced burst: $N x 64-byte CAN FD frames, 1 ms apart"
$Q "$TGT" ssh "rm -f /tmp/burst.log; (/tmp/canfd-test recv /dev/can1/rx0 $N >/tmp/burst.log 2>&1 &)"
sleep 0.5
cangen "$IF" -f -b -L 64 -D i -I 2A0 -g 1 -n "$N"
sleep 1
GOT=$($Q "$TGT" ssh "wc -l < /tmp/burst.log" | tr -d ' ')
$Q "$TGT" ssh "/tmp/canfd-test info /dev/can1/rx0 | grep stats; slay -f canfd-test >/dev/null 2>&1"
[ "$GOT" = "$N" ] || fail "burst: $GOT of $N frames reached the reader"
echo "PASS: 64-byte CAN FD both ways, classic and 29-bit ids, $N/$N burst frames"
