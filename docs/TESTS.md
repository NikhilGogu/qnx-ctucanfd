# Tests and results

What the end-to-end suite checks and what it found, moved here from the README. Result lines are in [test-results.txt](test-results.txt); how the driver works is in [DESIGN.md](DESIGN.md). Target timings omitted. Run the example locally for full output. Anything timed on this setup is QEMU timing on `vcan0`, not bus timing.

## Restart under the supervisor

`ctucan-supervisor` starts the driver again when it dies. After `slay -s KILL dev-can-ctucanfd` in QEMU the host's `candump` on `vcan0` shows telemetry resuming:

- ecu-sim telemetry every 1 ms, the host sending 500 frames/s into QNX, three kills (test `restart_under_load`): telemetry resumed after each kill.
- ecu-sim telemetry every 10 ms, no other traffic, one kill (codelab step 9): telemetry resumed; the one frame due while no driver ran was skipped, so the sequence number jumps by two.

## Two QNX nodes

The setup is in the README. This is what the tester counted.

The tester checks every telemetry frame from node 1 (length, CRC-8, sequence), sends a 64-byte CAN FD echo request on 0x1E0 every 10 ms, which ecu-sim sends back unchanged on 0x1E8, and times each round trip on its own clock; once a second it sends the diagnostic request. Result (`two_nodes`, 30 s, `docs/test-results.txt`): 3094 of 3094 telemetry frames on node 2 with sequence and CRC clean, also on `vcan0`; 3000 of 3000 echo round trips with every byte checked on node 2 and on `vcan0`; 30 of 30 diagnostic requests answered. The round-trip times are omitted. Run the example locally for full output.

## Host unit tests

`tests/host/` builds the hardware layer, `src/ctucanfd_hw.c`, and the driver, `src/dev-can-ctucanfd.c`, with the host's C compiler and runs them against a fake core (`tests/host/fake_core.c`), with AddressSanitizer and UndefinedBehaviorSanitizer. The driver also runs against a fake libcan (`tests/host/fake_libcan.c`). They need no QNX SDP and no QEMU, and the `checks` workflow runs them with gcc and clang on every push and pull request:

```
make -C tests/host run          # SAN= to build without the sanitizers
```

