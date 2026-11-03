#!/bin/bash
# gate-test.sh [target] [host-if]
# Quick end-to-end check on the build box after `make` and scripts/qemu-can.sh:
# deploys the driver to a qtarget target, exchanges CAN and CAN FD frames with
# the host and compares complete frames, not prefixes or line counts.
set -uo pipefail
TGT=${1:-tc}; IF=${2:-vcan0}; N=${BURST:-400}
Q=${QTARGET:-~/bin/qtarget}
B=$(cd "$(dirname "$0")/.." && pwd)/build/x86_64
fail() { echo "FAIL: $*"; exit 1; }

# A running binary cannot be overwritten (EBUSY): stop the old driver (the
# supervisor first, or it starts it again) and retry while it is exiting.
$Q "$TGT" ssh "slay -f -s KILL ctucan-supervisor >/dev/null 2>&1; slay -f dev-can-ctucanfd canfd-test >/dev/null 2>&1; true"
# A copy counts only when the md5 on the target matches the build.
for f in dev-can-ctucanfd canfd-test; do
    want=$(md5sum < "$B/$f" | cut -d' ' -f1); ok=0
    for i in 1 2 3 4 5; do
        if $Q "$TGT" put "$B/$f" /tmp/$f && [ "$($Q "$TGT" ssh "md5sum /tmp/$f" | cut -d' ' -f1)" = "$want" ]; then
            ok=1; break
        fi
        sleep 1
    done
    [ $ok = 1 ] || fail "copy of $f: retries exhausted or md5 mismatch"
    echo "deployed /tmp/$f, md5 $want matches the build"
done
OUT=$($Q "$TGT" ssh '/tmp/dev-can-ctucanfd >/tmp/drv.log 2>&1 </dev/null; rc=$?; cat /tmp/drv.log; exit $rc') \
    || fail "driver start: $OUT"
echo "$OUT"

echo "== QNX -> host ($IF)"
LOG=$(mktemp); timeout 10 candump -L -n 2 "$IF" > "$LOG" 2>&1 & CD=$!
sleep 0.5
$Q "$TGT" ssh "/tmp/canfd-test send /dev/can1/tx1 123 64 && /tmp/canfd-test send /dev/can1/tx1 7DF 8 0201050000000000" \
    >/dev/null || fail "send"
wait $CD; cat "$LOG"
P64=$(printf '%02X' $(seq 0 63))
grep -Eq " 123##[13579BDF]$P64\$" "$LOG" || fail "64-byte CAN FD frame with BRS not seen complete on $IF"
grep -q " 7DF#0201050000000000\$" "$LOG" || fail "classic frame not seen complete on $IF"
rm -f "$LOG"

echo "== host -> QNX (/dev/can1/rx0)"
$Q "$TGT" ssh "rm -f /tmp/rx.log; (/tmp/canfd-test recv /dev/can1/rx0 2 >/tmp/rx.log 2>&1 &)"
sleep 0.5
cansend "$IF" "456##1$(printf '%02X' $(seq 192 255))"
cansend "$IF" 1ABCDEF0#DEADBEEF
sleep 0.5
R=$($Q "$TGT" ssh "cat /tmp/rx.log"); echo "$R"
echo "$R" | grep -qF "/dev/can1/rx0  456  [64]  $(printf '%02X ' $(seq 192 255) | sed 's/ $//')  ts=" \
    || fail "64-byte CAN FD frame not received complete on QNX"
echo "$R" | grep -qF "/dev/can1/rx0  1ABCDEF0  [04]  DE AD BE EF  ts=" || fail "extended-id frame not received complete on QNX"

echo "== paced burst: $N sequenced 64-byte CAN FD frames, 1 ms apart, every byte checked"
$Q "$TGT" ssh "rm -f /tmp/burst.log; (/tmp/canfd-test seqrecv /dev/can1/rx0 $N 30000 >/tmp/burst.log 2>&1 &)"
sleep 0.5
python3 - "$IF" "$N" <<'PY'
import socket, struct, sys, time
s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
s.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
s.bind((sys.argv[1],))
t = time.monotonic()
for q in range(int(sys.argv[2])):
    d = bytes(((q >> (8 * i)) & 255) if i < 4 else ((q + i) & 255) for i in range(64))
    s.send(struct.pack("=IBBBB64s", 0x2A0, 64, 1, 0, 0, d))
    t += 0.001
    time.sleep(max(0, t - time.monotonic()))
PY
sleep 1
R=$($Q "$TGT" ssh "cat /tmp/burst.log; /tmp/canfd-test info /dev/can1/rx0 | grep stats; slay -f canfd-test >/dev/null 2>&1")
echo "$R"
echo "$R" | grep -q "seqrecv: got $N of $N, bad 0, missing 0, out_of_order 0" || fail "burst"
echo "PASS: 64-byte CAN FD both ways, classic and 29-bit ids, $N/$N burst frames checked byte for byte"
