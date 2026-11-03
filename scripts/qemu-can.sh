#!/bin/bash
# qemu-can.sh <target-dir> [host-can-if]
# Relaunch a mkqnximage QEMU target with the exact arguments mkqnximage used,
# plus a QEMU CAN bus, the CTU CAN FD PCI board (ctucan_pci) and a bridge to a
# host SocketCAN interface. Runs daemonised; pidfile stays output/qemu.pid so
# `mkqnximage --stop` and qtarget keep working.
set -euo pipefail
D=$(cd "${1:?target dir, e.g. ~/targets/tc}" && pwd)
IF=${2:-vcan0}
LOG=${QEMU_LOG:-$D.log}

CMD=$(grep '^QEMU Command:' "$LOG" | tail -1 | sed 's/^QEMU Command: //')
[ -n "$CMD" ] || { echo "no 'QEMU Command:' line in $LOG" >&2; exit 1; }
# -daemonize refuses -nographic; the serial console already goes to a file.
CMD=${CMD//-nographic/}

if ! ip link show "$IF" >/dev/null 2>&1; then
    echo "host CAN interface $IF missing (see scripts/host-vcan.sh)" >&2; exit 1
fi

P=$(cat "$D/output/qemu.pid" 2>/dev/null || true)
if [ -n "$P" ] && kill -0 "$P" 2>/dev/null; then
    echo "target already running (pid $P); stop it first" >&2; exit 1
fi

cd "$D"
# Both CTU CAN FD cores sit on one bus. The bus id must not be a bare
# "canbus0": QEMU then reports the link path as ambiguous. QEMU refuses to realise the board
# unless both canbusN links are set.
eval "$CMD" -display none -daemonize \
    -monitor unix:"$D/output/monitor.sock",server=on,wait=off \
    -object can-bus,id=canbus0-bus \
    -object can-host-socketcan,id=canhost0,if="$IF",canbus=canbus0-bus \
    -device ctucan_pci,canbus0=canbus0-bus,canbus1=canbus0-bus
echo "started pid $(cat "$D/output/qemu.pid") with ctucan_pci on canbus0 <-> $IF"
