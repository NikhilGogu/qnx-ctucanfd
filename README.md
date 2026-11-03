# dev-can-ctucanfd: CAN FD for QNX 8.0, tested in QEMU

`dev-can-ctucanfd` is a native QNX 8.0 driver for the CTU CAN FD IP core, built on the QNX CAN DDK (`libcan`). QEMU emulates a PCI board carrying this core (`ctucan_pci`), so CAN and CAN FD software for QNX can be developed and tested in a virtual machine before hardware arrives, talking to Linux SocketCAN tools (`candump`, `cansend`) on the host. The driver gives QNX applications the usual libcan devices, `/dev/can1/rx0` and `/dev/can1/tx1`, with 64-byte CAN FD payloads.

Why QNX: a CAN stack in a car or a robot is a safety-relevant service. On QNX the driver is an ordinary process: when it crashes, the kernel, the applications and the shell carry on, and a supervisor starts it again. In QEMU an ECU's 1 ms telemetry stream resumed 7 to 10 ms after `slay -s KILL dev-can-ctucanfd` (host timestamps, results below).

Status (v0.1): tested in QEMU 6.2 with QNX SDP 8.0 on x86_64. It compiles and links for aarch64le but has not run on ARM, and it has not run on CTU CAN FD silicon.

## What is in the repository

| Path | What |
|---|---|
| `src/` | The driver: hardware layer for one core, libcan glue, fault injector (`make FI=1` only) |
| `tools/canfd-test.c` | Test client: send, receive, sequenced streams, statistics, timing, fault injection |
| `tools/ecu-sim.c` | A simulated battery-management ECU: 64-byte telemetry every 10 ms, one diagnostic answer |
| `tools/ctucan-supervisor.c` | Starts the driver again when it dies |
| `probe/ctucan_probe.c` | Reads the board's identification registers |
| `scripts/` | QEMU launch with the CAN board, host vcan setup, gate test, full test suite |
| `docs/DESIGN.md` | How it works and what was measured |

## The emulated hardware

