#!/bin/bash
# Copyright 2026 The ctucanfd authors
# SPDX-License-Identifier: Apache-2.0
# run_pass.sh N : one dry run of the codelab from the top, in a new workspace
set -u
N=$1; C=$HOME/cldry; P=$C/pass$N
source ~/qnx800/qnxsdp-env.sh > /dev/null
for t in tcl2 tc2; do
    [ -d ~/targets/$t ] && (cd ~/targets/$t && mkqnximage --stop > /dev/null 2>&1)
    rm -rf ~/targets/$t ~/targets/$t.log
done
rm -rf $P; mkdir -p $P/logs
git config --file $P/gitconfig url."$C/qnx-ctucanfd.bundle".insteadOf https://github.com/NikhilGogu/qnx-ctucanfd.git
python3 $C/gen_pass.py $C/qnx-canfd-qemu.md $P/pass.sh "~/canfd-lab=>$P/canfd-lab" \
    'NODE1=$LAB/node1 NODE2=$LAB/node2=>NODE1=~/targets/tcl2 NODE2=~/targets/tc2' \
    'sudo apt install=>apt-get -s install' 'sudo adduser=>sudo -n adduser'
echo "pass $N start $(date -u +%T)"
env -i HOME=$HOME USER=$USER LOGNAME=$USER TERM=dumb PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    GIT_CONFIG_GLOBAL=$P/gitconfig PASSLOG_DIR=$P/logs timeout 1800 bash $P/pass.sh > $P/logs/pass.log 2>&1
echo "pass $N exit $? end $(date -u +%T)"
for t in tcl2 tc2; do
    [ -d ~/targets/$t ] && (cd ~/targets/$t && mkqnximage --stop > /dev/null 2>&1)
    rm -rf ~/targets/$t ~/targets/$t.log
done
grep -c '^!!!' $P/logs/pass.log; grep -n '^!!!\|^=== ' $P/logs/pass.log