| Test | What it checks |
|---|---|
| `test_dlc_timing` | DLC to length and back for every CAN FD size; bit timing at the start-up defaults, exact bit rate, valid fields and sample point within half a time quantum over a grid of clocks, rates and sample points; refused timings; BTR field packing |
| `test_frames` | The TX frame format policy (`-C`, `-A`, `-S`) for every length; TXT buffer contents for classic, CAN FD (DLC rounding, zero padding, BRS) and remote frames; refused frames write nothing; TX_COMMAND and the rotating priorities; RX pops with FDF, BRS and ESI; QEMU's and the datasheet's word counts for remote frames; every bad format word refused after reading only that word; every length and id format written to a TXT buffer and read back from the FIFO unchanged |
| `test_mode_rmw` | MODE and SETTINGS written as one word: on a fake core that starts at the datasheet reset value, start and stop keep MODE.RXBAM and SETTINGS.TBFBO (reset 1, datasheet 3.1.3 and 3.1.4) and the bits that depend on the core build, and the driver's own bits follow the configuration whatever the core held. QEMU 6.2 ignores both reset-1 bits, so only this test shows a write that clears them |
| `test_integ_hold` | The integration hold (`docs/DESIGN.md`, section 8). A fake core that reads bus-off after ENA until the test ends it, and aborts a Set ready at bus-off when TBFBO = 1. Held at start: a queued frame causes no TX_COMMAND write and no bus-off count. Released by FCSI with error active, and by the single FAULT_STATE read in the TX path when no FCSI comes (a bus-off after that is still counted). The same after a `SET_TIMING` restart, direct and deferred behind stuck frames. With FAULT_STATE reading 0 (as QEMU 6.2 does), error active or error passive at start or at a restart, nothing is held and the frame is sent. The fake core reads 0 after a reset by default, as QEMU does, so every other host suite runs with that reading. The fake core is the only model of the integration phase: QEMU 6.2 has none. Also builds against an older driver (`make -C tests/host integ_hold_base BASE=/path/to/other/src`), where it fails |
| `test_time` | 32-bit timestamps across the 2^32 us wrap, offsets set before and after a wrap, the 64-bit value counting on with the same low 32 bits |
| `test_core` | Identification, soft reset and its timeout, the register writes of start and stop and their order (interrupt output off first and on last, timing before the core joins the bus); the retransmission limit fields for every `-R` value |
| `test_driver` | The driver's own `main()` up to `can_resmgr_start()` and its shutdown; the TX ring (FIFO retirement, one TX command per refill, order breaks counted, failed, aborted and lost buffers, the ring going round from its last buffer with 1 to 4 buffers); bus-off (TX held through none and error passive, released at error active, in-flight frames aborted, ERCRST with `-a` and on RECOVER); warning limit, error passive and bus error types; interrupt service (spurious pulses, 64 frames per pulse, 8 passes per pulse, output off at every `InterruptUnmask()`, also when register writes are posted, a stopped core left quiet, refused unmasks counted); reader wake-ups (one return per waiting mailbox, RBNEI held while returns are left, a mailbox that wakes nobody dropped after 8 pulses); every devctl size check and refusal; `-R` and `-E`; the TX wait (`test_tx_wait`): bus-off aborts that finish on the next pulse and ones given up when the timer fires, with and without `-a`, a deferred `SET_TIMING` restart ended by a pulse, by the FINISH deadline, by the ABORT deadline, and the latest of two timings winning, the timer path and devctl code for `-u 3`, a stray timer devctl changing nothing, and SIGTERM shutdown with stuck, finishing and pending-restart TXT buffers (the only virtual sleep, at most 40 ms); a TX wait deadline and 32-bit timestamps across the 2^32 us wrap (`body_wrap`); the slow-TX log line, its rate limit and count (`body_slow_tx`) |
| `test_fi` | The fault-injection build (`src/ctucanfd_fi.c` over the fake core): injected warning, bus-off and held TX completions go through `service()` and `finish_pulse()` on a tick, with no `InterruptUnmask()`; the RX budget holds next to an injected error, and readers reached during a tick are left to the next interrupt pulse with RBNEI held; bus-off with aborts slower than the 20 ms bound returns at once with the TX wait on, and the wait timer gives the aborts up at 20 ms with no sleep (`body_busoff_slow_abort`) |
| `test_txw_orders` | The TX wait under every order of 12 events, 4 deep, from 15 start states: 311,040 sequences, each on a freshly started driver, with the invariants below checked after every event and every millisecond of virtual time |
| `test_no_sleep` | No sleep on libcan's thread: a `SET_TIMING` restart, a bus-off and a bus-off with `-a`, each with 4 frames whose aborts never finish, while a client asks for statistics. The fake libcan counts every `nanosleep()` made inside the driver's callbacks (event handler, transmit, devctl); the count must be 0, and all 4 frames must be given up. Also builds against an older driver (see below) |
| `test_rx_restart` | No RX frame lost to a restart: 10 frames in the RX FIFO and 10 blocked readers before a `SET_TIMING` restart with nothing in flight, before one deferred behind 4 frames whose aborts never finish, and during a bus-off with `-a` and the same 4 frames. No pulse runs until the restart is over. All 10 frames must reach the readers in order and the reset must flush none. Also builds against an older driver (see below) |
| `test_supervisor` | `tools/ctucan-supervisor.c` with its spawn and sleep replaced: a SIGTERM during the restart delay starts no driver |

The fake core models only what the code relies on, as `src/ctucanfd_regs.h` and `docs/DESIGN.md` describe it. It is not a model of the silicon or of QEMU. The driver runs on a virtual clock (`fake_clock_gettime()`, `fake_nanosleep()`), and the fake wait timer (`ctucan_timer_*` in `tests/host/fake_libcan.c`) records each arm and, when a test fires it, sends `CTUCAN_DEVCTL_TIMER` to the device path the driver gave it, as the helper process does on QNX. The tests check that libcan's thread never sleeps: the virtual sleep total stays 0 except in shutdown. The fake libcan follows the DDK text quoted in DESIGN.md section 3.1: the event handler is called until it returns NULL, and each returned RX mailbox wakes one blocked reader. It says nothing about how libcan schedules pulses against clients. The headers in `tests/host/shim/` define the QNX names the driver uses so it builds without the SDP; they are our own stand-ins, written from how this repository uses the names, not QNX's headers. Real interrupts, the PCI server and libcan itself are covered only by the end-to-end suite below.

