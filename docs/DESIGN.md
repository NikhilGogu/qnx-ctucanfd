# ctucanfd: design

`dev-can-ctucanfd` is a native QNX 8.0 CAN driver, built on the CAN DDK (`libcan.a`), for the CTU CAN FD IP core on PCI. QEMU emulates that board as `ctucan_pci`, so CAN and CAN FD software for QNX can run in QEMU with no hardware and talk to host tools over SocketCAN. The only other QEMU CAN path for QNX that we found, dev-can-linux, runs Linux SJA1000 drivers (kvaser_pci), which cannot carry CAN FD.

Every number in this file comes from a command run on the build box (Ubuntu 22.04, QEMU 6.2.0) against a QNX 8.0 x86_64 target built with mkqnximage. Source reading of QEMU's model is marked as such.

## 1. The hardware as QEMU 6.2 models it

QEMU side (`qemu-system-x86_64 -device ctucan_pci,help`): two link properties, `canbus0` and `canbus1`, one per core. The board refuses to realise unless both are set. The bus object id must not be the bare string `canbus0`: QEMU then fails with `Path 'canbus0' does not uniquely identify an object`, so the scripts use `canbus0-bus`.

`info pci` in the QEMU monitor:

```
Class 3081: PCI device 1760:ff00   (class 0x0c09, CAN bus controller)
IRQ 10, pin A
BAR0: 32 bit memory at 0xfeba0000 [0xfeba7fff]
BAR1: 32 bit memory at 0xfeb90000 [0xfeb9ffff]
```

QNX side, `pci-tool -vvv -d 0:5:0`: `vid/did: 1760/ff00`, `class/subclass/reg: 0c/09/00 CANbus Serial Bus Controller`, `cmd/status registers: 103/0`, two 32-bit MEM address spaces, both `ENABLED`.

`probe/ctucan_probe.c` attaches through the PCI server, maps both BARs and reads:

```
irq: 10 (of 1)
BAR0+0x0000 bridge id: 0xc0000002 (cores=2)
core0 DEVICE_ID=0xcafd VERSION=2.2 (raw 0x0202cafd) OK
core0 MODE=0x00000010 STATUS=0x00000084 RX_MEM_INFO=0x08000800 TX_STATUS=0x00008888 YOLO=0xdeadbeef
core1 DEVICE_ID=0xcafd VERSION=2.2 (raw 0x0202cafd) OK
```

So BAR0 holds the bridge id (core count in bits 3:0) and BAR1 holds the cores, one every 0x4000 bytes. The core identification registers are in BAR1, not BAR0. After reset each core reports FDE set (MODE 0x10), idle with TX buffers free (STATUS 0x84), an 8 KiB RX FIFO that is empty (0x800 words, 0x800 free) and four empty TXT buffers (0x8888).

What the QEMU 6.2 model leaves out or does differently (from reading `hw/net/can/ctucan_core.c` and `ctucan_pci.c`, confirmed where a register read could show it):

| Behaviour | Consequence for the driver |
|---|---|
| No bit timing: BTR and BTR_FD read back `0x00000000` after the driver wrote them | Timing is computed and written for silicon; QEMU ignores it |
| No error counters, fault confinement, bus errors or bus-off | Error paths cannot be exercised in QEMU (section 8) |
| A TXT buffer goes ready to TX OK inside the TX_COMMAND write | TX completion interrupt is pending before the write returns |
| A ready buffer with priority 0 is never sent; all ready buffers are sent inside one TX_COMMAND write, highest priority first | Rotating priorities 7 down to 4 (section 5) |
| Interrupt output is INT_STAT AND INT_ENA; INT_MASK only stops new bits latching (also datasheet 2.12) | The output is switched off with INT_ENA_CLR (section 3) |
| A remote frame from the host is stored with DLC worth of data words (silicon stores none) | RX accepts both word counts for RTR frames |
| Disabling the core (SETTINGS.ENA = 0) does not empty the TXT buffers, unlike silicon | Restart quiesces TX first, then resets the core |
| EWL_ERP_FAULT, REC_TEC and ERR_CAPT read 0 | FAULT_STATE reads "none of active, passive, bus-off"; the driver treats that as not bus-off |
| Writes narrower than 32 bits are not shifted into place | All register access is 32-bit, 16-bit fields go through their word |
| Both cores drive one `qemu_irq`; each core raises or lowers it alone | Use one core per board for interrupt-driven nodes; a second node gets a second board |
| No bus timing: frames from the host land in the FIFO as fast as the host sends | Unpaced bursts overflow the FIFO after 102 frames of 64 bytes |

## 2. Driver architecture on the CAN DDK

### 2.1 Model

The CAN DDK (QNX docs: `com.qnx.doc.ddk/topic/can/`) defines a driver as a standalone executable that statically links `libcan.a`. libcan creates one resource manager path per mailbox, `/dev/canU/rxN` and `/dev/canU/txN`, on a single-threaded dispatch loop. The driver supplies callbacks in `can_drvr_funcs_t`:

