id: qnx-canfd-qemu
title: Test CAN FD software on QNX 8.0 in QEMU, no hardware needed
summary: Boot QNX OS 8.0 in QEMU with an emulated CTU CAN FD board, run a native QNX CAN FD driver for it, exchange 64-byte frames with Linux SocketCAN tools, run a simulated ECU, kill the driver and watch a supervisor bring it back, and join two QNX nodes on one virtual CAN bus.
categories: qnx, automotive, codelabs
tags: intermediate
difficulty: 3
status: published
authors: Nikhil Reddy Gogu, Yashaswin Puranik
feedback_link: https://github.com/NikhilGogu/qnx-ctucanfd/issues


# Test CAN FD software on QNX 8.0 in QEMU, no hardware needed

## What you will build
Duration: 2:00

CAN FD software for QNX usually needs a CAN FD board on your desk. QEMU can emulate one: `ctucan_pci` is a PCI board with the CTU CAN FD IP core, an open source CAN FD controller. In this codelab you boot QNX OS 8.0 in QEMU with that board, run **dev-can-ctucanfd**, a QNX driver for the core built on the QNX CAN Driver Development Kit (libcan), and connect the emulated CAN bus to Linux SocketCAN on your machine, so the usual Linux CAN tools see every frame.

```text
 QNX OS 8.0 in QEMU (node 1)    your Ubuntu machine     QNX OS 8.0 in QEMU (node 2)
 ecu-sim, canfd-test            candump, cansend        canfd-test tester
 dev-can-ctucanfd                                       dev-can-ctucanfd
 ctucan_pci board  <--------->  vcan0 (SocketCAN)  <--------->  ctucan_pci board
```

**What you will do:**

* Create a virtual CAN FD interface on your machine
* Boot QNX OS 8.0 in QEMU with an emulated CTU CAN FD PCI board
* Build and start the driver, and send 64-byte CAN FD frames both ways
* Run a simulated battery ECU that sends telemetry every 10 ms and answers a diagnostic request
* Kill the driver while the ECU is sending, and watch a supervisor start it again
* Add a second QNX node on the same bus and check every frame that passes between the two

**What you will learn about QNX:** a CAN driver on QNX is an ordinary process. libcan makes it a resource manager: applications open `/dev/can1/rx0` and `/dev/can1/tx1` and read and write frames with `devctl()`. When the driver dies, the kernel, the applications and your shell keep running, the process manager tells a supervisor, and the supervisor starts the driver again.

Everything in this codelab was run as written on Ubuntu 22.04 with QNX SDP 8.0 and QEMU 6.2. The outputs are from that run, trimmed: long lines end in `...`, and paths in your home directory start with `~`. Your addresses, process ids and times will differ. QEMU models no CAN bit timing, so the times you see are how fast QEMU and QNX move frames on your machine, not how long a frame takes on a real bus.

---

## Before you begin
Duration: 4:00

You need:

