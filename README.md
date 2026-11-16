# dev-can-ctucanfd: CAN FD for QNX 8.0, tested in QEMU

Run your CAN FD application on QNX 8.0 in QEMU, with no board.

[Demo video](https://youtu.be/cFywX90Fi-M) | [Codelab: Test CAN FD software on QNX 8.0 in QEMU, no hardware needed](https://nikhil-qnx-codelabs.pages.dev/qnx-canfd-qemu/) | [Design](docs/DESIGN.md) | [Test results](docs/test-results.txt)

`dev-can-ctucanfd` is a native QNX 8.0 driver for the CTU CAN FD IP core, built on the QNX CAN DDK (`libcan`). QEMU emulates a PCI board carrying this core (`ctucan_pci`), so CAN and CAN FD software for QNX can be developed and tested in a virtual machine, talking to Linux SocketCAN tools (`candump`, `cansend`) on the host. The driver gives QNX applications the usual libcan devices, `/dev/can1/rx0` and `/dev/can1/tx1`, with 64-byte CAN FD payloads. It is an ordinary QNX process; `ctucan-supervisor` starts it again when it dies (restart gaps: [docs/TESTS.md](docs/TESTS.md)).

Status (v0.2.0): tested in QEMU 6.2 with QNX SDP 8.0 on x86_64, also with two QNX nodes on one virtual CAN bus. 41 of 41 end-to-end tests pass ([docs/test-results.txt](docs/test-results.txt)). It compiles and links for aarch64le. It has not run on ARM or on CTU CAN FD silicon.

The codelab takes about 45 minutes (source in [codelab/](codelab/qnx-canfd-qemu.md), dry runs in [codelab/DRYRUN.md](codelab/DRYRUN.md)).

## What you see

Start the driver in QEMU, send a 64-byte CAN FD frame from QNX, and watch it on the host with `candump`. The output is from the codelab, trimmed there with `...`.

```
# QNX, in QEMU
# /tmp/dev-can-ctucanfd
dev-can-ctucanfd: B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10
dev-can-ctucanfd: CTU CAN FD 2.2, /dev/can1/rx0..rx0 tx1..tx1, CAN FD, 500000 bit/s (brp 4 tq 50), data 2000000 bit/s (brp 1 tq 50) BRS, 4 TXT buffer(s)
# /tmp/canfd-test send /dev/can1/tx1 123 64

# host
$ candump -t d -x vcan0
 (000.000000)  vcan0  TX B -  123  [64]  00 01 02 03 04 05 06 07 08 09 ... 3D 3E 3F
```

`[64]` is the length and `B` means bit rate switch, so this is a CAN FD frame. The other direction, from the codelab: `cansend vcan0 456##1C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF` on the host, and `/tmp/canfd-test recv /dev/can1/rx0 2` on QNX prints

```
/dev/can1/rx0  456  [16]  C0 C1 C2 C3 C4 C5 C6 C7 C8 C9 CA CB CC CD CE CF  ts=16199300
```

## Quickstart

Prerequisites: a Linux host (Ubuntu 22.04 or later, x86_64) with KVM, the `vcan` kernel module and QEMU with the `ctucan_pci` board (tested with QEMU 6.2), and QNX SDP 8.0 (a free non-commercial licence works). The codelab walks through every step with its output.

```
sudo apt-get install can-utils linux-modules-extra-$(uname -r)   # vcan lives in the extra modules
git clone https://github.com/NikhilGogu/qnx-ctucanfd.git && cd qnx-ctucanfd
scripts/host-vcan.sh vcan0                   # vcan0 with mtu 72, up
source ~/qnx800/qnxsdp-env.sh && make        # build/x86_64/
(mkdir -p ~/targets/tc && cd ~/targets/tc && mkqnximage --type=qemu --arch=x86_64 --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt --run=-h > ~/targets/tc.log)
scripts/qemu-can.sh ~/targets/tc vcan0       # restarts the target with the CAN board
scp build/x86_64/* root@<target>:/tmp/
# on the target: /tmp/dev-can-ctucanfd, then /tmp/canfd-test send /dev/can1/tx1 123 64
# on the host:   candump -x vcan0
```

The full test suite (build, gate test and 41 tests) runs with `scripts/test-suite.sh tc vcan0 [tc2]`. It calls `~/bin/qtarget`; if you do not have that helper, run it as `QTARGET=scripts/qtarget-lite.sh scripts/test-suite.sh tc vcan0`, which uses the minimal helper in this repository.

## How it uses QNX

- **Resource manager on the CAN DDK.** `src/dev-can-ctucanfd.c` registers its callbacks with libcan (`can_resmgr_init`, `can_resmgr_init_device`, `can_resmgr_create_device`, `can_resmgr_start`), which runs the QNX resource manager loop. Clients use plain `open`, `read`, `write` and `devctl` on `/dev/can1/rx0` and `/dev/can1/tx1`, and the stock `canctl` works.
- **Standard CAN devctls.** `cb_devctl` answers the `CAN_DEVCTL_*` commands, for example `GET_INFO`, `GET_STATS`, `SET_MID`, `SET_TIMING`, `WRITE_CANMSG_EXT` and `READ_CANMSG_EXT`. Driver-specific devctls are in `src/ctucanfd_devctl.h`.
- **Interrupts as pulses, in bounded batches.** `can_resmgr_attach_intr()` ties the IRQ to one pulse code, and libcan calls the driver's `event_handler` on the resource manager thread, so the driver state needs no locks. Each pulse moves at most 64 frames from the RX FIFO, makes at most 8 passes over the interrupt status and returns at most 64 mailboxes to libcan (`RX_BUDGET`, `PASS_LIMIT`, `REPORT_BUDGET`), then calls `InterruptUnmask()` once. Work left over keeps an interrupt bit pending, so the next pulse continues it. See `docs/DESIGN.md`, section 3.
- **PCI attach.** `pci_device_find`, `pci_device_attach` (exclusive owner), `pci_device_read_ba` and `pci_device_read_irq` find the board; the core's registers are mapped with `mmap_device_memory` and `PROT_NOCACHE`.
- **Around the driver.** `tools/ecu-sim.c` sends its telemetry from a `SCHED_FIFO` thread with absolute `clock_nanosleep` deadlines. `tools/ctucan-supervisor.c` starts the driver with `posix_spawn` and counts it ready only when the process it started answers a `CAN_DEVCTL_GET_INFO` message. The driver logs to slog2.

## Why it matters

An application that uses libcan's devices and devctls can be run and checked against this driver on a machine with QNX SDP and QEMU, with no CAN board, and the same host can watch and inject frames with `candump` and `cansend`. The repository's demo is a simulated battery-management ECU that sends 64-byte telemetry and answers an OBD-II request.

The test suite is scripted and exits non-zero if the build, the gate test or any test fails, so it can run unattended on such a machine.

This is a test fixture path, QEMU only. Production CAN controllers such as FlexCAN and MCAN are out of scope. The driver has not run on ARM or on CTU CAN FD silicon, and QEMU models no bit timing or bus errors, so timing results are QEMU timing.

## What is in the repository

| Path | What |
|---|---|
| `src/` | The driver: hardware layer for one core, libcan glue, fault injector (`make FI=1` only) |
| `tools/canfd-test.c` | Test client: send, receive, sequenced streams, statistics, timing, fault injection, and `tester`, the far end of ecu-sim on a second node |
| `tools/ecu-sim.c` | A simulated battery-management ECU: 64-byte telemetry every 10 ms, one diagnostic answer, an echo service for round-trip tests |
| `tools/ctucan-supervisor.c` | Starts the driver again when it dies |
| `probe/ctucan_probe.c` | Reads the board's identification registers |
| `scripts/` | QEMU launch with the CAN board, host vcan setup, gate test, full test suite |
| `tests/host/` | Unit tests of the hardware layer on a fake core, built with the host compiler; CI runs them |
| `docs/DESIGN.md` | How it works and what was measured |
| `docs/TESTS.md`, `docs/test-results.txt` | What the 41 tests cover, the measurements, and the recorded run |
| `NOTICE` | Where the register map comes from |
| `codelab/` | QNX codelab "Test CAN FD software on QNX 8.0 in QEMU, no hardware needed" and its dry runs |

## The emulated hardware

QEMU's `ctucan_pci` (PCI 1760:ff00) has two CTU CAN FD 2.2 cores. BAR0 holds a bridge id whose low nibble is the core count; BAR1 holds the cores, one every 0x4000 bytes. Both cores share one interrupt line (IRQ 10 on our target), so use one core per board. QEMU 6.2 does not model bit timing, error counters, fault confinement, bus timing or the core's acceptance filters (`ctucan-probe` prints `FILTER_STATUS=0x0000 filters present: none`); see `docs/DESIGN.md`, sections 1 and 4.1.

The QEMU options that add the board and bridge it to a host interface:

```
-object can-bus,id=canbus0-bus
-object can-host-socketcan,id=canhost0,if=vcan0,canbus=canbus0-bus
-device ctucan_pci,canbus0=canbus0-bus,canbus1=canbus0-bus
```

The board refuses to start unless both `canbusN` links are set, and the bus id must not be the bare string `canbus0` (QEMU then reports the path as ambiguous). QEMU's host bridge passes CAN FD frames only when the host interface has the CAN FD MTU of 72.

## Host setup (Ubuntu)

```
sudo apt-get install can-utils linux-modules-extra-$(uname -r)   # vcan lives in the extra modules
scripts/host-vcan.sh vcan0          # loads vcan, creates vcan0 with mtu 72, brings it up
```

## Build

With QNX SDP 8.0 installed:

```
git clone https://github.com/NikhilGogu/qnx-ctucanfd.git && cd qnx-ctucanfd
source ~/qnx800/qnxsdp-env.sh
make                    # build/x86_64/: dev-can-ctucanfd canfd-test ecu-sim ctucan-supervisor ctucan-probe
make FI=1               # build/x86_64-fi/: the same with the fault injector compiled in
make ARCH=aarch64le     # build/aarch64le/: compiles and links; not run
```

The driver links statically against `libcan.a` and needs `-lpci -lsecpol -lslog2`.

## Run in QEMU

1. Build a QNX 8.0 x86_64 QEMU image with `mkqnximage`, with ssh access as root, and keep its output next to the target directory, for example `mkdir -p ~/targets/tc && cd ~/targets/tc && mkqnximage --type=qemu --arch=x86_64 --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt --run=-h > ~/targets/tc.log`.
2. Start it with the CAN board: `scripts/qemu-can.sh ~/targets/tc vcan0`. The script stops any QEMU of that target that still runs (the one `mkqnximage --run` started may still be starting, so `mkqnximage --stop` can miss it), reuses the exact QEMU command line mkqnximage logged in `~/targets/tc.log` and adds the three options above, a monitor socket and `-daemonize`.
3. Copy the binaries to the target (`scp build/x86_64/* root@<target>:/tmp/`) and start the driver as root:

```
# /tmp/dev-can-ctucanfd
dev-can-ctucanfd: B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10
dev-can-ctucanfd: CTU CAN FD 2.2, /dev/can1/rx0..rx0 tx1..tx1, CAN FD, 500000 bit/s (brp 4 tq 50), data 2000000 bit/s (brp 1 tq 50) BRS, 4 TXT buffer(s)
```

The driver detaches from the shell once it runs and lets go of the terminal, so `ssh root@<target> /tmp/dev-can-ctucanfd` returns; from then on it logs to slog2. `slay dev-can-ctucanfd` (SIGTERM) stops it cleanly.

Options (`dev-can-ctucanfd -h`):

| Option | Meaning (default) |
|---|---|
| `-i idx`, `-c core`, `-u unit` | PCI board index (0), core on the board (0), device unit (core + 1) |
| `-r n`, `-t n` | RX and TX mailboxes (1 each) |
| `-q n`, `-w n` | queue length per mailbox (64), blocked clients per mailbox (16) |
| `-T n` | TXT buffers in use, 1 to 4 (4) |
| `-x` | mailbox ids are 29-bit (11-bit) |
| `-b rate`, `-d rate`, `-k hz` | nominal bit rate (500000), data bit rate (2000000), core clock (100 MHz) |
| `-C`, `-A`, `-S` | classic CAN only; every frame as CAN FD; no bit rate switch |
| `-l`, `-L`, `-a` | internal loopback, listen only, automatic bus-off recovery |

## Use

Frames over 8 bytes go out as CAN FD with bit rate switch, lengths rounded up to the next CAN FD size and zero padded. `CAN_DEVCTL_WRITE_CANMSG_EXT` and `CAN_DEVCTL_READ_CANMSG_EXT` carry the full 64 bytes; a plain `write()` on a TX device sends a data frame with the mailbox id (`CAN_DEVCTL_SET_MID`). Remote frames go out as classic RTR frames; a remote frame that would need CAN FD is refused.

```
# QNX
/tmp/canfd-test send /dev/can1/tx1 123 64            # 64-byte CAN FD frame, id 0x123
/tmp/canfd-test send /dev/can1/tx1 1ABCDEF0x 8       # 29-bit id (suffix x); suffix r = remote frame
/tmp/canfd-test recv /dev/can1/rx0 2
/tmp/canfd-test info /dev/can1/rx0                   # libcan statistics and error registers
/tmp/canfd-test xstats /dev/can1/rx0                 # driver statistics
/tmp/canctl -u1,tx1 -i                               # the stock QNX canctl works too

# host
candump -x vcan0
cansend vcan0 456##1C0C1C2C3                         # CAN FD frame with BRS to QNX
```

Driver-specific devctls are in `src/ctucanfd_devctl.h`: extended statistics, manual bus-off recovery (when `-a` is not given) and fault injection.

Supervision and the ECU demo:

```
/tmp/ctucan-supervisor -- /tmp/dev-can-ctucanfd &    # restarts the driver when it dies (-n limit, -r ready ms)
/tmp/ecu-sim &                                       # telemetry on 0x18F every 10 ms
# host: candump -t d vcan0,18F:7FF ; cansend vcan0 7DF#02015B  ->  7E8 03 41 5B xx
slay -s KILL dev-can-ctucanfd                        # ecu-sim reopens, telemetry resumes
```

## Two QNX nodes

Two QEMU targets, each with its own `ctucan_pci` board, both started with `scripts/qemu-can.sh` on the same host `vcan0`, make two nodes: each QEMU bridges its own CAN bus to `vcan0`, and the host kernel passes every frame to the other QEMU and to `candump`. There is no arbitration, acknowledgement or bit timing between them, and all times are QEMU timing.

```
# node 1: the ECU          /tmp/ecu-sim &
# node 2: the tester       /tmp/canfd-test tester /dev/can1/rx0 /dev/can1/tx1 30
# host:   the observer     candump -t d -x vcan0
```

The tester's measurements (telemetry, echo round trips, diagnostic requests) are in [docs/TESTS.md](docs/TESTS.md).

## Tests

`scripts/test-suite.sh tc vcan0 [tc2]` builds both variants and runs `scripts/gate-test.sh` and every test in `scripts/tests/ctutest.py` against the QEMU target, keeping the output in `build/test-results-*.txt`. The host side uses a raw SocketCAN socket and `candump -L`, and checks complete frames: identifier, format, length, every payload byte including padding, and sequence numbers. The recorded run (commit c9c7745, from a clean clone, two QNX 8.0 x86_64 targets under QEMU 6.2 with KVM): the gate test and 41 of 41 tests passed, two-node test included. [docs/test-results.txt](docs/test-results.txt) has every result line.

The hardware layer also has unit tests that need no QNX: `make -C tests/host run` builds `src/ctucanfd_hw.c` with the host compiler against a fake core and checks DLC tables, bit timing, TXT buffer encoding and the RX format-word checks. The `checks` workflow runs them on every push.

What the tests cover, with the numbers: [docs/TESTS.md](docs/TESTS.md). In short: every frame length and id format both ways, byte for byte; concurrent readers and writers; bounded interrupt service; TX order; driver restart under load with the supervisor; two QNX nodes; and error, bus-off and reset paths through the fault injector.

## Limitations

- Per-frame FDF, BRS and ESI flags are not reported to clients: libcan's `CAN_MSG` has no field for them, so the frame format is a driver policy (`docs/DESIGN.md`, section 6). To see the flags of frames QNX sends, run `candump -x vcan0` on the host: `B` means bit rate switch.
- QEMU 6.2 has no bit timing, error counters, fault confinement or bus timing. Bit timing is computed and written for silicon and ignored by QEMU.
- The error and bus-off paths are tested through the fault injector only.
- Times measured on the host are QEMU timing on vcan0.
- One active core per board (the two cores share one interrupt line in QEMU's model).
- Acceptance filtering is in software only. QEMU 6.2's model has no acceptance filters (it ignores the filter registers and stores every frame), and the driver does not program the core's filters.
- Devctl latency has no upper bound: a sustained interrupt load at a priority above the clients' can delay them.
- Two nodes are two QEMU virtual machines joined by the host's vcan; no physical CAN bus was used.
- The aarch64le build compiles and links and has not run.

## Codelab

`codelab/qnx-canfd-qemu.md` is a QNX codelab in claat Markdown, about 45 minutes: host vcan, QEMU with `ctucan_pci`, building and starting the driver, first frames, ecu-sim and a diagnostic request, a kill and restart under the supervisor, and two nodes. `codelab/DRYRUN.md` records nine runs of it from the top on the build box; the last one needed no change.

## Licence

Apache-2.0, see `LICENSE`. The CAN DDK (`libcan`) and QNX SDP are QNX products under their own licence and are not part of this repository. The register map: see `NOTICE`.

## References

QNX CAN DDK guide (qnx.com/developers/docs, `com.qnx.doc.ddk/topic/can`), [CTU CAN FD IP core datasheet](https://canbus.pages.fel.cvut.cz/ctucanfd_ip_core/doc/Datasheet.pdf), QEMU `hw/net/can/ctucan_core.c` and `docs/system/devices/can.rst`.
