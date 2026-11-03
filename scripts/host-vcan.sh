#!/bin/bash
# host-vcan.sh [if]  Create a CAN FD capable virtual CAN interface on the host.
# Ubuntu cloud kernels ship vcan in linux-modules-extra-$(uname -r).
set -euo pipefail
IF=${1:-vcan0}
sudo modprobe vcan
sudo modprobe can_raw
ip link show "$IF" >/dev/null 2>&1 || sudo ip link add dev "$IF" type vcan
sudo ip link set "$IF" mtu 72   # CANFD_MTU, so QEMU's bridge enables CAN FD frames
sudo ip link set up "$IF"
ip -d link show "$IF"