1. **QNX SDP 8.0 with a free non-commercial licence.** If you do not have one yet, follow the QNX codelab [Get a Free Non-Commercial QNX 8.0 License](https://qnx.github.io/codelabs/get-non-commercial-qnx-license/), then install QNX SDP 8.0 with the QNX Software Center. This codelab assumes it is in `~/qnx800`, so that `~/qnx800/qnxsdp-env.sh` exists.
2. **Ubuntu (22.04 or later, x86_64) with KVM.** Install QEMU, libvirt (mkqnximage targets get their address from libvirt's default network), the Linux CAN tools and the kernel module package that holds `vcan`:

    ```bash
    sudo apt install qemu-system-x86 libvirt-daemon-system can-utils git make \
        openssh-client linux-modules-extra-$(uname -r)
    sudo adduser $USER kvm
    sudo adduser $USER libvirt
    ```

    Log out and in again after `adduser`. Then check the network, let mkqnximage's own script allow QEMU to use libvirt's `virbr0` bridge, and check that your QEMU has the CAN FD board:

    ```bash
    virsh -c qemu:///system net-list
    ~/qnx800/host/common/mkqnximage/qemu/check-net
    qemu-system-x86_64 -device ctucan_pci,help
    ```

    `virsh` should list `default` as `active`. `check-net` prints nothing when QEMU may already use `virbr0`; otherwise it asks once for your sudo password. The last command lists the board's properties:

    ```text
    ctucan_pci options:
      acpi-index=<uint32>    -  (default: 0)
      ...
      canbus0=<link<can-bus>>
      canbus1=<link<can-bus>>
      ...
    ```

3. **An SSH key without a passphrase** at `~/.ssh/id_ed25519`. mkqnximage installs its public half for root on the target. If you have none: `ssh-keygen -t ed25519 -N '' -f ~/.ssh/id_ed25519`.

> aside negative
>
> The commands in this codelab run as root on QNX virtual machines and use `sudo` on your machine to create a virtual CAN interface. Use a machine where that is allowed.

---

## Get the code and build it
Duration: 3:00

Everything this codelab creates goes into `~/canfd-lab`. Start with a small file that every terminal you open will read: it sets up the QNX SDP environment, names the two target directories, and defines `on` (run a command as root on a target) and `put` (copy files to `/tmp` on a target).

```bash
mkdir -p ~/canfd-lab
cat > ~/canfd-lab/env.sh <<'EOF'
source ~/qnx800/qnxsdp-env.sh > /dev/null
export LAB=~/canfd-lab
export NODE1=$LAB/node1 NODE2=$LAB/node2
SSHOPT="-o UserKnownHostsFile=$LAB/known_hosts -o StrictHostKeyChecking=accept-new"
SSHOPT="$SSHOPT -o LogLevel=ERROR -o ConnectTimeout=5"
ip_of() { (cd $1 && mkqnximage --getip); }
on()  { local d=$1; shift; ssh $SSHOPT root@$(ip_of $d) "$@"; }
put() { local d=$1; shift; scp -q $SSHOPT "$@" root@$(ip_of $d):/tmp/; }
EOF
source ~/canfd-lab/env.sh
```

`ip_of` runs `mkqnximage --getip` in a target's directory, which prints the address libvirt gave that target. The host keys of your QNX targets go into `~/canfd-lab/known_hosts`, so they do not mix with your own.

Now clone the driver and build it:

```bash
cd $LAB
git clone https://github.com/NikhilGogu/qnx-ctucanfd.git
cd qnx-ctucanfd
make
ls build/x86_64
```

```text
canfd-test  ctucan-hangsrv  ctucan-probe  ctucan-supervisor  dev-can-ctucanfd  ecu-sim
```

What you just built with `qcc`, the QNX compiler:

* `dev-can-ctucanfd`: the driver. It links libcan from the SDP statically.
* `canfd-test`: a test client that sends and receives frames through the driver's devices.
* `ecu-sim`: a simulated battery-management ECU.
* `ctucan-supervisor`: starts the driver again when it dies.
* `ctucan-probe`: reads the board's identification registers.
* `ctucan-hangsrv`: a helper for the supervisor's tests; you will not need it here.

Check: `ls build/x86_64` lists the six programs.

---

## Create the virtual CAN bus
Duration: 2:00

On Linux, `vcan` is a virtual CAN interface: every frame one program sends on it reaches every other program listening on it. QEMU will connect its emulated CAN bus to it. The repository has a script for this:

```bash
scripts/host-vcan.sh vcan0
```

```text
9: vcan0: <NOARP,UP,LOWER_UP> mtu 72 qdisc noqueue state UNKNOWN mode DEFAULT ...
    link/can  promiscuity 0 minmtu 0 maxmtu 0
    vcan numtxqueues 1 numrxqueues 1 gso_max_size 65536 gso_max_segs 65535
```

The script loads the `vcan` module, creates `vcan0`, sets its MTU to 72 and brings it up. 72 bytes is the size of a Linux CAN FD frame structure: QEMU passes CAN FD frames to a host interface only when that interface has this MTU. With the classic CAN MTU of 16, 64-byte frames would never leave QEMU.

Check: the first line of the output shows `mtu 72` and `UP`.

---

## Start a QNX target with a CAN FD board
Duration: 5:00

`mkqnximage` is the QNX SDP's script for building QNX images for development. Build an x86_64 image for QEMU in `$NODE1`, with your SSH key for root:

```bash
mkdir -p $NODE1 && cd $NODE1
mkqnximage --type=qemu --arch=x86_64 --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt \
    --run=-h > $NODE1.log
cd $LAB/qnx-ctucanfd
grep '^QEMU Command:' $NODE1.log
```

```text
QEMU Command: qemu-system-x86_64 -smp 2 --enable-kvm --cpu host -m 1G -drive ...
```

`--run=-h` built the image and booted it without a display, and mkqnximage logged the exact QEMU command it used: 2 CPUs, 1 GB of RAM, the disk image, a network card on libvirt's `virbr0` and the QNX kernel image. That command has no CAN hardware. `scripts/qemu-can.sh` stops that QEMU and starts the same command with three more options:

```text
-object can-bus,id=canbus0-bus
-object can-host-socketcan,id=canhost0,if=vcan0,canbus=canbus0-bus
-device ctucan_pci,canbus0=canbus0-bus,canbus1=canbus0-bus
```

The first creates a CAN bus inside QEMU, the second connects that bus to `vcan0` on your machine, and the third adds the CTU CAN FD PCI board. The board has two CAN FD cores, and QEMU refuses to start it unless both are connected to a bus, so both go on the same one. Start the target with them:

```bash
scripts/qemu-can.sh $NODE1 vcan0
until on $NODE1 true; do sleep 2; done
on $NODE1 uname -a
```

```text
stopping QEMU pid 531195, which runs this target
started pid 531331 with ctucan_pci on canbus0 <-> vcan0
QNX noname 8.0.0 2026/06/05-16:26:03EDT x86pc x86_64
```

The `until` loop tries again every 2 seconds until the target answers on SSH; with KVM that took about 15 seconds in our runs. Now copy the programs to the target and look at the board from QNX:

```bash
put $NODE1 $(find build/x86_64 -type f)
on $NODE1 /tmp/ctucan-probe
```

```text
found 1760:ff00 at B0:D5:F0
irq: 10 (of 1)
BAR0: MEM addr=0xfeba0000 size=0x8000
BAR1: MEM addr=0xfeb90000 size=0x10000
BAR0+0x0000 bridge id: 0xc0000002 (cores=2)
core0 DEVICE_ID=0xcafd VERSION=2.2 (raw 0x0202cafd) OK
core0 MODE=0x00000010 STATUS=0x00000084 RX_MEM_INFO=0x08000800 TX_STATUS=0x00008888 ...
core0 FILTER_STATUS=0x0000 filters present: none
core1 DEVICE_ID=0xcafd VERSION=2.2 (raw 0x0202cafd) OK
...
```

The probe found the board through the QNX PCI server, mapped its two memory regions and read each core's registers: `0xCAFD` is the CTU CAN FD device id, version 2.2. `FILTER_STATUS` says which hardware acceptance filters the core has; QEMU's model has none, so the driver filters frames in software.

Check: the probe prints `DEVICE_ID=0xcafd ... OK` for `core0`.

---

## Start the driver
Duration: 3:00

Start the driver as root on the target:

```bash
on $NODE1 /tmp/dev-can-ctucanfd
on $NODE1 ls /dev/can1
```

```text
dev-can-ctucanfd: B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10
dev-can-ctucanfd: CTU CAN FD 2.2, /dev/can1/rx0..rx0 tx1..tx1, CAN FD, 500000 bit/s (brp 4 tq 50), data 2000000 bit/s (brp 1 tq 50) BRS, 4 TXT buffer(s)
rx0
tx1
```

The driver attached to the PCI board, reset core 0, set it up for CAN FD (500 kbit/s nominal, 2 Mbit/s data phase with bit rate switch) and registered two devices: `/dev/can1/rx0` for receiving and `/dev/can1/tx1` for sending. Then it left your shell and kept running in the background, as QNX drivers do. QEMU accepts the bit timing registers but does not model bit timing, so the rates matter only on real hardware.

The driver is a process like any other. Look at its threads:

```bash
on $NODE1 pidin -p dev-can-ctucanfd
on $NODE1 pidin -p dev-can-ctucanfd irqs
```

```text
     pid tid name                         prio STATE          Blocked
  663573   1 tmp/dev-can-ctucanfd          10r RECEIVE        1
  663573   2 tmp/dev-can-ctucanfd         254i INTR
     pid tid name
  663573   1 tmp/dev-can-ctucanfd
  663573   2 tmp/dev-can-ctucanfd
		1            0xa    0 ----- =PULSE 0x40000004:-1 0:0
```

Thread 1 waits in `RECEIVE`: it runs libcan's resource manager loop and receives the messages that clients send when they call `open()`, `devctl()` or `read()` on `/dev/can1/*`. Thread 2, at priority 254, waits in `INTR` for the CAN core's interrupt. The `irqs` view shows the interrupt the driver attached: vector `0xa` (IRQ 10), delivered as a `PULSE`, a small kernel message, so thread 1 handles frames and client requests one after the other in the same loop. This is the QNX microkernel design: the driver is not part of the kernel, it is a server that other processes send messages to.

Check: `ls /dev/can1` shows `rx0` and `tx1`.

---

## Send and receive your first frames
Duration: 5:00

Open a second terminal on your machine for `candump`, the Linux CAN monitor, and leave it running:

```bash
# terminal 2
source ~/canfd-lab/env.sh
candump -t d -x vcan0
```

In your first terminal, send a 64-byte CAN FD frame and a classic 8-byte frame from QNX:

```bash
on $NODE1 /tmp/canfd-test send /dev/can1/tx1 123 64
on $NODE1 /tmp/canfd-test send /dev/can1/tx1 321 8 1122334455667788
```

Terminal 2 shows both frames as they reach `vcan0`:

```text
 (000.000000)  vcan0  TX B -  123  [64]  00 01 02 03 04 05 06 07 08 09 ... 3D 3E 3F
 (000.134120)  vcan0  TX - -  321   [8]  11 22 33 44 55 66 77 88
```

`[64]` is the length, and the `B` says the frame was sent with bit rate switch, so it is a CAN FD frame. candump marks every frame `TX` because on `vcan0` every frame comes from a program on your machine: here QEMU's bridge, later also `cansend`. The driver sends every frame longer than 8 bytes as CAN FD with bit rate switch. `canfd-test` used `CAN_DEVCTL_WRITE_CANMSG_EXT`, the libcan devctl that carries the full 64-byte payload.

Now the other direction. Start a reader on QNX in the background (the `&` is on your machine, so its output appears in this terminal), then send two frames from Linux, one CAN FD frame with 16 bytes and one classic frame with a 29-bit identifier:

```bash
on $NODE1 /tmp/canfd-test recv /dev/can1/rx0 2 &
cansend vcan0 456##1C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF
cansend vcan0 1ABCDEF0#DEADBEEF
wait
```

```text
/dev/can1/rx0  456  [16]  C0 C1 C2 C3 C4 C5 C6 C7 C8 C9 CA CB CC CD CE CF  ts=16199300
/dev/can1/rx0  1ABCDEF0  [04]  DE AD BE EF  ts=16199857
```

In `cansend`, `##1` marks a CAN FD frame with bit rate switch, and an identifier with more than three hex digits is a 29-bit one. `ts` is the time the driver took the frame from the core, in microseconds. If a frame arrives before the reader opens the device, the driver keeps it in the device's queue (64 frames deep), so the reader still gets it.

libcan counts what the driver did:

```bash
on $NODE1 /tmp/canfd-test info /dev/can1/rx0
```

```text
description: CTU CAN FD 2.2 PCI 1760:ff00 core 0 FD
bit_rate: 500000  brp 4 ts1 43 ts2 6 sjw 6
mailboxes: rx 1 tx 1  msgq 64 waitq 16
stats: tx 2 rx 2 irq 4 (rx 2 tx 2) frame_err 0 hw_overflow 0 swq_full 0 warn 0 ...
error: fault 0x00000000 rec_tec 0x00000000 err_cnt 0x00000000 status 0x00000084
```

Two frames sent and two received. `irq 4 (rx 2 tx 2)` counts the interrupts, one for each frame here; two frames that arrive close together can share one, so you may see `irq 3 (rx 1 tx 2)`. Terminal 2 also shows the two frames from `cansend`.

Stop `candump` in terminal 2 with Ctrl+C.

Check: terminal 2 showed `123` with `[64]` and `B`, and the reader printed both frames with all their bytes.

---

## Run a simulated ECU
Duration: 5:00

`ecu-sim` plays a battery-management ECU. A thread with the real-time policy `SCHED_FIFO` at priority 30 sends one 64-byte CAN FD telemetry frame on id `0x18F` every 10 ms; it sleeps until an absolute deadline (`clock_nanosleep()` with `TIMER_ABSTIME`), so the period does not drift. A second thread answers one OBD-II diagnostic request. Start it in the background on the target, with its output in a file there:

```bash
on $NODE1 '/tmp/ecu-sim > /tmp/ecu.log 2>&1 &'
on $NODE1 pidin -p ecu-sim
```

```text
     pid tid name                         prio STATE          Blocked
  954390   1 tmp/ecu-sim                   30f NANOSLEEP
  954390   2 tmp/ecu-sim                   30f REPLY          663573
```

The quotes make the redirection and the `&` happen on the target. Thread 1 sends telemetry and sleeps until its next deadline, so you usually find it in `NANOSLEEP` (priority `30f`, f for FIFO), or in `REPLY` if you catch it handing a frame to the driver. Thread 2 is `REPLY` blocked on the driver's process id: it sent a read request and waits for the driver to answer with a frame. Watch five telemetry frames and the time between them:

```bash
candump -t d -n 5 vcan0,18F:7FF
```

```text
 (000.000000)  vcan0  18F  [64]  0D 00 00 00 8C 00 00 00 34 0E 85 FF 50 61 ... 00 10
 (000.010106)  vcan0  18F  [64]  0E 00 00 00 96 00 00 00 2E 0E 86 FF 50 61 ... 00 98
 (000.010167)  vcan0  18F  [64]  0F 00 00 00 A0 00 00 00 2F 0E 87 FF 50 61 ... 00 1D
 (000.010071)  vcan0  18F  [64]  10 00 00 00 AA 00 00 00 30 0E 88 FF 50 61 ... 00 55
 (000.010116)  vcan0  18F  [64]  11 00 00 00 B4 00 00 00 31 0E 89 FF 50 61 ... 00 CA
```

Bytes 0 to 3 are a sequence number (little endian), bytes 4 to 7 the milliseconds since start, then pack voltage, current, state of charge (`0x50`, 80 %), 16 cell voltages, and in byte 63 a CRC-8 over the other 63 bytes, so a receiver can check every frame. The filter `18F:7FF` makes candump show only id `0x18F`.

Now ask the ECU for its battery's remaining life, OBD-II service 01, PID `0x5B`. Start candump for the request and the answer, then send the request from Linux:

```bash
candump -t d -n 2 vcan0,7DF:7FF,7E8:7FF &
sleep 0.5
cansend vcan0 7DF#02015B0000000000
wait
```

```text
 (000.000000)  vcan0  7DF   [8]  02 01 5B 00 00 00 00 00
 (000.001216)  vcan0  7E8   [8]  03 41 5B CC 00 00 00 00
```

`7E8` is the answer of the first ECU: 3 bytes follow, `41` is service 01's positive response, `5B` the PID, and `CC` (204) is the value, 204 / 255 = 80 %. The answer left QNX about a millisecond after the request entered `vcan0`, through QEMU, the driver, ecu-sim and back.

Check: the deltas between telemetry frames are close to `0.010000`, and `7E8` answered `7DF`.

---

## Kill the driver and let the supervisor restart it
Duration: 5:00

On QNX the driver is a process, so it can crash without taking the system with it. `ctucan-supervisor` starts the driver and starts it again when it dies: it asks the process manager for a notification on every process death (`procmgr_event_notify()`), and it counts the driver as ready only when the driver's own process answers on `/dev/can1/tx1`. Replace the driver you started by hand with a supervised one:

```bash
on $NODE1 slay dev-can-ctucanfd
on $NODE1 '/tmp/ctucan-supervisor -- /tmp/dev-can-ctucanfd > /tmp/sup.log 2>&1 &'
sleep 1
on $NODE1 cat /tmp/sup.log
```

```text
dev-can-ctucanfd: B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10
dev-can-ctucanfd: CTU CAN FD 2.2, /dev/can1/rx0..rx0 tx1..tx1, CAN FD, ...
1793742630.252176 ctucan-supervisor: driver pid 1073172 ready (/dev/can1/tx1 answers GET_INFO) 7.5 ms after spawn
```

`slay` sent SIGTERM, and the driver stopped cleanly. ecu-sim did not stop: its writes failed, and it tries to open its devices again every millisecond until a driver is back.

Now kill the driver the hard way, with SIGKILL, which it cannot catch, while you record the telemetry on Linux:

```bash
candump -L vcan0,18F:7FF > $LAB/kill.log &
sleep 2
on $NODE1 slay -s KILL dev-can-ctucanfd
sleep 2
kill $!
awk '{ t = substr($1, 2, 17); if (p) { d = (t - p) * 1000; if (d > max) max = d }; p = t }
     END { printf "%d frames, largest gap %.1f ms\n", NR, max }' $LAB/kill.log
```

```text
409 frames, largest gap 20.3 ms
```

`candump -L` writes one line per frame with an absolute timestamp, and the `awk` script prints the largest time between two frames. The telemetry paused for one period: the frame due while no driver was there was skipped, and the sequence number in bytes 0 to 3 jumps by two. Depending on where in the 10 ms period the kill lands, you see a gap of about 10 or 20 ms. What happened in between:

```bash
on $NODE1 cat /tmp/sup.log
on $NODE1 cat /tmp/ecu.log
```

```text
...
1793742633.485323 ctucan-supervisor: driver pid 1073172 died 3.241 s after spawn
dev-can-ctucanfd: B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10
dev-can-ctucanfd: CTU CAN FD 2.2, /dev/can1/rx0..rx0 tx1..tx1, CAN FD, ...
1793742633.490920 ctucan-supervisor: driver pid 1163284 ready (/dev/can1/tx1 answers GET_INFO) 5.6 ms after spawn
ecu-sim: telemetry id 18F every 10000 us on /dev/can1/tx1, diagnostics and echo (1E0 -> 1E8) on /dev/can1/rx0
ecu-sim: telemetry reopened /dev/can1/tx1 after 136.3 ms
ecu-sim: diag rx reopened /dev/can1/rx0 after 137.9 ms
ecu-sim: diag rx reopened /dev/can1/rx0 after 7.3 ms
ecu-sim: telemetry reopened /dev/can1/tx1 after 0.0 ms
```

The new driver was ready 5.6 ms after the supervisor started it. The first two ecu-sim lines are from the `slay` before: ecu-sim waited about 137 ms, until the supervisor's first driver was up. After the kill, its receive thread was back 7.3 ms after its first failed read, and its telemetry thread found the new driver at its next send. The kernel freed everything the dead driver held: its memory, its interrupt attachment and its connection to the PCI server. The supervisor started a new driver, which reset the CAN core so that nothing from the dead process was left in it, and ecu-sim's threads found the devices again. Your shell, the SSH sessions and ecu-sim kept running throughout.

Check: the largest gap is about 10 or 20 ms, and `sup.log` shows `died` followed by `ready`.

---

## Add a second QNX node
Duration: 7:00

A second target, with its own CTU CAN FD board bridged to the same `vcan0`, makes a second node on the bus. Build and start it the same way as the first:

```bash
mkdir -p $NODE2 && cd $NODE2
mkqnximage --type=qemu --arch=x86_64 --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt \
    --run=-h > $NODE2.log
cd $LAB/qnx-ctucanfd
scripts/qemu-can.sh $NODE2 vcan0
until on $NODE2 true; do sleep 2; done
put $NODE2 $(find build/x86_64 -type f)
on $NODE2 /tmp/dev-can-ctucanfd
```

```text
stopping QEMU pid 532920, which runs this target
started pid 533038 with ctucan_pci on canbus0 <-> vcan0
dev-can-ctucanfd: B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10
dev-can-ctucanfd: CTU CAN FD 2.2, /dev/can1/rx0..rx0 tx1..tx1, CAN FD, ...
```

Each QEMU has its own CAN bus with its own bridge to `vcan0`. A frame that node 1 sends goes from its QEMU to `vcan0`, and the Linux kernel passes it to every other program listening on `vcan0`: node 2's QEMU, and candump. This is two QEMU virtual machines joined by your machine's virtual CAN interface, not a physical bus: there is no arbitration, no acknowledgement and no bit timing.

In terminal 2, watch the frames between the nodes (the filter `1E0:7F0` lets ids `0x1E0` to `0x1EF` through):

```bash
# terminal 2
source ~/canfd-lab/env.sh
candump -t d -x vcan0,1E0:7F0
```

On node 2, `canfd-test tester` is the far end of ecu-sim. For 10 seconds it checks every telemetry frame from node 1 (length, CRC, sequence), sends a 64-byte CAN FD echo request on `0x1E0` every 10 ms, which ecu-sim on node 1 sends back unchanged on `0x1E8`, and sends the diagnostic request once a second:

```bash
on $NODE2 /tmp/canfd-test tester /dev/can1/rx0 /dev/can1/tx1 10
```

```text
tester: telemetry 18F: 1095 frames, seq 2454..3548
tester: telemetry 18F: missing 0, out_of_order 0, bad_crc 0, bad_len 0
tester: telemetry 18F: period at this driver mean 10.000 ms, min 9.120, max 10.877
tester: echo 1E0->1E8: 1000 sent, 1000 answered, lost 0, bad 0, duplicate 0
tester: echo 1E0->1E8: round trip ms min 2.910, p50 3.769, p99 4.531, max 4.778
tester: diag 7DF->7E8: 10 sent, 10 answered, round trip ms min 3.896, max 4.152
tester: unexpected 0, other 0, tx error 0
tester: PASS
```

Terminal 2 shows the requests from node 2 and the answers from node 1, both 64-byte CAN FD frames with bit rate switch:

```text
 (000.008292)  vcan0  TX B -  1E0  [64]  E7 03 00 00 EB EC ED EE EF F0 F1 F2 F3 F4 ...
 (000.001853)  vcan0  TX B -  1E8  [64]  E7 03 00 00 EB EC ED EE EF F0 F1 F2 F3 F4 ...
```

What the tester measured:

* **Telemetry, node 1 to node 2:** every frame in the range arrived, in order, with a correct CRC. The range starts before the tester did: the first frames it read had waited in node 2's receive queue, which keeps the newest 64. The period, measured with the timestamps node 2's driver gave the frames, is the 10 ms that ecu-sim keeps on node 1.
* **Echo, node 2 to node 1 and back:** every request was answered with the same 64 bytes. The round trip is measured on node 2's clock alone, so the two virtual machines' clocks need not agree. In candump each answer follows its request after about 1.7 ms (1.4 to 3 ms in our runs): that part of the round trip is node 1 (its QEMU, driver and ecu-sim); the rest is node 2's own way out and back in.
* All of these times are QEMU timing on your machine.

Stop `candump` in terminal 2 with Ctrl+C.

Check: the tester prints `PASS`, with `missing 0` and `lost 0`.

---

## Clean up
Duration: 2:00

Stop both targets and remove everything this codelab created:

```bash
(cd $NODE2 && mkqnximage --stop)
(cd $NODE1 && mkqnximage --stop)
cd ~ && rm -rf $LAB
```

Check: this prints nothing:

```bash
for p in $(pgrep qemu-system); do echo "$p $(readlink /proc/$p/cwd)"; done |
    grep -F -e $NODE1 -e $NODE2
```

mkqnximage starts QEMU in the target's directory, so the working directory shows which QEMU belongs to this codelab. A line here is a QEMU process from this codelab that is still running (its directory shows as `(deleted)`); end it with `kill` and the process id at the start of the line.

If nothing else on your machine uses `vcan0`, remove it too: `sudo ip link delete vcan0`.

---

## What next
Duration: 1:00

You ran CAN FD software on QNX OS 8.0 without CAN hardware: a native driver on the CAN DDK, real 64-byte frames to and from Linux tools, a real-time ECU, a driver crash that the system rode out, and two QNX nodes on one bus.

* **The test suite:** `scripts/test-suite.sh` builds the driver and runs the end-to-end tests against a target; with a second target it also runs the two-node test you just did by hand. The README says how to point it at your targets.
* **Errors and bus-off:** QEMU models no bus errors, so the driver has a fault injection build (`make FI=1`) that plays error and bus-off sequences to it. `docs/DESIGN.md` explains how the driver works and what each test shows.
* **Your own application:** anything that uses libcan's `devctl()` interface, such as `CAN_DEVCTL_READ_CANMSG_EXT` and `CAN_DEVCTL_WRITE_CANMSG_EXT`, can run against this driver in QEMU first.
* **Hardware:** the driver has run only in QEMU, not on CTU CAN FD silicon or ARM.
* **QNX documentation:** [mkqnximage](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html), [pidin](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/p/pidin.html), [slay](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slay.html) and [procmgr_event_notify()](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/procmgr_event_notify.html).

The driver, its design notes and its test logs are at [github.com/NikhilGogu/qnx-ctucanfd](https://github.com/NikhilGogu/qnx-ctucanfd). Report problems with this codelab there.