### No sleep in a libcan callback (`test_no_sleep`)

`tests/host/test_no_sleep.c` uses only what every version of the driver has: client writes, interrupt pulses, the standard `SET_TIMING` devctl, `GET_XSTATS` fields and the fake core. So it also builds against another tree's driver source, with that tree's own headers and this tree's fakes:

```
make -C tests/host no_sleep_base BASE=/path/to/other/src
```

Results with clang, ASan and UBSan:

| Driver | `SET_TIMING` restart | bus-off | bus-off with `-a` | Result |
|---|---|---|---|---|
| main 30fff05 (sleep-poll) | 200 sleeps in callbacks | 100 | 100 | fail 3 of 3 cases |
| this tree | 0 | 0 | 0 | pass 3 of 3 cases |

In both drivers all 4 stuck frames are given up and the restart runs once, so the cases do what they claim. The one sleep left in this tree is `tx_shutdown_wait()`. It is called only from `teardown()`, which runs after `can_resmgr_start()` has returned or when start-up fails before it, so never on libcan's thread while clients are served. The test does not count it.

### No RX frame lost to a restart (`test_rx_restart`)

The controller reset empties the RX FIFO. Before the reset the driver now reads out the frames in it and queues them for the readers: once while the core is on the bus, and once after it has left the bus. The pulse that RBNEI already brought, or the one the restart runs in, wakes the readers. Bus-off recovery is COMMAND.ERCRST, not a reset, so the FIFO is kept there in every version. Like `test_no_sleep`, the test builds against another tree's driver:

```
make -C tests/host rx_restart_base BASE=/path/to/other/src
```

Results with clang, ASan and UBSan:

| Driver | plain restart | restart behind 4 stuck frames | bus-off with `-a` | Result |
|---|---|---|---|---|
| main 30fff05 | 10 of 10 flushed, 0 read | 10 flushed, 0 read | 10 of 10 read | fail 2 of 3 cases |
| this tree before the drain (0110e39) | 10 flushed, 0 read | 10 flushed, 0 read | 10 of 10 read | fail 2 of 3 cases |
| this tree | 10 of 10 read in order, 0 flushed | 10 of 10, 0 flushed | 10 of 10 | pass 3 of 3 cases |

### Every order of TX wait events (`test_txw_orders`)

`tests/host/test_txw_orders.c` runs every sequence of 4 events from a 12-event alphabet against the TX wait (DESIGN.md section 5). There is no randomness: all 12^4 sequences run from each of 15 start states, 311,040 runs in all. Each run starts the driver afresh.

- Alphabet: the core sends its highest-priority ready buffer (SEND); an interrupt pulse (PULSE); aborts in progress finish (ABTDONE); a client write (WRITE); a plain devctl (DEVCTL); `SET_TIMING` (TIMING); bus-off with its pulse (BUSOFF); error active with its pulse (RECOVER); 5 or 20 ms with the wait timer firing on time (ADV5, ADV20); 25 ms with the timer's helper late (LATE25); the late timer firing (FIRE).
- Start states: 1, 2 or 3 frames in flight, or 4 in flight and 2 queued; FINISH with a restart pending; ABORT with aborts stuck; ABORT after bus-off, with and without `-a`. Each comes with aborts that finish at once and with aborts that finish only on ABTDONE, so a run without ABTDONE has aborts that never finish. The ABORT-after-restart state has stuck aborts only.
- Invariants: every frame written is counted once (queued, in flight, sent, aborted), and the sent count matches what went on the wire. Frames reach the wire in write order. The virtual sleep total stays 0. A wait with the timer on time ends within 40 ms of its start, or within 20 ms of its last bus-off. A buffer is given up only after its abort had 20 ms. Nothing is set ready during a wait or a bus-off hold, and queued frames move again once both are over. A requested restart runs exactly once, after the wait ends, with no frame in a TXT buffer and never inside a service batch. The wait timer is armed exactly while a wait is on.
- After each sequence the test drains: 50 ms with the timer on time and a pulse, which must end any wait, then a working bus until nothing is queued or in flight.
- Not modelled: RX traffic, failed or lost TXT buffers, a frame that completes while its abort is pending, error passive, a reset that never completes, posted writes, more than one TX mailbox and shutdown.
- The test watches the core through read and write observers in `fake_core.c` and the wait timer through an arm observer in `fake_libcan.c`. These observers only record what happens.

