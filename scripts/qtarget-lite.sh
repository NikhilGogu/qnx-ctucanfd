#!/bin/bash
# qtarget-lite.sh NAME ssh [cmd...] | put LOCAL REMOTE | get REMOTE LOCAL | ip
# Minimal stand-in for the qtarget helper the test scripts call: the target
# is a mkqnximage QEMU image in ${TARGETS:-~/targets}/NAME, reached as root
# over ssh at the address mkqnximage --getip reports. Needs the SDP
# environment for mkqnximage.
set -uo pipefail
N=${1:?name}; C=${2:?command}; shift 2
D=${TARGETS:-$HOME/targets}/$N
O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes)
IP=$(cd "$D" && mkqnximage --getip 2>/dev/null | tail -1)
case $C in
  ip)  echo "$IP" ;;
  ssh) ssh "${O[@]}" "root@$IP" "$@" ;;
  put) scp -q "${O[@]}" "$1" "root@$IP:$2" ;;
  get) scp -q "${O[@]}" "root@$IP:$1" "$2" ;;
  *)   echo "unknown command $C" >&2; exit 2 ;;
esac
