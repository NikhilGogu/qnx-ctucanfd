# Tests and measurements

What the end-to-end suite checks and what was measured, moved here from the README. Every result line is in [test-results.txt](test-results.txt); how the driver works is in [DESIGN.md](DESIGN.md). Times measured on the host are QEMU timing on `vcan0`.

## Restart gaps

`ctucan-supervisor` starts the driver again when it dies. Telemetry gaps measured on the host's `vcan0` after `slay -s KILL dev-can-ctucanfd` in QEMU:

- ecu-sim telemetry every 1 ms, the host sending 500 frames/s into QNX, three kills (test `restart_under_load`): 9.0, 6.5 and 8.4 ms.
- ecu-sim telemetry every 10 ms, no other traffic, one kill (codelab step 9): 20.3 ms in the run the codelab shows and 19.8 ms in its last dry run; the one frame due while no driver ran was skipped.

## Two QNX nodes, measured

The setup is in the README. This is what the tester measured.

The tester checks every telemetry frame from node 1 (length, CRC-8, sequence), sends a 64-byte CAN FD echo request on 0x1E0 every 10 ms, which ecu-sim sends back unchanged on 0x1E8, and times each round trip on its own clock; once a second it sends the diagnostic request. Measured (`two_nodes`, 30 s, `docs/test-results.txt`): 3094 of 3094 telemetry frames on node 2 with sequence and CRC clean, also on `vcan0`; 3000 of 3000 echo round trips with every byte checked on node 2 and on `vcan0`, round trip on node 2's clock min 2.751 ms, median 3.271, 99th percentile 3.681, max 4.162; of that, node 1 between request and reply on `vcan0` median 1.700 ms, 99th percentile 1.998; 30 of 30 diagnostic requests answered.

## Host unit tests

`tests/host/` builds the hardware layer, `src/ctucanfd_hw.c`, and the driver, `src/dev-can-ctucanfd.c`, with the host's C compiler and runs them against a fake core (`tests/host/fake_core.c`), with AddressSanitizer and UndefinedBehaviorSanitizer. The driver also runs against a fake libcan (`tests/host/fake_libcan.c`). They need no QNX SDP and no QEMU, and the `checks` workflow runs them with gcc and clang on every push and pull request:

```
make -C tests/host run          # SAN= to build without the sanitizers
```

| Test | What it checks |
|---|---|
| `test_dlc_timing` | DLC to length and back for every CAN FD size; bit timing at the start-up defaults, exact bit rate, valid fields and sample point within half a time quantum over a grid of clocks, rates and sample points; refused timings; BTR field packing |
| `test_frames` | The TX frame format policy (`-C`, `-A`, `-S`) for every length; TXT buffer contents for classic, CAN FD (DLC rounding, zero padding, BRS) and remote frames; refused frames write nothing; TX_COMMAND and the rotating priorities; RX pops with FDF, BRS and ESI; QEMU's and the datasheet's word counts for remote frames; every bad format word refused after reading only that word; every length and id format written to a TXT buffer and read back from the FIFO unchanged |
| `test_time` | 32-bit timestamps across the 2^32 us wrap, offsets set before and after a wrap, the 64-bit value counting on with the same low 32 bits |
| `test_core` | Identification, soft reset and its timeout, the register writes of start and stop and their order (interrupt output off first and on last, timing before the core joins the bus) |
| `test_driver` | The driver's own `main()` up to `can_resmgr_start()` and its shutdown; the TX ring (FIFO retirement, one TX command per refill, order breaks counted, failed, aborted and lost buffers, the ring going round from its last buffer with 1 to 4 buffers); bus-off (TX held through none and error passive, released at error active, in-flight frames aborted, ERCRST with `-a` and on RECOVER); warning limit, error passive and bus error types; interrupt service (spurious pulses, 64 frames per pulse, 8 passes per pulse, output off at every `InterruptUnmask()`, a stopped core left quiet, refused unmasks counted); reader wake-ups (one return per waiting mailbox, RBNEI held while returns are left, a mailbox that wakes nobody dropped after 8 pulses); every devctl size check and refusal |
| `test_supervisor` | `tools/ctucan-supervisor.c` with its spawn and sleep replaced: a SIGTERM during the restart delay starts no driver |

The fake core models only what the code relies on, as `src/ctucanfd_regs.h` and `docs/DESIGN.md` describe it. It is not a model of the silicon or of QEMU. The fake libcan follows the DDK text quoted in DESIGN.md section 3.1: the event handler is called until it returns NULL, and each returned RX mailbox wakes one blocked reader. It says nothing about how libcan schedules pulses against clients. The headers in `tests/host/shim/` define the QNX names the driver uses so it builds without the SDP; they are our own stand-ins, written from how this repository uses the names, not QNX's headers. Real interrupts, the PCI server and libcan itself are covered only by the end-to-end suite below.

