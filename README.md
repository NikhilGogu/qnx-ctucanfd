# dev-can-ctucanfd: CAN FD for QNX 8.0, tested in QEMU

`dev-can-ctucanfd` is a native QNX 8.0 driver for the CTU CAN FD IP core, built on the QNX CAN DDK (`libcan`). QEMU emulates a PCI board carrying this core (`ctucan_pci`), so CAN and CAN FD software for QNX can be developed and tested in a virtual machine before hardware arrives, talking to Linux SocketCAN tools (`candump`, `cansend`) on the host. The driver gives QNX applications the usual libcan devices, `/dev/can1/rx0` and `/dev/can1/tx1`, with 64-byte CAN FD payloads.

Why QNX: a CAN stack in a car or a robot is a safety-relevant service. On QNX the driver is an ordinary process: when it crashes, the kernel, the applications and the shell carry on, and a supervisor starts it again. In QEMU, after three `slay -s KILL dev-can-ctucanfd`, an ECU's 1 ms telemetry stream resumed after gaps of 9.0, 6.5 and 8.4 ms (host timestamps, results below).

Status (v0.2.0): tested in QEMU 6.2 with QNX SDP 8.0 on x86_64, also with two QNX nodes on one virtual CAN bus. It compiles and links for aarch64le but has not run on ARM, and it has not run on CTU CAN FD silicon.