## Signature tests (`scripts/tests/signature.py`)

Target tests for properties that need the driver's design, run on their own: `scripts/tests/signature.py --target a --if vcan0 [--rate N] [freeze_free] [rx_loss] [kill_storm]`. They use only ctutest.py's helpers, the standard CAN devctls, the fault-injection scenarios main has, and `tools/ctucan-latprobe.c`, which sends `CAN_DEVCTL_GET_STATS` back to back and reports the longest answer. So the same file runs against older drivers.

- `freeze_free`: bus-off, and a `SET_TIMING` restart, each with 4 frames that never finish. Passes only if both hold: with nothing else running, the frames are given up at the 20 ms bound (`tx_abort_forced 4`), and while a second client asks for statistics back to back no answer takes over 5 ms.
- `rx_loss`: a `SET_TIMING` restart halfway through 2 s of 64-byte frames with sequence numbers from the host at 1000 frames/s, read by `canfd-test seqrecv` on the target. A plain restart with nothing in flight runs first, on a fresh driver and the same load, as the control. Then the same restart with 4 frames in the TXT buffers that never finish. Pass: no RX FIFO overflow, nothing out of order or bad, and no more frames lost than the control lost. The result line also says whether no frame was lost at all. 1000 frames/s is well inside what the bus carries (about 2700 per second for 64-byte frames at 500 kbit/s and 2 Mbit/s). On the QEMU target every driver takes in about 2000 per second at most and overflows the FIFO above that, so a higher rate tests QEMU, not the driver.
- `kill_storm`: ctucan-supervisor, ecu-sim at 1 ms, 500 frames/s of host load, 60 SIGKILLs 0.5 s apart, half by name and half of the driver alone. One gap per kill, telemetry order and CRC intact, every death and restart seen by the supervisor, and the same number of driver processes after as before, none DEAD.

AWS run 2, QNX 8.0 in QEMU, 3 rounds each, interleaved:

| Driver | `freeze_free` | `kill_storm` |
|---|---|---|
| main 30fff05 | fail 3/3: bound kept by waiting in the request | pass 3/3 |
| an earlier version of this tree, with a timer thread (history, since replaced by the helper process) | fail 3/3: bound missed with no traffic | pass 3/3 |
| this tree (timer helper process) | pass 3/3 | pass 3/3, no helper left behind |

The longest answers are omitted. Run the example locally for full output.

`rx_loss`, AWS run 3, QNX 8.0 in QEMU, 5 rounds each, interleaved. Frames lost by the plain restart (control) and by the restart with 4 stuck frames; in brackets, how many of those the reset flushed from the RX FIFO:

| Driver | control lost | stuck-frame restart lost | `rx_loss` | No frame lost at all |
|---|---|---|---|---|
| main 30fff05 | 1, 1, 1, 1, 1 | 21, 22, 21, 21, 21 (21 flushed each) | fail 5/5 | 0/5 |
| 0110e39, before the RX drain | 1, 1, 1, 1, 1 | 1, 2, 1, 1, 1 (1 flushed each) | pass 4/5 | 0/5 |
| this tree | 0, 1, 0, 0, 1 | 0, 0, 0, 1, 0 (0 flushed) | pass 4/5 | 4/5 |

FIFO overflows were 0 in every run. Main waits 20 ms inside the request with the FIFO unread, and the reset then flushes what came in. In this tree the reset flushed nothing; the one frame lost in round 4 was not in the FIFO, so it came while the core was off the bus. That round fails the rule because its control lost none.

`kill_storm`, AWS run 3, 3 rounds each, interleaved: main 30fff05 pass 3/3, this tree pass 3/3. One earlier run of 0110e39 failed the count: 64 gaps over 5 ms for 60 kills, so 4 extra. Main showed 2 such extra gaps in another run. A 5 ms threshold sits close to what host load alone can cause. About half the gaps are long on every driver: kills come 0.5 s apart, and the supervisor waits 50 ms before it restarts a driver that died less than 1 s after its start. With kills 1.2 s apart the supervisor added no delay.

## The suite and its results