## The suite and its results

`scripts/test-suite.sh tc vcan0 [tc2]` builds both variants and runs `scripts/gate-test.sh` and every test in `scripts/tests/ctutest.py` against the QEMU target, keeping the output in `build/test-results-*.txt`. With a second target on the same interface (`tc2`) it also runs the two-node test; without one that test is skipped. The scripts reach the target through `~/bin/qtarget NAME ssh|put`; set `QTARGET=scripts/qtarget-lite.sh` to use the minimal helper in this repository, which finds the target in `~/targets/NAME` with `mkqnximage --getip`. The host side uses a raw SocketCAN socket and `candump -L`, and checks complete frames: identifier, format, length, every payload byte including padding, and sequence numbers.

Results of the run recorded in `docs/test-results.txt` (commit c9c7745, from a clean clone with `QTARGET=scripts/qtarget-lite.sh`, two QNX 8.0 x86_64 targets under QEMU 6.2 with KVM): the gate test and 41 of 41 tests passed, two-node test included, and every binary copied to the target was checked by md5. Since that commit the driver's TX ring bookkeeping changed (DESIGN.md section 12), checked only by the host unit tests, and the suite has not been run again on a target.

- Every length from 0 to 64 from QNX to the host, and every classic and CAN FD length to QNX, checked byte for byte including padding; remote frames both ways; 11-bit and 29-bit ids both ways, including 29-bit ids whose low 18 bits are zero.
- Four blocked readers on one mailbox: each of 200 frames reached exactly one reader and all four finished. Four writers at once: 4 x 100 frames, each stream in order. With no reader, the 64-frame mailbox queue kept the newest 64 of 100 frames.
- 2000 frames of 64 bytes each way at the same time, 2 ms apart: both streams complete and in order.
- Bounded interrupt service: 300 frames in bursts of 90 arrived in order in 7 interrupt pulses of at most 64 frames, and a client's devctls were answered 757 times while frames were still coming; at that load the longest took 32533 us, and 2636 us with 4-frame batches. A pulse that arrives while the core is stopped after a failed reset is unmasked with the core's interrupt output left off (1 pulse, then none in the next second), and a mailbox whose wake-ups fail is retried for 8 pulses and then dropped and counted.
- TX order across four TXT buffers: 2000/2000 in order with no pacing. With completions held back by the fault injector, 1000/1000 in order across 249 TX commands that readied several buffers at once; the same run with the first version's fixed priorities broke order 996 times (negative control).
- Restart under load, 1 ms workload (`restart_under_load`): ecu-sim telemetry on 0x2F8 every 1 ms, the host sending 500 frames/s of 64 bytes into QNX, three `slay -s KILL dev-can-ctucanfd` 2 s apart with the supervisor: telemetry gaps of 9.0, 6.5 and 8.4 ms in candump timestamps (8, 7 and 8 periods lost); the driver was ready 6.2, 7.8 and 6.4 ms after the supervisor started it.
- Supervisor: it kills and restarts a driver that does not become ready, takes a driver as ready only when the process it started serves the path, abandons a readiness probe that a server never answers, and counts spawn failures.
- ecu-sim for 30 s at its 10 ms period: 3032 frames, CRC and sequence clean; period in QEMU timestamps on vcan0 mean 10.000 ms, standard deviation 0.049 ms, 1st and 99th percentile 9.870 and 10.131 ms. The diagnostic request was answered 1.1 ms after it was sent.
- Two QNX nodes: see the section above.
- Fault injection: warning limit up and down, error passive, bus-off with automatic recovery (300/300 frames in order), bus-off whose FAULT_STATE reads none and then passive before active (TX stayed held until active), bus-off with manual recovery (no frame left while bus-off, the writer got EAGAIN, all 100 queued frames left in order afterwards), aborts that do not finish in time, also with no traffic and no ticker (dropped and counted at bus-off, TX resumed), late TX completions with one RX frame per pulse (none lost), a restart whose reset times out followed by one that works, a corrupt RX format word among payloads that look like format words (no frame fabricated), and state cleared by scenario OFF and by a reset. The fault injector changes only what the driver sees; QEMU sends each frame when it is set ready (docs/DESIGN.md, section 8).
- Also: short devctl payloads refused, SJW larger than a phase segment refused, SIGTERM reaches the cleanup path, a spoofed interrupt pulse is refused by the kernel and counted, and the normal build refuses the fault-injection devctls.