Tutorial: the codelab [Test CAN FD software on QNX 8.0 in QEMU, no hardware needed](https://nikhil-qnx-codelabs.pages.dev/qnx-canfd-qemu/) walks through everything below in about 45 minutes (source in [codelab/](codelab/qnx-canfd-qemu.md), dry runs in [codelab/DRYRUN.md](codelab/DRYRUN.md)).

## What is in the repository

| Path | What |
|---|---|
| `src/` | The driver: hardware layer for one core, libcan glue, fault injector (`make FI=1` only) |
| `tools/canfd-test.c` | Test client: send, receive, sequenced streams, statistics, timing, fault injection, and `tester`, the far end of ecu-sim on a second node |
| `tools/ecu-sim.c` | A simulated battery-management ECU: 64-byte telemetry every 10 ms, one diagnostic answer, an echo service for round-trip tests |
| `tools/ctucan-supervisor.c` | Starts the driver again when it dies |
| `probe/ctucan_probe.c` | Reads the board's identification registers |
| `scripts/` | QEMU launch with the CAN board, host vcan setup, gate test, full test suite |
| `docs/DESIGN.md` | How it works and what was measured |
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

Two QEMU targets, each with its own `ctucan_pci` board, both started with `scripts/qemu-can.sh` on the same host `vcan0`, make two nodes: each QEMU bridges its own CAN bus to `vcan0`, and the host kernel passes every frame to the other QEMU and to `candump`. This is two QEMU virtual machines joined by the host's virtual CAN interface, not a physical bus: there is no arbitration, acknowledgement or bit timing, and all times are QEMU timing.

```
# node 1: the ECU          /tmp/ecu-sim &
# node 2: the tester       /tmp/canfd-test tester /dev/can1/rx0 /dev/can1/tx1 30
# host:   the observer     candump -t d -x vcan0
```

The tester checks every telemetry frame from node 1 (length, CRC-8, sequence), sends a 64-byte CAN FD echo request on 0x1E0 every 10 ms, which ecu-sim sends back unchanged on 0x1E8, and times each round trip on its own clock; once a second it sends the diagnostic request. Measured (`two_nodes`, 30 s, `docs/test-results.txt`): 3094 of 3094 telemetry frames on node 2 with sequence and CRC clean, also on `vcan0`; 3000 of 3000 echo round trips with every byte checked on node 2 and on `vcan0`, round trip on node 2's clock min 2.751 ms, median 3.271, 99th percentile 3.681, max 4.162; of that, node 1 between request and reply on `vcan0` median 1.700 ms, 99th percentile 1.998; 30 of 30 diagnostic requests answered.

## Tests

`scripts/test-suite.sh tc vcan0 [tc2]` builds both variants and runs `scripts/gate-test.sh` and every test in `scripts/tests/ctutest.py` against the QEMU target, keeping the output in `build/test-results-*.txt`. With a second target on the same interface (`tc2`) it also runs the two-node test; without one that test is skipped. The scripts reach the target through `~/bin/qtarget NAME ssh|put`; set `QTARGET=scripts/qtarget-lite.sh` to use the minimal helper in this repository, which finds the target in `~/targets/NAME` with `mkqnximage --getip`. The host side uses a raw SocketCAN socket and `candump -L`, and checks complete frames: identifier, format, length, every payload byte including padding, and sequence numbers.

Results of the run recorded in `docs/test-results.txt` (commit c9c7745, from a clean clone with `QTARGET=scripts/qtarget-lite.sh`, two QNX 8.0 x86_64 targets under QEMU 6.2 with KVM): the gate test and 41 of 41 tests passed, two-node test included, and every binary copied to the target was checked by md5.

- Every length from 0 to 64 from QNX to the host, and every classic and CAN FD length to QNX, checked byte for byte including padding; remote frames both ways; 11-bit and 29-bit ids both ways, including 29-bit ids whose low 18 bits are zero.
- Four blocked readers on one mailbox: each of 200 frames reached exactly one reader and all four finished. Four writers at once: 4 x 100 frames, each stream in order. With no reader, the 64-frame mailbox queue kept the newest 64 of 100 frames.
- 2000 frames of 64 bytes each way at the same time, 2 ms apart: both streams complete and in order.
- Bounded interrupt service: 300 frames in bursts of 90 arrived in order in 7 interrupt pulses of at most 64 frames, and a client's devctls were answered 757 times while frames were still coming; at that load the longest took 32533 us, and 2636 us with 4-frame batches. These are measurements at the tested loads, not a bound. A pulse that arrives while the core is stopped after a failed reset does not re-arm it (no interrupt storm), and a mailbox whose wake-ups fail is retried for 8 pulses and then dropped and counted.
- TX order across four TXT buffers: 2000/2000 in order with no pacing. With completions held back by the fault injector, 1000/1000 in order across 249 TX commands that readied several buffers at once; the same run with the stage-one fixed priorities broke order 996 times (negative control).
- `slay -s KILL dev-can-ctucanfd` three times under load, with the supervisor: telemetry gaps of 9.0, 6.5 and 8.4 ms in candump timestamps; the driver was ready 6.2, 7.8 and 6.4 ms after the supervisor started it. The supervisor kills and restarts a driver that does not become ready, does not mistake another process serving the path for readiness, abandons a readiness probe that a server never answers, and counts spawn failures.
- ecu-sim for 30 s: 3032 frames, CRC and sequence clean; period mean 10.000 ms, standard deviation 0.049 ms, 1st and 99th percentile 9.870 and 10.131 ms. These are QEMU timestamps on vcan0, not bus timing. The diagnostic request was answered 1.1 ms after it was sent.
- Two QNX nodes: see the section above.
- Fault injection: warning limit up and down, error passive, bus-off with automatic recovery (300/300 frames in order), bus-off whose FAULT_STATE reads none and then passive before active (TX stayed held until active), bus-off with manual recovery (no frame left while bus-off, the writer got EAGAIN, all 100 queued frames left in order afterwards), aborts that do not finish in time, also with no traffic and no ticker (dropped and counted at bus-off, TX resumed), late TX completions with one RX frame per pulse (none lost), a restart whose reset times out followed by one that works, a corrupt RX format word among payloads that look like format words (no frame fabricated), and state cleared by scenario OFF and by a reset. The fault injector changes only what the driver sees; QEMU sends each frame when it is set ready (docs/DESIGN.md, section 8).
- Also: short devctl payloads refused, SJW larger than a phase segment refused, SIGTERM reaches the cleanup path, a spoofed interrupt pulse is refused by the kernel and counted, and the normal build refuses the fault-injection devctls.

## Limitations

- QEMU 6.2 has no bit timing, error counters, fault confinement or bus timing. Bit timing is computed and written for silicon and ignored by QEMU. The error and bus-off paths are tested through the fault injector only. Host-side timestamps show QEMU behaviour, not CAN bus timing.
- One active core per board (the two cores share one interrupt line in QEMU's model).
- Software acceptance filtering only: QEMU 6.2's model has no acceptance filters (it ignores the filter registers and stores every frame), so the core's filters are not programmed and nothing could test them. Per-frame FDF, BRS and ESI flags are not reported to clients (libcan's `CAN_MSG` has no field for them).
- Two nodes means two QEMU virtual machines joined by the host's vcan, not a physical bus.
- aarch64le: compiles and links, not run.

## Codelab

`codelab/qnx-canfd-qemu.md` is a QNX codelab in claat Markdown, about 45 minutes: host vcan, QEMU with `ctucan_pci`, building and starting the driver, first frames, ecu-sim and a diagnostic request, a kill and restart under the supervisor, and two nodes. `codelab/DRYRUN.md` records nine runs of it from the top on the build box; the last one needed no change.

## Licence

Apache-2.0, see `LICENSE`. The CAN DDK (`libcan`) and QNX SDP are QNX products under their own licence and are not part of this repository.

## How this was built

dev-can-ctucanfd was written by AI coding agents (Claude Opus 5.5 in Claude Code) working for our team on a build box with QNX SDP 8.0 and QEMU, and reviewed by a second model (GPT-6 Astra in Codex CLI) before each release; its findings and what we did about them are in docs/DESIGN.md. QNX library behaviour comes only from the QNX documentation and black-box tests of our own code. Every number in this README comes from a command that ran against QNX OS 8.0 in QEMU; the full output is in docs/test-results.txt.

References: QNX CAN DDK guide (qnx.com/developers/docs, `com.qnx.doc.ddk/topic/can`), CTU CAN FD IP core datasheet (canbus.pages.fel.cvut.cz/ctucanfd_ip_core), QEMU `hw/net/can/ctucan_core.c` and `docs/system/devices/can.rst`.