- `transmit(CANDEV *)`: libcan has queued a frame on a TX mailbox.
- `devctl(CANDEV *, io_devctl_t *)`: called first for every devctl. Return a byte count for data, `EOK`, or -1 with errno; errno `ENOTSUP` hands the command back to libcan's default handling (that is how `CAN_DEVCTL_WRITE_CANMSG_EXT` and `CAN_DEVCTL_READ_CANMSG_EXT` reach the queues).
- `event_handler(void *hdl, int code)`: called for the interrupt pulse. It returns the `CANDEV` that has news (libcan then replies to blocked readers or refills a TX mailbox) and is called again until it returns NULL.

Queues are libcan's: each `CANDEV` has a `free_queue` and a `msg_queue` of `canmsg_t`, moved with `canmsg_dequeue_element()` and `canmsg_queue_element()`; the DDK's `CANDEV` page says the driver may change only `cflags` and must manipulate the queues through those two functions. The resource manager "is single-threaded, and all callbacks are executed in the same thread" (DDK overview). The driver links for x86_64 with `-lcan -lpci -lsecpol -lslog2`.

Device numbering follows QNX's convention: RX mailboxes first, TX numbered after them. With the defaults that is `/dev/can1/rx0` and `/dev/can1/tx1` for core 0.

### 2.2 Source layout

| File | Role |
|---|---|
| `src/ctucanfd_regs.h` | Register map and bit fields, written from the datasheet; our own definitions |
| `src/ctucanfd_hw.[ch]` | One core: identify, reset with timeout, start/stop, TXT buffer write and commands, rotating priorities, validated RX pop, DLC tables, bit timing. No libcan or OS state |
| `src/ctucanfd_fi.c` | Fault injector for the hardware layer, only in `make FI=1` builds (section 8) |
| `src/ctucanfd_devctl.h` | Driver-specific devctls and pulse codes, shared with the tools |
| `src/dev-can-ctucanfd.c` | Options, PCI attach, libcan glue, bounded interrupt service, TX ring, error policy, devctls, shutdown |
| `tools/canfd-test.c` | Client: send, write, recv, sequenced send/receive, stats, timing, recovery, fault injection and its ticker, devctl monitor, pulses |
| `tools/ecu-sim.c` | Simulated battery-management ECU (section 10a) |
| `tools/ctucan-supervisor.c` | Restarts the driver when it dies (section 9) |
| `probe/ctucan_probe.c` | Identification probe |
| `scripts/qemu-can.sh`, `host-vcan.sh` | QEMU launch with CAN, host vcan0 |
| `scripts/gate-test.sh`, `test-suite.sh`, `tests/ctutest.py` | Quick check, full suite runner, end-to-end tests |

### 2.3 Startup

1. Options, then bit timing from the core clock (default 100 MHz): 500 kbit/s nominal at 87.5 % sample point (`brp 4 tq 50`) and 2 Mbit/s data at 75 % (`brp 1 tq 50`), SJW no larger than either phase segment.
2. `ThreadCtl(_NTO_TCTL_IO_LEVEL, _NTO_IO_LEVEL_1)`. Without I/O privileges the probe died with SIGSEGV during PCI access. The `ThreadCtl()` entry of the QNX 8.0 library reference describes level 1 as port I/O plus interrupt disable and enable for the calling thread, and recommends `_NTO_TCTL_IO_LEVEL` over the older `_NTO_TCTL_IO`. With it, `pci_device_attach()` and the BAR mappings work on tc (driver start log: `B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10`).
3. `pci_device_find(idx, 0x1760, 0xff00)`, `pci_device_attach(..., pci_attachFlags_EXCLUSIVE_OWNER)`, `pci_device_read_ba()`, `pci_device_read_irq()` (gives 10). BAR0's bridge id gives the number of cores: `-c 2` is refused with `core 2 requested, bridge id 0xc0000002 reports 2 core(s)` (exit 19). The 16 KiB window of the chosen core in BAR1 is mapped with `PROT_NOCACHE`.
4. DEVICE_ID must read 0xCAFD; then a soft reset (MODE.RST) that must bring back the reset values within a bounded poll, or the driver exits. A restarted driver therefore never inherits TXT buffers or interrupt state from a dead one.
5. `can_resmgr_init()`, then `can_resmgr_init_device()` and `can_resmgr_create_device()` for every mailbox, each result checked; any failure unwinds what was set up (interrupts, devices, resource manager, mapping, PCI attach) in reverse order.
6. `can_resmgr_attach_intr()` for the interrupt, with pulse code 0; the driver attaches nothing else.
7. Enable the core (SETTINGS.ENA, MODE.FDE) with RBNEI, DOI, TXBHCI, EWLI, FCSI and BEI enabled and every other source masked, set the TXT priorities, then `can_resmgr_start()`.

## 3. Interrupts on QNX 8

The QNX 8.0 migration guide (`com.qnx.doc.qnxsdp.migration/topic/kernel.html`): "Interrupt Service Routines (ISRs) are no longer supported. Instead, QNX has a new mechanism for attaching threads to interrupts: Interrupt Service Threads (ISTs). If you are using events, there is currently no need for you to update your code". `InterruptAttach()` and `InterruptAttachArray()` are listed as removed; `InterruptAttachThread()` is new.

### 3.1 What runs where

