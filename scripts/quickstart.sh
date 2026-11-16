#!/bin/bash
# quickstart.sh [target-name] [host-if]
# From a fresh clone to a checked driver in QEMU in one command, on a Linux
# host with KVM, QEMU with the ctucan_pci board, can-utils, the vcan module
# and QNX SDP 8.0. Steps: host vcan interface, build, a mkqnximage target in
# ${TARGETS:-~/targets}/NAME (made on the first run only), QEMU restarted with
# the CAN board, then scripts/gate-test.sh, which copies the binaries, starts
# the driver and checks frames both ways.
# Settings: QNX_SDP_ENV (default ~/qnx800/qnxsdp-env.sh), SSH_KEY (public key
# for root on the target, default ~/.ssh/id_ed25519.pub), QTARGET, TARGETS.
set -euo pipefail
NAME=${1:-tc}; IF=${2:-vcan0}
D=$(cd "$(dirname "$0")/.." && pwd)
T=${TARGETS:-$HOME/targets}
ENV=${QNX_SDP_ENV:-$HOME/qnx800/qnxsdp-env.sh}
KEY=${SSH_KEY:-$HOME/.ssh/id_ed25519.pub}
export QTARGET=${QTARGET:-$D/scripts/qtarget-lite.sh}
export TARGETS=$T
die() { echo "quickstart: $*" >&2; exit 1; }
step() { echo "== $*"; }

[ -f "$ENV" ] || die "QNX SDP environment $ENV not found (set QNX_SDP_ENV)"
[ -f "$KEY" ] || die "ssh public key $KEY not found (set SSH_KEY)"
command -v candump >/dev/null || die "candump not found: install can-utils"
qemu-system-x86_64 -device help 2>/dev/null | grep -q ctucan_pci || die "this QEMU has no ctucan_pci board"

step "host interface $IF"
"$D/scripts/host-vcan.sh" "$IF" >/dev/null

step "build"
source "$ENV" >/dev/null
make -s -C "$D"

if [ ! -d "$T/$NAME/output" ]; then
    step "QNX image in $T/$NAME (first run only)"
    mkdir -p "$T/$NAME"
    (cd "$T/$NAME" && mkqnximage --type=qemu --arch=x86_64 --ssh-ident="$KEY" --noprompt --run=-h > "$T/$NAME.log")
fi

step "QEMU with the CAN board"
"$D/scripts/qemu-can.sh" "$T/$NAME" "$IF"

step "waiting for ssh on $NAME"
up=0
for _ in $(seq 90); do
    if "$QTARGET" "$NAME" ssh true 2>/dev/null; then up=1; break; fi
    sleep 2
done
[ $up = 1 ] || die "no ssh answer from $NAME"

step "gate test"
"$D/scripts/gate-test.sh" "$NAME" "$IF"
echo "== done: the driver runs on $NAME; try /tmp/canfd-test send /dev/can1/tx1 123 64 there and candump -x $IF here"
