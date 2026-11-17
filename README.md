# dev-can-ctucanfd: CAN FD for QNX 8.0, tested in QEMU

`dev-can-ctucanfd` is a QNX 8.0 driver for the CTU CAN FD controller that QEMU emulates as the PCI board `ctucan_pci`. It is built on the QNX CAN DDK (`libcan`) and gives QNX programs the usual libcan devices, `/dev/can1/rx0` and `/dev/can1/tx1`, with 64-byte CAN FD payloads. QEMU bridges the virtual CAN bus to SocketCAN on the Linux host, so `candump` and `cansend` there see and send the frames.

It is for developers who write CAN or CAN FD software for QNX and want to run and test it with no CAN board, on a workstation or a CI runner.

It needs a Linux host with KVM, QEMU with `ctucan_pci` (tested with 6.2), the vcan module, can-utils, QNX SDP 8.0 in `~/qnx800` and an ssh key in `~/.ssh/id_ed25519.pub`. The [Quickstart](#quickstart) below sets up `vcan0` and a QNX image in QEMU with the CAN board. After that, one command builds the driver, runs the gate test and every end-to-end test, and exits non-zero if any fails:

```
QTARGET=scripts/qtarget-lite.sh scripts/test-suite.sh tc vcan0
```

[Demo video](https://youtu.be/cFywX90Fi-M) | [Codelab: Test CAN FD software on QNX 8.0 in QEMU, no CAN board needed](https://nikhil-qnx-codelabs.pages.dev/qnx-canfd-qemu/) | [Design](docs/DESIGN.md) | [Test results](docs/test-results.txt)

Status (v0.2.0): tested in QEMU 6.2 with QNX SDP 8.0 on x86_64, also with two QNX nodes on one virtual CAN bus. In the recorded run at commit c9c7745, 41 of 41 end-to-end tests passed ([docs/test-results.txt](docs/test-results.txt)). Since that run the TX ring bookkeeping in the driver changed (a fix found by the host unit tests) and fault-injection ticks now go through the interrupt service path, both checked on the host only; `scripts/qemu-can.sh` now also stops the QEMU in the target's pid file, and the host unit tests were added. The suite has not been run again on a target since, and the driver has not been rebuilt with the SDP since that change. It compiles and links for aarch64le. It has not run on ARM or on CTU CAN FD silicon.

The driver is an ordinary QNX process, and `ctucan-supervisor` starts it again when it dies. In `restart_under_load` the driver was killed three times under load and telemetry resumed each time; frames in the dead driver's queues are lost ([docs/TESTS.md](docs/TESTS.md)).

The codelab's source is in [codelab/](codelab/qnx-canfd-qemu.md), and [codelab/DRYRUN.md](codelab/DRYRUN.md) records how it was checked.

## For judges

| Criterion | Where to look |
|---|---|
| Technical implementation | `src/dev-can-ctucanfd.c`: libcan callbacks `cb_devctl` and `cb_event`, bounded interrupt service in `service()` and `finish_pulse()`, error and bus-off handling in `error_event()` and `enter_busoff()`. `src/ctucanfd_hw.c`: the register layer. The fault-injection build (`make FI=1`, `src/ctucanfd_fi.c`). The recorded run in [docs/test-results.txt](docs/test-results.txt). Host unit tests in `tests/host/`, run by the `checks` workflow |
| Design | The Quickstart below and the codelab. `ctucan-supervisor` restarts the driver. The stock `canctl` works ([docs/DESIGN.md](docs/DESIGN.md), section 7) |
| Potential impact | CAN FD application code for QNX tested with no CAN bench: `tools/ecu-sim.c` and `canfd-test tester` are the example application and its tester, and `scripts/test-suite.sh` runs the whole suite unattended and exits non-zero on any failure |
| Innovation | Bounded interrupt batches with one `InterruptUnmask()` per pulse, and the interrupt output toggle that closed a lost-interrupt race ([docs/DESIGN.md](docs/DESIGN.md), sections 3.2 and 3.3). Error and bus-off paths tested through a fault injector that QEMU's model lacks (section 8). Recovery by restarting a plain process under a supervisor |

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

The full test suite (build, gate test and every end-to-end test) runs with `scripts/test-suite.sh tc vcan0 [tc2]`. It calls `~/bin/qtarget`; if you do not have that helper, run it as `QTARGET=scripts/qtarget-lite.sh scripts/test-suite.sh tc vcan0`, which uses the minimal helper in this repository.

## QNX technologies used

| QNX technology | In this repository | Tests that use it |
|---|---|---|
| Resource manager on the CAN DDK | `src/dev-can-ctucanfd.c` registers its callbacks with libcan (`can_resmgr_init`, `can_resmgr_init_device`, `can_resmgr_create_device`, `can_resmgr_start`). Clients use `open`, `read`, `write` and `devctl` on `/dev/can1/rx0` and `/dev/can1/tx1`, and the stock `canctl` works | `tx_lengths`, `rx_lengths`, `readers`, `writers` |
| Devctls | `cb_devctl` answers the standard `CAN_DEVCTL_*` commands, for example `GET_INFO`, `GET_STATS`, `SET_MID` and `SET_TIMING`. Driver-specific devctls are in `src/ctucanfd_devctl.h` | `devctl_sizes`, `timing`, `fi_rejected_normal` |
| Interrupts delivered as pulses | `can_resmgr_attach_intr()` ties the IRQ to one pulse code. `cb_event` serves each pulse in a bounded batch (`RX_BUDGET`, `PASS_LIMIT`, `REPORT_BUDGET`) and ends it with one `InterruptUnmask()` in `finish_pulse()` | `burst_batches`, `fi_batches`, `spoofed_irq_pulse`, `fi_reset_storm` |
| Message passing between processes | `tools/ctucan-supervisor.c` learns of deaths from `procmgr_event_notify()`, checks the server pid with `ConnectServerInfo()` and counts the driver ready once it answers `CAN_DEVCTL_GET_INFO` | `supervisor_readiness`, `supervisor_hang` |
| The driver is an ordinary process | It is killed under load while the kernel, clients and shell keep running, and the supervisor starts it again with `posix_spawn()` | `restart_under_load`, `sigterm` |
| Real-time scheduling | `tools/ecu-sim.c` sends telemetry from a `SCHED_FIFO` thread with absolute `clock_nanosleep()` deadlines | `ecu_sim`, `two_nodes` |
| PCI server and device memory | `pci_device_find`, `pci_device_attach` (exclusive owner), `pci_device_read_ba` and `pci_device_read_irq` find the board. The core's registers are mapped with `mmap_device_memory()` and `PROT_NOCACHE`, after `ThreadCtl()` grants I/O privileges | every test: the driver does not start without them |
| slog2 | After start-up the driver logs only to slog2 | `sigterm` finds the stop line there |

libcan runs every callback on one thread, so the driver state needs no locks. Between pulses that thread is back in its message loop. Which waiting message it takes first depends on the priorities of the pulse and the client, so devctl latency has no upper bound (Limitations). `docs/DESIGN.md`, section 3, has the details.

## Use in testing

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
| `tests/host/` | Unit tests of the hardware layer and the driver on a fake core and a fake libcan, built with the host compiler; CI runs them |
| `docs/DESIGN.md` | How it works and what was measured |
| `docs/TESTS.md`, `docs/test-results.txt` | What the tests cover, the measurements, and the recorded run |
| `NOTICE` | Where the register map comes from |
| `codelab/` | QNX codelab "Test CAN FD software on QNX 8.0 in QEMU, no CAN board needed" and its dry runs |

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
/tmp/canfd-test time /dev/can1/rx0                   # 64-bit and 32-bit timestamps
/tmp/canfd-test fdstats /dev/can1/rx0                # frames by format: classic, CAN FD, BRS, ESI
/tmp/canctl -u1,tx1 -i                               # the stock QNX canctl works too

# host
candump -x vcan0
cansend vcan0 456##1C0C1C2C3                         # CAN FD frame with BRS to QNX
```

Driver-specific devctls are in `src/ctucanfd_devctl.h`: extended statistics, frame counts by format, a 64-bit timestamp, manual bus-off recovery (when `-a` is not given) and fault injection.

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

The hardware layer and the driver also have unit tests that need no QNX: `make -C tests/host run` builds `src/ctucanfd_hw.c` and `src/dev-can-ctucanfd.c` with the host compiler against a fake core and a fake libcan. They check DLC tables, bit timing, TXT buffer encoding, the RX format-word checks, the TX ring, error and bus-off handling, the interrupt service bounds, reader wake-ups and the devctl checks ([docs/TESTS.md](docs/TESTS.md)). The `checks` workflow runs them on every push.

What the tests cover, with the numbers: [docs/TESTS.md](docs/TESTS.md). They check every frame length and id format both ways, byte for byte; concurrent readers and writers; bounded interrupt service; TX order; driver restart under load with the supervisor; two QNX nodes; and error, bus-off and reset paths through the fault injector.

## Limitations

- Per-frame FDF, BRS and ESI flags are not reported to clients: libcan's `CAN_MSG` has no field for them, so the frame format is a driver policy (`docs/DESIGN.md`, section 6). The driver counts frames by format both ways (`canfd-test fdstats`, `CTUCAN_DEVCTL_GET_FDSTATS`). To see the flags of single frames QNX sends, run `candump -x vcan0` on the host: `B` means bit rate switch.
- QEMU 6.2 has no bit timing, error counters, fault confinement or bus timing. Bit timing is computed and written for silicon and ignored by QEMU.
- The error and bus-off paths are tested through the fault injector only.
- Times measured on the host are QEMU timing on vcan0.
- One active core per board (the two cores share one interrupt line in QEMU's model).
- Acceptance filtering is in software only. QEMU 6.2's model has no acceptance filters (it ignores the filter registers and stores every frame), and the driver does not program the core's filters.
- Frame timestamps and `CAN_DEVCTL_GET_TIMESTAMP` are 32-bit microseconds and wrap every 2^32 us. `CTUCAN_DEVCTL_GET_TIMESTAMP64` gives the same clock as 64 bits, with the same low 32 bits (`canfd-test time`).
- Devctl latency has no upper bound: a sustained interrupt load at a priority above the clients' can delay them.
- Two nodes are two QEMU virtual machines joined by the host's vcan; no physical CAN bus was used.
- The aarch64le build compiles and links and has not run.

## Codelab

`codelab/qnx-canfd-qemu.md` is a QNX codelab in claat Markdown: host vcan, QEMU with `ctucan_pci`, building and starting the driver, first frames, ecu-sim and a diagnostic request, a kill and restart under the supervisor, and two nodes. `codelab/DRYRUN.md` records nine runs of it from the top; the last one needed no change to the text.

## Licence

Apache-2.0, see `LICENSE`. The CAN DDK (`libcan`) and QNX SDP are QNX products under their own licence and are not part of this repository. The register map: see `NOTICE`.

## References

QNX CAN DDK guide (qnx.com/developers/docs, `com.qnx.doc.ddk/topic/can`), [CTU CAN FD IP core datasheet](https://canbus.pages.fel.cvut.cz/ctucanfd_ip_core/doc/Datasheet.pdf), QEMU `hw/net/can/ctucan_core.c` and `docs/system/devices/can.rst`.