The CAN DDK documents the interrupt contract: `can_resmgr_attach_intr()` "associates an IRQ and pulse code with the CAN pulse handler", which calls `event_handler()` when the interrupt occurs; "the interrupt doesn't fire again until it's unmasked"; once the interrupt is fully handled the handler "must call the InterruptUnmask() function and return NULL"; libcan calls `event_handler()` repeatedly until it returns NULL; for a returned TX device it calls `transmit()`, and for a returned RX device it "unblocks the client that was blocked the longest". So all device work runs on libcan's resource-manager thread, which receives the interrupt as a pulse. That thread is not an interrupt service thread. `pidin irq` on tc shows the one attachment: IRQ 10 (`0xa`) delivering a pulse with code 0 to a connection of the driver's process.

### 3.2 Bounded service in interrupt-driven batches

Each interrupt pulse does one bounded batch: at most 64 frames moved from the RX FIFO, at most 8 passes over INT_STAT, at most 64 RX mailbox returns to libcan, and the TXT buffers that have finished (four at most, judged from one TX_STATUS snapshot per pass, never cut short by the RX budget). The pulse ends with exactly one `InterruptUnmask(0, iid)`, checked (`unmask_err`). There is no second pulse code and no pulse the driver sends itself: only documented API is used.

Work that is left over stays visible to the core as a pending interrupt status bit. Frames left in the FIFO keep RBNEI set (the core sets it again while the FIFO is not empty). Reader notifications left over keep RBNEI set too: the driver acknowledges RBNEI only after the pulse's mailbox returns are done (`notify_deferred` counts the pulses that held it). TX completions that arrive during the batch set TXBHCI again. Before the unmask the driver turns the core's interrupt output off (INT_ENA_CLR; the output is INT_STAT AND INT_ENA, INT_MASK only stops bits latching, datasheet 2.12 and QEMU's `ctucan_update_irq()`), and after it turns the output on again, so any pending bit raises the line afresh and the kernel delivers the next pulse. Between two pulses libcan's thread returns to its message loop and serves clients.

Measured on tc (`docs/test-results.txt`):

- `burst_batches`: 300 host frames of 64 bytes in bursts of 90 (more than one batch): all 300 reached the reader in order in 7 interrupt pulses, 3 of which left work for the next one; largest batch 64 frames. A client issuing `CAN_DEVCTL_GET_STATS` back to back got 758 answers while frames were still coming in, and its longest devctl took 33028 us at this load.
- `fi_batches`: the same with the batch forced down to 4 frames: 90/90 frames in order in 23 pulses, `unmask_err` 0, 87 devctl answers while frames were coming, longest devctl 2574 us at that load.
- `fi_reader_budget`: one mailbox return per pulse and readers blocked on four mailboxes: every reader finished, with 30 pulses holding RBNEI for readers. An earlier version of this test put all four readers on one mailbox: there was one interrupt pulse and one mailbox return, and all four readers got their 10 frames, so one return can wake several readers of a mailbox. The test therefore spreads the readers over four mailboxes, where each return wakes only that mailbox's readers.

These latencies were measured at the loads above; they are not a bound. libcan's thread takes pulses and client messages in priority order (MsgReceive), so a sustained stream of interrupts at a priority above the clients' can still keep a client waiting for longer (section 12).

While the core is stopped (a restart whose reset failed, or shutdown) an interrupt pulse is unmasked as usual but the core's output is left off, so status left in the core cannot bring pulse after pulse (`stopped_pulses`). `fi_reset_storm`: a reset that never reaches the core, with RX frames left in the FIFO and a pulse queued behind the restart, gave 1 such pulse and then no pulse in the next second; the same test with this check removed from the driver saw 693 pulses.

A client that sends a pulse with the interrupt code would make the driver unmask an interrupt it did not mask; the kernel refuses when nothing is masked and the driver counts it (`spoofed_irq_pulse`). The driver cannot tell such a pulse from a real one; section 12.

### 3.3 Lost interrupt (stage one) and the output toggle

The stage-one driver stalled under a 1 ms paced burst: the controller held a full FIFO and a pending, enabled interrupt that never fired (`INT_STAT 0x00000409`, `RX_STATUS 0x00000660`). An event that arrived between the last INT_STAT read and the unmask left the line asserted, and no new interrupt came for a line that was already high. Stage one re-read INT_STAT after unmasking and looped. The driver now disables the core's interrupt output before unmasking and enables it afterwards, so a pending event always produces a fresh rising edge after the unmask, whatever the interrupt controller does with a line that is already asserted. `rearm_pending` counts the times an event was pending at that point.

### 3.4 Explicit IST (not in v0.1)

QNX recommends explicit interrupt threads (`InterruptAttachThread()`) "to increase efficiency and control". An explicit IST could drain the RX FIFO into a ring at a priority above client threads, so RX latency would not depend on how busy libcan's thread is. v0.1 does not do this: nothing measured so far shows dispatch contention, and the bounded service above already keeps one interrupt from monopolising that thread.

## 4. RX path

The RX FIFO holds frames as a frame format word (DLC, RTR, IDE, FDF, BRS, ESI, RWCNT), an identifier word, two timestamp words and up to 16 data words. The driver checks the format word against itself before it reads anything else: RWCNT must be what the DLC and frame format need (3 plus the data words; for a remote frame 3, or 3 plus the data words as QEMU stores it), a remote frame must not be CAN FD, BRS and ESI only appear in CAN FD frames, and the frame must fit in what the FIFO holds (RX_MEM_INFO). On any mismatch nothing more is consumed and the whole FIFO is flushed (COMMAND.RRB), counted in `rx_flushes` and `rx_flushed`: where the next frame starts is unknown, and a payload word could pass for a format word. `fi_rx_corrupt` sends frames whose payload words are all valid-looking format words and corrupts one format word: with the frames back to back, 5 were delivered intact and 15 flushed; paced, 19 delivered and 1 flushed; nothing was fabricated in either case.

The identifier word uses the same layout as a libcan MID (11-bit id in bits 28:18, 29-bit id as is), so ids pass through unchanged and `ext.is_extended_mid` comes from IDE.

Each RX mailbox has a MID, a filter and an identifier format. A frame goes to every mailbox with filter 0, and to a mailbox with a non-zero filter when `((mid ^ mbox.mid) & mbox.mfilter) == 0` and the frame has the mailbox's format (11-bit by default, 29-bit with `-x`). When a mailbox queue is full the oldest frame is dropped (`sw_receive_q_full`). A FIFO overrun clears STATUS.DOR before DOI, as the datasheet requires, and is counted in `hw_receive_overflows`.

An RX mailbox is returned to libcan as long as it has both queued frames and blocked readers (the `msgcnt` and `cnt` fields of its queues in `<hw/libcan.h>`), up to 64 returns per pulse; a mailbox whose counts do not shrink after a return is left alone for that pulse. Returns left over are continued by the next interrupt pulse (section 3.2). A return that wakes nobody (neither count shrinks) keeps the notification pending, so RBNEI stays held, for up to 8 pulses in a row; after that the mailbox's queued frames are dropped, counted (`notify_stuck`, `rx_discarded`) and logged, and RBNEI is released, so neither the readers are left behind frames nor do pulses go on. `fi_reader_nowake` makes every return wake nobody: 5 frames gave 5 such recoveries in 40 pulses, and once wake-ups worked again the same blocked reader got the next 3 frames. With four readers blocked on one mailbox, each of 200 host frames reached exactly one reader and all four finished (`readers` test).

QEMU leaves the timestamp words at zero, so `ext.timestamp` is CLOCK_MONOTONIC in microseconds, adjustable with `CAN_DEVCTL_SET_TIMESTAMP`.

## 5. TX path

libcan queues a frame and calls `transmit()`. The driver takes frames round robin across TX mailboxes and keeps up to four in the core's TXT buffers (`-T 1..4`, default 4). A frame goes into the buffer at the head of a ring only if TX_STATUS shows that buffer writable (Empty, TX OK, TX failed, Aborted; datasheet 2.9). The driver copies what it needs (sequence, id, length, format) into its own slot for that buffer and returns libcan's queue element at once. All buffers filled in one pass are set ready with one TX_COMMAND.

Order: the core sends the ready buffer with the highest priority first. The oldest buffer in flight gets priority 7 and each later one one less; when the oldest finishes, the priorities rotate so the next oldest gets 7 (the scheme Linux's ctucanfd driver uses). QEMU 6.2 never sends a buffer with priority 0, so the priorities run 7 down to 4.

Completion: on TXBHCI the driver acknowledges it, takes one TX_STATUS snapshot and retires buffers strictly from the oldest, as many as have finished (four at most, not limited by the RX budget, so a completion is never left behind once TXBHCI is cleared): TX OK counts as transmitted, TX failed as a frame error, Aborted as aborted, and Empty while a frame was in flight as lost (`tx_lost`). A later buffer that finishes while an older one is still pending counts as an ordering fault (`tx_order_err`); both decisions use the same snapshot. A plain `write()` has `mid == CAN_MSG_MID_UNKNOWN`; the mailbox then supplies the id and its format (11-bit, or 29-bit with `-x`), and the frame is a data frame whatever the reused queue element held before.

Restart and shutdown: before a `CAN_DEVCTL_SET_TIMING` restart or a shutdown, TX admission stops and the driver waits up to 20 ms for the TXT buffers to finish, then aborts the rest and waits up to 20 ms again. Aborted frames are dropped and counted (`tx_aborted`); frames still in libcan's queues are kept and sent after a restart.

Tests (`scripts/tests/ctutest.py`, numbers from `docs/test-results.txt`):

- `tx_order`: one writer with no pacing, 2000 frames: all 2000 reached vcan0 in order. In QEMU a TXT buffer completes inside the TX_COMMAND write, so the driver rarely has several frames ready at once (max in flight 3, 0 multi-buffer commands in that run).
- `fi_tx_order`: the fault-injection build holds each completion until a 4 ms grid point at least 5 ms later and releases at most three per millisecond, oldest first, so the driver refills several buffers in one command at changing ring positions: 1000/1000 frames in order across 249 multi-buffer TX commands, max 4 in flight, `tx_order_err 0`. QEMU sent each frame when it was set ready, so this checks the order the driver sets buffers ready and the priorities QEMU applies to buffers ready together.
- `fi_tx_order_fixed`: the same run with the stage-one fixed priorities (buffer n gets n + 1) as a negative control: 996 order breaks in 1000 frames. The ordering test does detect a wrong priority scheme.

## 6. CAN FD

The framework carries CAN FD: `CAN_MSG.dat[CAN_MSG_DATA_MAX]` with `CAN_MSG_DATA_MAX_CANFD 64`, and `CANDEV.cflags` bit 31 (`CANDEV_CFLAG_CANFD`). `CAN_MSG` has no per-frame FDF, BRS or ESI flags, so the frame format is a driver policy:

- CAN FD is on by default (`-C` for classic only). Frames longer than 8 bytes go out as CAN FD with BRS (`-S` turns BRS off; `-A` sends every frame as CAN FD).
- Lengths are rounded up to the next CAN FD size (12, 16, 20, 24, 32, 48, 64) and zero padded; the `tx_lengths` test checks every length from 0 to 64 on vcan0, payload and padding byte for byte.
- `CAN_DEVCTL_WRITE_CANMSG_EXT` is refused (EINVAL) above 64 bytes (8 in classic mode), and for a remote frame that would have to be CAN FD (more than 8 bytes, or `-A`), since CAN FD has no remote frames. Remote frames of up to 8 bytes go out as classic RTR frames.
- RX accepts classic and CAN FD frames alike. The API cannot report FDF, BRS or ESI to clients; using `CAN_MSG.rsv` for that would need QNX's agreement first, so it is not done.

QEMU's host bridge enables CAN FD only when the host interface MTU is 72 (`CANFD_MTU`), so `host-vcan.sh` sets `mtu 72`.

## 7. Devctl and canctl

Handled by the driver: `GET/SET_MID`, `GET/SET_MFILTER`, `GET/SET_TIMESTAMP`, `GET_INFO`, `GET_STATS`, `ERROR` (FAULT_STATE word, REC/TEC, ERR_NORM/ERR_FD, STATUS), `SET_TIMING` (nominal; restarts the controller as in section 5), `DEBUG_INFO` (register dump to slog2). Payloads shorter than the command needs are refused with EINVAL before any field is read (`badsize`: SET_MID with 2 bytes, WRITE_CANMSG_EXT with 40, SET_TIMING with 4). `SET_MID` masks the id to the mailbox's format. `SET_TIMING` refuses an SJW larger than either phase segment and nominal rates outside 5 kbit/s to 1 Mbit/s. libcan handles the read and write devctls after the driver's checks (ENOTSUP fall-through, DDK devctl page).

Driver-specific devctls (`src/ctucanfd_devctl.h`): `CTUCAN_DEVCTL_GET_XSTATS` (pulses, deferred batches, unmask errors, held reader notifications, RX flushes, TX outcomes, warning and bus-off events, state), `CTUCAN_DEVCTL_RECOVER` (request bus-off recovery without `-a`), and, in fault-injection builds only, `CTUCAN_DEVCTL_FI` (start a scenario) and `CTUCAN_DEVCTL_FI_TICK` (advance it); the normal build answers both with ENOTTY.

The stock `canctl` works against the driver: `canctl -u1,tx1 -i` printed `Message queue size: 64`, `Bitrate: 500000 Baud`, `Bitrate prescaler: 4`, `Sync jump width: 6`, `Time segment 1: 43`, `Time segment 2: 6`. canctl's `-w` takes a raw MID, so a standard id must be shifted left by 18.

## 8. Errors, bus-off and fault injection

Enabled interrupts: EWLI, FCSI, BEI, DOI (plus RBNEI and TXBHCI).

- BEI: the ERR_CAPT error type goes to the matching stats counter (bit, CRC, form, ACK, stuff).
- EWLI fires when the warning limit is reached and when both counters fall below it again (datasheet). The driver follows STATUS.EWL for this, apart from FAULT_STATE, so a warning that leaves FAULT_STATE at error active is still counted (`error_warning_state_count`, `ewl_up`, `ewl_down`).
- FCSI: FAULT_STATE (error active, error passive, bus-off) is compared with the last state; error passive and bus-off are counted and logged with TEC and REC.
- Bus-off policy: TX admission stops at once and stays stopped until FAULT_STATE reads error active; a reading of none or of error passive does not release it (`fi_busoff_latch`: the hold was still set 60 ms in while FAULT_STATE read passive, and the stream showed one 106 ms gap). Frames in TXT buffers are aborted, and the driver waits for those aborts at the point of bus-off, at most 20 ms; what is still not final then is given up and counted (`tx_abort_forced`). No later traffic or timer is needed for that: `fi_busoff_abort_notick` runs with no fault-injection ticker and no traffic, and `fi_busoff_stuck_abort` with recovery afterwards. Frames in libcan's queues stay queued, and once a queue is full writers get EAGAIN from libcan. Recovery: with `-a` the driver writes COMMAND.ERCRST at once (the core rejoins after 128 times 11 recessive bits); without it the core stays bus-off until `CTUCAN_DEVCTL_RECOVER` or a `SET_TIMING` restart.
- A TXT buffer that ends in "TX failed" (retransmission limit) is retired and counted as a frame error.

QEMU 6.2 models none of this (section 1), so the fault-injection build (`make FI=1`, `src/ctucanfd_fi.c`) overlays scripted values on the registers the service loop reads (INT_STAT, STATUS.EWL, FAULT_STATE, REC/TEC, TX_STATUS, YOLO, RX_DATA) and watches the commands it writes (COMMAND.ERCRST, TX_COMMAND, MODE.RST, INT_STAT acks). The script advances on every interrupt pulse and on `CTUCAN_DEVCTL_FI_TICK`, which a test client (`canfd-test fitick`) sends every millisecond. Scenario OFF and a soft reset of the core clear every overlay (`fi_state_clear`). The normal build answers the fault-injection devctls like any unknown command, with ENOTTY (`fi_rejected_normal`).

What the injector does not prove: QEMU sends a frame on the bus the moment its TXT buffer is set ready, so a held completion or an abort changes only the driver's view of the frame. `fi_txdelay`, `fi_tx_budget`, `fi_quiesce`, `fi_busoff_stuck_abort` and the frames counted as aborted show the driver's bookkeeping (admission, queueing, ordering of its own work, counting, recovery); they do not show a frame delayed or cancelled on the wire. The bus-off tests do show that no frame is set ready while TX is held, because QEMU would have sent it. Error counters and fault confinement transitions are scripted, not produced by a bus.

## 9. Restart and recovery

SIGTERM: libcan "registers libcan's signal handler" and `can_resmgr_start()` "blocks until the driver is terminated by the SIGTERM signal" (DDK implementation page). The driver then stops TX admission, waits for or aborts the TXT buffers (section 5), disables the core, detaches the interrupt, destroys the devices, calls `can_resmgr_fini()` and unmaps. The `sigterm` test sends `slay dev-can-ctucanfd` and finds `stopped: tx ...` in slog2 and `/dev/can1` gone.

Supervisor: HAM is not in the target image or the SDP's target tree, so `tools/ctucan-supervisor.c` restarts the driver. The driver leaves its parent once it is running: a shell's `wait` on its pid returned at once while the same pid kept running. So the supervisor does not wait for its child. It asks the process manager for a pulse on every process death (`procmgr_event_notify(PROCMGR_EVENT_PROCESS_DEATH)`) and checks `kill(pid, 0)`, with a 100 ms poll as a fallback. Ready means that `/dev/can1/tx1` is served by the pid the supervisor started (`ConnectServerInfo()` on the connection) and that this process answers `CAN_DEVCTL_GET_INFO`. The probe runs in a forked child that reports through a pipe; the supervisor waits 200 ms for it and then kills and abandons it, so a server that accepts the open and never replies cannot stall the supervisor (`supervisor_hang`, with `tools/test-hangsrv.c` as that server). A driver that is not ready within 3 s (`-r`) is killed and started again; spawn failures, deaths and timeouts all count against `-n`; restarts back off from 50 ms to 1 s. `supervisor_readiness`: a stand-in that never serves the path was killed after 300 ms three times and the supervisor stopped at `-n 2`; with an older driver still serving the path, the new child was not taken as ready; a missing driver binary failed to spawn three times and counted against `-n 2`.

Clients: ecu-sim reopens its devices every millisecond while the driver is away (section 10a).

Measured (`restart_under_load`, `docs/test-results.txt`): supervisor, ecu-sim at a 1 ms period on id 0x2F8, host load of 500 frames/s of 64 bytes into QNX, three `slay -s KILL dev-can-ctucanfd` 2 s apart, `candump -L` on the host. Telemetry gaps 9.3, 9.7 and 8.1 ms (10, 10 and 8 periods lost, shown by ecu-sim's sequence number, which counts periods); the driver was ready 5.4, 5.7 and 8.3 ms after the supervisor spawned it; ecu-sim reopened its device 7.5, 7.8 and 6.4 ms after its first failed write.

## 10. Testing in QEMU

```
scripts/host-vcan.sh vcan0                 # vcan module, mtu 72, link up
qtarget tc create; qtarget tc stop         # any mkqnximage target (README: plain mkqnximage)
scripts/qemu-can.sh ~/targets/tc vcan0     # same QEMU args + CAN bus + ctucan_pci + bridge
scripts/test-suite.sh tc vcan0             # builds both variants, gate test, all of ctutest.py
```

`qemu-can.sh` takes the exact `QEMU Command:` line mkqnximage logged, swaps `-nographic` for `-display none -daemonize` (QEMU refuses the pair), adds a monitor socket and appends the CAN options. The pidfile stays `output/qemu.pid`, so `qtarget status|ssh|stop` keep working.

`gate-test.sh` is the quick check: one 64-byte CAN FD frame and one classic frame each way, compared complete, and a paced burst of 400 sequenced 64-byte frames checked byte for byte by `canfd-test seqrecv`. `tests/ctutest.py` holds 40 tests (`--list` prints them); the host side uses a raw SocketCAN socket or `candump -L` and checks complete frames and sequence numbers. The last full run, with every result line, is `docs/test-results.txt` (commit 5512fdb, from a clean clone with `QTARGET=scripts/qtarget-lite.sh`: gate test and 40 of 40 tests passed; every copied binary checked by md5 on the target). The README lists what the tests cover.

Not tested: two QNX nodes on one QEMU bus, hardware acceptance filters (not implemented), an unrelated device sharing the interrupt line (the line is not shared on tc; `pidin irq` shows only this driver on IRQ 10), and anything on silicon or ARM.

## 10a. ecu-sim

`tools/ecu-sim.c` is a small simulated battery-management ECU on QNX. A SCHED_FIFO thread (priority 30) sends one 64-byte CAN FD telemetry frame on id 0x18F every 10 ms with absolute deadlines (`clock_nanosleep(TIMER_ABSTIME)`): sequence number, time since start, pack voltage and current, state of charge and health, highest cell temperature, 16 cell voltages, its reopen count, and a CRC-8 (polynomial 0x1D) over the first 63 bytes. A period that is already over when the thread wakes is skipped and counted, so the sequence number counts periods. A second thread answers one diagnostic request: classic frame `7DF#02015B` (OBD-II service 01, PID 0x5B, battery pack remaining life, SAE J1979) gets `7E8#03415Bxx0000000000` with xx = state of charge * 255 / 100. Both threads reopen their device every millisecond after the driver goes away.

Measured on tc (`ctutest.py ecu_sim`, 30 s, `candump -L vcan0,18F:7FF,7E8:7FF` on the host, `docs/test-results.txt`): 3033 telemetry frames, CRC and sequence clean; inter-frame time mean 10.000 ms, standard deviation 0.050 ms, min 9.429, 1st percentile 9.874, 99th percentile 10.126, max 10.575 ms; ecu-sim reported 0 missed periods. The diagnostic request sent from a raw socket was answered 1.2 ms later (host clock). These are timestamps of QEMU's host bridge on vcan0: they show how the QNX side schedules and how QEMU forwards frames, not CAN bus timing, since QEMU 6.2 models no bit timing.

## 11. Demo storyline (under 3 minutes)

1. Problem (20 s): CAN FD software for QNX normally needs a CAN FD board on the desk. QEMU emulates one, but QNX had no driver for it.
2. Architecture (20 s): QEMU ctucan_pci, PCI server, dev-can-ctucanfd on libcan, `/dev/can1/*`, the QEMU bus bridged to host vcan0.
3. Live ECU (50 s): ecu-sim on QNX sends a 64-byte CAN FD status frame every 10 ms; the host runs `candump -t d vcan0,18F:7FF` to show the period, and `cansend vcan0 7DF#02015B` gets the answer on 7E8.
4. Fault and recovery (45 s): `slay -s KILL dev-can-ctucanfd` mid-stream. The kernel, ecu-sim and the shell carry on; the supervisor restarts the driver, ecu-sim reopens `/dev/can1/tx1`, candump shows the gap of a few milliseconds, and the period resumes.
5. Proof (30 s): `scripts/test-suite.sh` results, the fault-injection bus-off run, `canctl -i`, the repository.

## 12. Known limits

- Tested in QEMU 6.2 only. QEMU has no bit timing, error counters, fault confinement or bus timing; the error and bus-off paths are tested only through the fault injector, which shows what the driver does with those register sequences, not that silicon produces them that way (section 8).
- The driver compiles and links for aarch64le; it has not run on ARM, and register access ordering and posted-write completion on ARM have not been reviewed.
- One active core per board: both cores of QEMU's board drive one interrupt line, and each core raises or lowers it on its own, so two driver instances on one board can lose interrupts. A second node needs a second board.
- A shared interrupt line is handled in code (a pulse with nothing pending is counted as `spurious` and unmasked) but not tested: on tc only this driver is attached to IRQ 10 (`pidin irq`).
- Interrupt pulses are not authenticated. Any process allowed to open `/dev/can1/*` can send a pulse with the interrupt code; with nothing masked the kernel refuses the unmask and the driver counts it, but a pulse that arrives while a real interrupt is pending would make the driver unmask early and the real pulse's unmask then fails (counted). The device permissions are the only guard.
- Software acceptance filtering only; the core's hardware filters are not used.
- Per-frame FDF, BRS and ESI are not reported to clients (section 6).
- A `SET_TIMING` restart discards frames still in the RX FIFO (counted in `rx_flushed`).
- Devctl latency is measured, not bounded: a sustained interrupt load at a priority above the clients' can delay them (section 3.2).

## 13. Review findings and what was done

A second model (GPT-6 Astra) reviewed the stage-one code (`review/astra/answer_04_ctucanfd.md`) and v0.1 (`answer_06_ctucanfd_v01.md`). Each point was checked against the code and the target. Status: fixed (with the test that shows it), partly fixed, or open.

First review, correctness:

| Item | Status |
|---|---|
| 1. Unbounded service, mask accounting, synthetic pulses, queued-pulse race | Fixed: bounded batches, one checked `InterruptUnmask(0, iid)` per pulse, no synthetic pulses at all (section 3.2). Tests: `burst_batches`, `fi_batches`, `spoofed_irq_pulse`. With one attachment the kernel delivers one pulse per unmask; the remaining race, an event between the last read and the unmask, is closed by the output toggle (`rearm_pending`). |
| 2. Stranded readers | Fixed: a mailbox is returned while it has frames and blocked readers; left-over returns hold RBNEI for the next pulse. Tests: `readers`, `fi_reader_budget`. |
| 3. TX lifecycle | Fixed: writability checked, own completion metadata, unexpected EMPTY counted, bounded quiesce and abort, dropped frames counted. Tests: `fi_txdelay`, `fi_quiesce`, `fi_tx_budget`, `write_path`. |
| 4. Errors and bus-off | Fixed: STATUS.EWL tracked apart from FAULT_STATE, DOR cleared before DOI, bus-off hold until error active. Tests: `fi_warn`, `fi_passive`, `fi_busoff_auto`, `fi_busoff_manual`, `fi_busoff_latch`, `fi_busoff_stuck_abort`. Partly: the DOR/DOI order is in the code but no test overruns the FIFO. |
| 5. RX framing | Fixed: format word checked before anything else is consumed, FIFO flushed on any mismatch. Test: `fi_rx_corrupt`. |
| 6. Input semantics | Fixed: explicit id format (`-x`), RTR that would need CAN FD refused, payload sizes checked, SJW against both phase segments. Tests: `ids`, `extended_mailbox`, `rtr`, `devctl_sizes`, `timing`. |
| 7. Startup and shutdown | Fixed: BAR0 core count (`-c 2` refused, checked by hand, section 2.3), every device-creation result checked with unwinding, reset timeout fails start-up and restarts (`fi_reset_retry`), SIGTERM reaches cleanup (`sigterm`), bounded TX quiescence (`fi_quiesce`). Partly: the unwinding of a partial start-up is not exercised by a test. |
| ARM ordering before claiming ARM support | Open: no ARM runtime claim is made. |

First review, design and scope: the IST description is corrected (section 3.1); one active core per board and untested shared INTx are listed in section 12; fault injection, the supervisor, ecu-sim, four TXT buffers and the aarch64le build are done; two QNX nodes, hardware filters and an explicit IST are not done; `gate-test.sh` no longer hides copy or start failures and compares complete frames.

Second review (v0.1), ranked defects:

| Defect | Status |
|---|---|
| 1. RX: wrong in-range RWCNT consumed before validation | Fixed: count checked against DLC before consuming, flush on disagreement. Test: `fi_rx_corrupt` (format-word lookalike payloads, frames back to back). |
| 2. TX completions lost at budget exhaustion; several TX_STATUS reads | Fixed: TX retirement outside the RX budget, one snapshot per pass. Tests: `fi_tx_budget`, `fi_tx_order`. |
| 3. Continuation fallback strands readers; `cont_queued` can stick | Fixed by removing the continuation: left-over work keeps a status bit pending in the core. Tests: `fi_reader_budget`, `burst_batches`. |
| 4. Failed `InterruptDetach()` leaks an attachment; IRQ pulses not authenticated | Fixed for the leak by removing the second attachment. Open for authentication (section 12); `spoofed_irq_pulse` shows the refused unmask. |
| 5. Successful reset retry does not restore `hw_started` | Fixed: set only after a successful reset and start; teardown stops a mapped core regardless. Test: `fi_reset_retry`. |
| 6. Bus-off released by any non-bus-off value; unbounded abort | Fixed: hold until error active, aborts bounded at 20 ms. Tests: `fi_busoff_latch`, `fi_busoff_stuck_abort`. |
| 7. FI state leaks across OFF and reset; FI does not prove wire-level delay | Fixed: OFF and reset clear every overlay (`fi_state_clear`); section 8 says which tests show bookkeeping only. |
| 8. Supervisor trusts the pathname, waits forever, ignores spawn failures | Fixed: right server pid plus a bounded `GET_INFO`, kill and restart on timeout, spawn failures counted. Test: `supervisor_readiness`. Partly: a server that registers the path and never answers is bounded by the timer in code but not tested. |
| Normal build must refuse FI devctls | Fixed. Test: `fi_rejected_normal`. |
| Q3: detach-and-reuse pulse scheme | Removed (defect 3). |

Third review (v0.1b):

| Item | Status |
|---|---|
| Stopped-controller rearm: a pulse after a failed reset re-enabled the output | Fixed: output left off while the core is stopped, the pulse still unmasked. Test: `fi_reset_storm` (fails with the check removed). |
| Reader retry: a return that woke nobody was dropped and RBNEI released | Fixed: notification kept for up to 8 pulses, then a counted, logged drop of the queued frames. Test: `fi_reader_nowake`. |
| Abort deadline relied on traffic or the fault-injection tick | Fixed: bounded wait for the aborts at the point of bus-off. Test: `fi_busoff_abort_notick` (no ticker, no traffic). |
| Devctl starvation under sustained higher-priority interrupts | Open: documented as a measured latency, not a bound (sections 3.2, 12); sustained ingress beyond the tested loads not tested. |
| Supervisor probe could hang on a server that never replies | Fixed: probe in a forked child, abandoned at 200 ms. Test: `supervisor_hang`. |
| A logged copy retry could leave an old binary in place | Fixed: gate and suite compare the md5 on the target with the build after every copy. |
| Interrupt pulse authentication | Open, as before (section 12). |