QEMU's `ctucan_pci` (PCI 1760:ff00) has two CTU CAN FD 2.2 cores. BAR0 holds a bridge id whose low nibble is the core count; BAR1 holds the cores, one every 0x4000 bytes. Both cores share one interrupt line (IRQ 10 on our target), so use one core per board. QEMU 6.2 does not model bit timing, error counters, fault confinement or bus timing; see `docs/DESIGN.md`, section 1.

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
source ~/qnx800/qnxsdp-env.sh
make                    # build/x86_64/: dev-can-ctucanfd canfd-test ecu-sim ctucan-supervisor ctucan-probe
make FI=1               # build/x86_64-fi/: the same with the fault injector compiled in
make ARCH=aarch64le     # build/aarch64le/: compiles and links; not run
```

The driver links statically against `libcan.a` and needs `-lpci -lsecpol -lslog2`.

## Run in QEMU

1. Build a QNX 8.0 x86_64 QEMU image with `mkqnximage`, with ssh access as root, for example in `~/targets/tc`: `mkqnximage --type=qemu --arch=x86_64 --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt --run=-h`. Stop it again (`mkqnximage --stop`).
2. Start it with the CAN board: `scripts/qemu-can.sh ~/targets/tc vcan0`. The script reuses the exact QEMU command line mkqnximage logged and adds the three options above, a monitor socket and `-daemonize`.
3. Copy the binaries to the target (`scp build/x86_64/* root@<target>:/tmp/`) and start the driver as root:

```
# /tmp/dev-can-ctucanfd
dev-can-ctucanfd: B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10
dev-can-ctucanfd: CTU CAN FD 2.2, /dev/can1/rx0..rx0 tx1..tx1, CAN FD, 500000 bit/s (brp 4 tq 50), data 2000000 bit/s (brp 1 tq 50) BRS, 4 TXT buffer(s)
```

The driver detaches from the shell once it runs. `slay dev-can-ctucanfd` (SIGTERM) stops it cleanly.

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
/tmp/ctucan-supervisor -- /tmp/dev-can-ctucanfd &    # restarts the driver when it dies
/tmp/ecu-sim &                                       # telemetry on 0x18F every 10 ms
# host: candump -t d vcan0,18F:7FF ; cansend vcan0 7DF#02015B  ->  7E8 03 41 5B xx
slay -s KILL dev-can-ctucanfd                        # ecu-sim reopens, telemetry resumes
```

## Tests

`scripts/test-suite.sh tc vcan0` builds both variants and runs `scripts/gate-test.sh` and every test in `scripts/tests/ctutest.py` against the QEMU target, keeping the output in `build/test-results-*.txt`. The scripts reach the target through `~/bin/qtarget NAME ssh|put`; set `QTARGET=scripts/qtarget-lite.sh` to use the minimal helper in this repository, which finds the target in `~/targets/NAME` with `mkqnximage --getip`. The host side uses a raw SocketCAN socket and `candump -L`, and checks complete frames: identifier, format, length, every payload byte including padding, and sequence numbers.

Results of the run recorded in `docs/test-results.txt` (commit be3e372, QNX 8.0 x86_64 under QEMU 6.2 with KVM): the gate test and 27 of 27 tests passed.

- Every length from 0 to 64 from QNX to the host, and every classic and CAN FD length to QNX, checked byte for byte including padding; remote frames both ways; 11-bit and 29-bit ids both ways, including 29-bit ids whose low 18 bits are zero.
- Four blocked readers on one mailbox: each of 200 frames reached exactly one reader and all four finished. Four writers at once: 4 x 100 frames, each stream in order. With no reader, the 64-frame mailbox queue kept the newest 64 of 100 frames.
- 2000 frames of 64 bytes each way at the same time, 2 ms apart: both streams complete and in order.
- TX order across four TXT buffers: 2000/2000 in order with no pacing. With completions held back by the fault injector, 1000/1000 in order across 249 TX commands that readied several buffers at once; the same run with the stage-one fixed priorities broke order 996 times (negative control).
- `slay -s KILL dev-can-ctucanfd` three times under load, with the supervisor: telemetry gaps of 7.2, 9.5 and 6.8 ms in candump timestamps; the driver was ready 5.5, 7.3 and 5.3 ms after the supervisor started it.
- ecu-sim for 30 s: 3031 frames, CRC and sequence clean; period mean 10.000 ms, standard deviation 0.034 ms, 1st and 99th percentile 9.908 and 10.095 ms. These are QEMU timestamps on vcan0, not bus timing. The diagnostic request was answered 1.0 ms after it was sent.
- Fault injection: warning limit up and down, error passive, bus-off with automatic recovery (300/300 frames in order) and with manual recovery (no frame left while bus-off, the writer got EAGAIN, all 100 queued frames left in order afterwards), late TX completions, abort and count of stuck frames on a timing restart, a corrupt RX format word (19 of 20 frames delivered intact, no bad payload), and an interrupt pulse queued behind a continuation (90/90 frames, no unmask error).
- Also: short devctl payloads refused, SJW larger than a phase segment refused, SIGTERM reaches the cleanup path, a spoofed interrupt pulse is refused by the kernel and counted.

## Limitations

- QEMU 6.2 has no bit timing, error counters, fault confinement or bus timing. Bit timing is computed and written for silicon and ignored by QEMU. The error and bus-off paths are tested through the fault injector only. Host-side timestamps show QEMU behaviour, not CAN bus timing.
- One active core per board (the two cores share one interrupt line in QEMU's model).
- Software acceptance filtering only; per-frame FDF, BRS and ESI flags are not reported to clients (libcan's `CAN_MSG` has no field for them).
- aarch64le: compiles and links, not run.

## Licence

Apache-2.0, see `LICENSE`. The CAN DDK (`libcan`) and QNX SDP are QNX products under their own licence and are not part of this repository.

## How it was made

The code and documents were written by AI coding agents (Claude) directed and reviewed by the authors, with a second model reviewing the design. Every number in this README and in `docs/DESIGN.md` comes from a command run on the build box against the QEMU target; the commands are in `scripts/`.

References: QNX CAN DDK guide (qnx.com/developers/docs, `com.qnx.doc.ddk/topic/can`), CTU CAN FD IP core datasheet (canbus.pages.fel.cvut.cz/ctucanfd_ip_core), QEMU `hw/net/can/ctucan_core.c` and `docs/system/devices/can.rst`.
