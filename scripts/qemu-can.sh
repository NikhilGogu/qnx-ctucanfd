#!/bin/bash
# qemu-can.sh <target-dir> [host-can-if]
# Relaunch a mkqnximage QEMU target with the exact arguments mkqnximage used,
# plus a QEMU CAN bus, the CTU CAN FD PCI board (ctucan_pci) and a bridge to a
# host SocketCAN interface. Stops a QEMU of this target that is still running
# first. Runs daemonised; pidfile stays output/qemu.pid so `mkqnximage --stop`
# and qtarget keep working.
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

# A QEMU of this image that still runs (for example the one `mkqnximage
# --run` started, which may not have written its pid file yet, so that
# `mkqnximage --stop` misses it) holds the disk lock. mkqnximage starts QEMU in
# the target directory, so stop every QEMU working there, and the one in the
# pid file (a QEMU this script started is daemonised and works in /), until
# none has been seen for a second.
P=$(cat "$D/output/qemu.pid" 2>/dev/null || true)
quiet=0; tries=0
while [ $quiet -lt 5 ]; do
    found=0
    for p in $(pgrep -f '^qemu-system-' || true); do
        if [ "$p" = "$P" ] || [ "$(readlink "/proc/$p/cwd" 2>/dev/null)" = "$D" ]; then
            [ $tries = 0 ] && echo "stopping QEMU pid $p, which runs this target"
            kill "$p" 2>/dev/null || true; found=1
        fi
    done
    if [ $found = 1 ]; then quiet=0; tries=$((tries + 1)); else quiet=$((quiet + 1)); fi
    [ $tries -lt 50 ] || { echo "QEMU in $D does not stop" >&2; exit 1; }
    sleep 0.2
done

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