`scripts/test-suite.sh tc vcan0 [tc2]` builds both variants and runs `scripts/gate-test.sh` and every test in `scripts/tests/ctutest.py` against the QEMU target, keeping the output in `build/test-results-*.txt`. With a second target on the same interface (`tc2`) it also runs the two-node test; without one that test is skipped. The scripts reach the target through `scripts/qtarget-lite.sh NAME ssh|put`, which finds the target in `~/targets/NAME` (or in `$TARGETS/NAME` when `TARGETS` is set) with `mkqnximage --getip`; set `QTARGET` to use another helper with the same commands. With `JUNIT=FILE` the suite also writes JUnit XML, one test case per test, with pass, fail and skip and no times (`ctutest.py --junit`). `scripts/tests/test_junit.py` checks that writer and that the run order lists every test once; the `checks` workflow runs it. `.github/workflows/qemu-e2e.yml` runs the whole suite on a self-hosted runner labelled `qnx-sdp` and `kvm`, started by hand; it keeps the log and the JUnit file only when asked, because the log holds QEMU timing figures. The host side uses a raw SocketCAN socket and `candump -L`, and checks complete frames: identifier, format, length, every payload byte including padding, and sequence numbers.

Results by driver commit. The 41-test record and the runs on 3dfda81 are history. They are not the status of the latest commit. The suite has 43 tests today, plus the gate test.

| Driver commit | Suite size | Runs | Result |
|---|---|---|---|
| c9c7745 | 41 tests | 1, two QNX 8.0 x86_64 targets under QEMU 6.2 with KVM, from a clean clone with `QTARGET=scripts/qtarget-lite.sh` (recorded in `docs/test-results.txt`) | gate test and 41 of 41 tests passed, two-node test included; every binary copied to the target was checked by md5 |
| 3dfda81 (the merge of the held branches) | 43 tests | 5, two targets | gate test passed in all 5; 43 of 43 tests passed in 4; in the third run `restart_under_load` failed |

The runs on 3dfda81 are not in `docs/test-results.txt`. In the failed run, besides the three gaps from the kills, the host saw one more telemetry gap over the 5 ms limit, before the first kill. Main does the same on this host: run alone, the test failed 1 of 12 times on main and 1 of 12 times on 3dfda81, then passed 20 of 20 times on 3dfda81. So the extra gap does not come from the merged changes. The cause was not traced. The later runs on QNX 8.0 are in the next section.

### Results on QNX 8.0, by driver commit

Counts only. Each full run is `scripts/test-suite.sh a vcan0 b` on two QNX 8.0 x86_64 targets under QEMU with KVM. A full run prints 44 PASS lines: the gate test and the 43 tests of `scripts/tests/ctutest.py`, whose own summary reads "43 of 43 passed". Older notes in the team's records call this "44 tests". The 41-test run recorded in `docs/test-results.txt` has the same shape: the gate test and 41 tests. `rx_loss` is in `scripts/tests/signature.py` and runs on its own, so it is not part of the 44. The records are in the team's results folder, which is not public; the paths are given as text.

| Driver | Full runs | Result | Record |
|---|---|---|---|
| main 30fff05 | 5 | 44/44 in 5 of 5 | results/ctucanfd-run2-report.md, results/aws-run-2/can2/can-main-*.txt |
| the helper-process fix, PR #7 | 5 | 44/44 in 5 of 5; after each run the five timer-dependent tests ran 10 times each: 250/250 | results/ctucanfd-run2-report.md, results/aws-run-2/can3/ |
| PR #8 (RX drain) | 3 | 44/44 in 3 of 3 | results/aws-run-3/can/drain/ |

That is 13 runs, all passing. The timer-thread version of the fix, tried before the helper process, showed 43 of 44 PASS lines in each of 5 runs: `fi_busoff_abort_notick` failed each time because the thread could not reach the driver's own device. It is history.

`kill_storm`: 60 SIGKILLs, three times per driver, under load. Every driver passed all three. After every kill the supervisor had a ready driver, the number of driver processes after the run matched the number before, and none was left dead (results/aws-run-2/sig/, results/aws-run-3/can/storm3/).

`restart_under_load` diagnostics. The checks are unchanged; when the gap count is wrong, the failure message adds each gap's start relative to the nearest kill (and how long that kill's ssh took), the driver's `slow TX` slog2 lines, and ecu-sim's `overrun` and `EAGAIN` lines (at most 32 per run). Hypotheses, most likely first, and how to read the lines:

1. A TX completion is serviced late. libcan's thread runs at priority 10 and takes the interrupt pulse in priority order with client messages; with host load, ssh sessions and the 500 frames/s RX stream on the target, the completion pulse can wait. A client write calls `transmit()`, which refills free TXT buffers but did not retire finished ones, so the ring can sit full at 4 buffers until the pulse runs. Sign: `slow TX` lines with `final seen N us before` above 0 (the buffer was final while the driver had not retired it), few `pulses since`, a large `prev gap`, and EAGAIN lines from ecu-sim at the same time. A candidate fix is to retire finished buffers in `transmit()` too; it is not made, because a gap has not yet been shown to come from this.
2. A stall of the QEMU process or the host around the start of the load: the gap is outside the kill windows, and there are no `slow TX` lines and no ecu-sim overruns, because nothing on the target saw time pass oddly.
3. ecu-sim's thread woke late (an overrun): an `overrun at seq` line from ecu-sim whose skipped periods match the gap, with no `slow TX` line.

- Every length from 0 to 64 from QNX to the host, and every classic and CAN FD length to QNX, checked byte for byte including padding; remote frames both ways; 11-bit and 29-bit ids both ways, including 29-bit ids whose low 18 bits are zero.
- Four blocked readers on one mailbox: each of 200 frames reached exactly one reader and all four finished. Four writers at once: 4 x 100 frames, each stream in order. With no reader, the 64-frame mailbox queue kept the newest 64 of 100 frames.
- 2000 frames of 64 bytes each way at the same time, 2 ms apart: both streams complete and in order.
- Bounded interrupt service: 300 frames in bursts of 90 arrived in order in 7 interrupt pulses of at most 64 frames, and a client's devctls were answered 757 times while frames were still coming; with 4-frame batches the client was answered 200 times in 23 pulses. A pulse that arrives while the core is stopped after a failed reset is unmasked with the core's interrupt output left off (1 pulse, then none in the next second), and a mailbox whose wake-ups fail is retried for 8 pulses and then dropped and counted.
- TX order across four TXT buffers: 2000/2000 in order with no pacing. With completions held back by the fault injector, 1000/1000 in order across 249 TX commands that readied several buffers at once; the same run with the first version's fixed priorities broke order 996 times (negative control).
- Restart under load, 1 ms workload (`restart_under_load`): ecu-sim telemetry on 0x2F8 every 1 ms, the host sending 500 frames/s of 64 bytes into QNX, three `slay -s KILL dev-can-ctucanfd` 2 s apart with the supervisor: telemetry resumed after each kill.
- Supervisor: it kills and restarts a driver that does not become ready, takes a driver as ready only when the process it started serves the path, abandons a readiness probe that a server never answers, and counts spawn failures.
- ecu-sim for 30 s at its 10 ms period: 3032 frames, CRC and sequence clean; the diagnostic request was answered with the right reply.
- Two QNX nodes: see the section above.
- Fault injection: warning limit up and down, error passive, bus-off with automatic recovery (300/300 frames in order), bus-off whose FAULT_STATE reads none and then passive before active (TX stayed held until active), bus-off with manual recovery (no frame left while bus-off, the writer got EAGAIN, all 100 queued frames left in order afterwards), aborts that do not finish in time, also with no traffic and no ticker (dropped and counted by the driver's wait timer 20 ms after bus-off, TX resumed), a `SET_TIMING` restart deferred until the TXT buffers are done (`fi_quiesce`), late TX completions with one RX frame per pulse (none lost), a restart whose reset times out followed by one that works, a corrupt RX format word among payloads that look like format words (no frame fabricated), and state cleared by scenario OFF and by a reset. The fault injector changes only what the driver sees; QEMU sends each frame when it is set ready (docs/DESIGN.md, section 8).
- Timestamps: the 32-bit value wraps at 2^32 us and the 64-bit devctl keeps counting with the same low 32 bits (`timestamp64`). Frames are counted by format both ways and the counts match what the host sent and saw (`fd_flag_counts`).
- Also: short devctl payloads refused, SJW larger than a phase segment refused, SIGTERM reaches the cleanup path, a spoofed interrupt pulse is refused by the kernel and counted, and the normal build refuses the fault-injection devctls.
