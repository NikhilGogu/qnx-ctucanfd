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
| A ready buffer with priority 0 is never sent | Driver sets TX_PRIORITY to 0x4321 |
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
| `src/ctucanfd_hw.[ch]` | One core: identify, reset, start/stop, write a TXT buffer, pop an RX frame, DLC tables, bit timing. No libcan or OS state |
| `src/dev-can-ctucanfd.c` | Options, PCI attach, libcan glue, interrupt service, devctls, shutdown |
| `tools/canfd-test.c` | Client: send/recv with the extended-message devctls (full 64-byte payload), info, pulse |
| `probe/ctucan_probe.c` | Identification probe |
| `scripts/qemu-can.sh`, `host-vcan.sh`, `gate-test.sh` | QEMU launch with CAN, host vcan0, end-to-end test |

### 2.3 Startup

1. `ThreadCtl(_NTO_TCTL_IO_LEVEL, _NTO_IO_LEVEL_1)`. Without I/O privileges the probe died with SIGSEGV during PCI access. The `ThreadCtl()` entry of the QNX 8.0 library reference describes level 1 as port I/O plus interrupt disable and enable for the calling thread, and recommends `_NTO_TCTL_IO_LEVEL` over the older `_NTO_TCTL_IO`. With it, `pci_device_attach()` and the BAR mappings work on tc (driver start log: `B0:D5:F0 BAR1 0xfeb90000 core 0 of 2, irq 10`).
2. `pci_device_find(idx, 0x1760, 0xff00)`, `pci_device_attach(..., pci_attachFlags_EXCLUSIVE_OWNER)`, `pci_device_read_ba()`, `pci_device_read_irq()` (gives 10), `mmap_device_memory(PROT_NOCACHE)` of the 16 KiB core window in BAR1.
3. Check DEVICE_ID 0xCAFD, then soft reset (MODE.RST) so a restarted driver never inherits TX buffers or interrupt state from a dead one.
4. Bit timing from the core clock (default 100 MHz), 500 kbit/s nominal at 87.5 % sample point and 2 Mbit/s data at 75 %: the driver prints `brp 4 tq 50` and `brp 1 tq 50`.
5. `can_resmgr_init()`, `can_resmgr_init_device()` per mailbox with `cflags |= CANDEV_CFLAG_CANFD`, `can_resmgr_create_device()`, `can_resmgr_attach_intr()`, then enable the core (SETTINGS.ENA, MODE.FDE) and its interrupts, then `can_resmgr_start()`.

## 3. Interrupts on QNX 8

The QNX 8.0 migration guide (`com.qnx.doc.qnxsdp.migration/topic/kernel.html`): "Interrupt Service Routines (ISRs) are no longer supported. Instead, QNX has a new mechanism for attaching threads to interrupts: Interrupt Service Threads (ISTs). If you are using events, there is currently no need for you to update your code". `InterruptAttach()` and `InterruptAttachArray()` are listed as removed; `InterruptAttachThread()` is new.

### 3.1 What runs where

The CAN DDK documents the interrupt contract: `can_resmgr_attach_intr()` "associates an IRQ and pulse code with the CAN pulse handler", which calls `event_handler()` when the interrupt occurs; "the interrupt doesn't fire again until it's unmasked"; once the interrupt is fully handled the handler "must call the InterruptUnmask() function and return NULL"; libcan calls `event_handler()` repeatedly until it returns NULL; for a returned TX device it calls `transmit()`, and for a returned RX device it "unblocks the client that was blocked the longest". So all device work runs on libcan's resource-manager thread, which receives the interrupt as a pulse. That thread is not an interrupt service thread. `pidin irq` on tc shows the one attachment: IRQ 10 (`0xa`) delivering a pulse with code 0 to a connection of the driver's process.

### 3.2 Bounded service

Each pulse gets a work budget of 64 units (one per frame moved from the RX FIFO, one per TXT buffer retired) and at most 8 passes over INT_STAT, and each RX mailbox return to libcan counts against a second budget of 64. An interrupt pulse ends with exactly one `InterruptUnmask(0, iid)`, and the result is checked (`unmask_err` in the driver's extended statistics). Work left over is not done in a loop: the driver disables the core's interrupt output (INT_ENA_CLR, which lowers the line because the output is INT_STAT AND INT_ENA; INT_MASK only stops new bits latching, datasheet 2.12 and QEMU's `ctucan_update_irq()`), unmasks the line, and sends itself a continuation pulse. The continuation runs the same bounded service and never unmasks anything. When the work is done the output is enabled again; an event that is still pending then raises the line afresh.

The continuation has its own pulse code. libcan passes a pulse to `event_handler()` only if its code was registered through `can_resmgr_attach_intr()` (stage one: a pulse with an unregistered code was dropped without calling the driver). The driver registers code 1 that way during start-up, with the line masked through the first attachment so the second one cannot fire, and then removes the second attachment's interrupt with `InterruptDetach()`. Afterwards `pidin irq` shows a single attachment and a client pulse with code 1 reaches `event_handler()` (`canfd-test pulse /dev/can1/rx0 1` raised `cont_pulses` from 0 to 1; code 2 was dropped). A process cannot open the pathnames it registered itself (the driver's own `open("/dev/can1/rx0")` failed with ENOENT for 2.4 s while other processes opened it), so the driver finds its channel with `ConnectServerInfo()` over its own connections, accepts it only if every connection to the process itself leads to the same channel, and attaches a side-channel connection for `MsgSendPulse()`. Without a continuation channel, deferred work resumes through the core's own interrupt instead.

A pulse sent with code 0 by a client would make the driver unmask an interrupt the kernel did not mask; `InterruptUnmask()` then fails and the driver counts it.

Measured on tc: 300 host frames of 64 bytes in four unpaced bursts of about 90 (above the budget) gave `hw_pulses 4 cont_pulses 3 cont_sent 3 unmask_err 0 deferred 3`, and the reader checked all 300 frames byte for byte (`seqrecv: got 300 of 300, bad 0, missing 0`).

### 3.3 Lost interrupt (stage one) and the output toggle

The stage-one driver stalled under a 1 ms paced burst: the controller held a full FIFO and a pending, enabled interrupt that never fired (`INT_STAT 0x00000409`, `RX_STATUS 0x00000660`). An event that arrived between the last INT_STAT read and the unmask left the line asserted, and no new interrupt came for a line that was already high. Stage one re-read INT_STAT after unmasking and looped. The driver now disables the core's interrupt output before unmasking and enables it afterwards, so a pending event always produces a fresh rising edge after the unmask, whatever the interrupt controller does with a line that is already asserted. `rearm_pending` counts the times an event was pending at that point.

### 3.4 Explicit IST (not in v0.1)

QNX recommends explicit interrupt threads (`InterruptAttachThread()`) "to increase efficiency and control". An explicit IST could drain the RX FIFO into a ring at a priority above client threads, so RX latency would not depend on how busy libcan's thread is. v0.1 does not do this: nothing measured so far shows dispatch contention, and the bounded service above already keeps one interrupt from monopolising that thread.

## 4. RX path

The RX FIFO holds frames as a frame format word (DLC, RTR, IDE, FDF, BRS, ESI, RWCNT), an identifier word, two timestamp words and up to 16 data words. The driver always pops RWCNT words so the FIFO stays aligned.

The identifier word uses the same layout as a libcan MID (11-bit id in bits 28:18, 29-bit id as is), so IDs pass through unchanged and `ext.is_extended_mid` comes from IDE.

Each RX mailbox has a MID and a filter; a frame goes to every mailbox where `((mid ^ mbox.mid) & mbox.mfilter) == 0`. The default filter 0 accepts everything. When a mailbox queue is full the oldest frame is dropped (`sw_receive_q_full`). A FIFO overrun is counted in `hw_receive_overflows` and cleared.

QEMU leaves the timestamp words at zero, so `ext.timestamp` is CLOCK_MONOTONIC in microseconds, adjustable with `CAN_DEVCTL_SET_TIMESTAMP`. On silicon the core timestamp can replace it.

Planned: map `CAN_DEVCTL_SET_MFILTER` onto the core's hardware filters (FILTER_A/B/C and the range filter) when one RX mailbox is configured.

## 5. TX path

libcan queues a frame and calls `transmit()`. The driver takes frames round robin across TX mailboxes and keeps up to four in the core's TXT buffers (`-T 1..4`, default 4). A frame goes into the buffer at the head of a ring only if TX_STATUS shows that buffer writable (Empty, TX OK, TX failed, Aborted; datasheet 2.9). The driver copies what it needs (sequence, id, length, format) into its own slot for that buffer and returns libcan's queue element at once. All buffers filled in one pass are set ready with one TX_COMMAND.

Order: the core sends the ready buffer with the highest priority first. The oldest buffer in flight gets priority 7 and each later one one less; when the oldest finishes, the priorities rotate so the next oldest gets 7 (the scheme Linux's ctucanfd driver uses). QEMU 6.2 never sends a buffer with priority 0, so the priorities run 7 down to 4.

Completion: on TXBHCI the driver retires buffers strictly from the oldest: TX OK counts as transmitted, TX failed as a frame error, Aborted as aborted, and Empty while a frame was in flight as lost (`tx_lost`). A later buffer that finishes while an older one is still pending counts as an ordering fault (`tx_order_err`). A plain `write()` has `mid == CAN_MSG_MID_UNKNOWN`; the mailbox then supplies the id and its format (11-bit, or 29-bit with `-x`), and the frame is a data frame whatever the reused queue element held before.

Restart and shutdown: before a `CAN_DEVCTL_SET_TIMING` restart or a shutdown, TX admission stops and the driver waits up to 20 ms for the TXT buffers to finish, then aborts the rest and waits up to 20 ms again. Aborted frames are dropped and counted (`tx_aborted`); frames still in libcan's queues are kept and sent after a restart.

Tests (`scripts/tests/ctutest.py`, results in section 10):

- `tx_order`: one writer with no pacing, 2000 frames: all 2000 reached vcan0 in order. In QEMU a TXT buffer completes inside the TX_COMMAND write, so the driver rarely has several frames ready at once (max in flight 2 in that run).
- `fi_tx_order`: the fault-injection build holds each completion 5 ms and releases at most three per millisecond, oldest first, so the driver refills several buffers in one command at changing ring positions: 1000/1000 frames in order across 249 multi-buffer TX commands, max 4 in flight, `tx_order_err 0`.
- `fi_tx_order_fixed`: the same run with the stage-one fixed priorities (buffer n gets n + 1) as a negative control: 747 order breaks in 1000 frames. The ordering test does detect a wrong priority scheme.

## 6. CAN FD

The framework carries CAN FD: `CAN_MSG.dat[CAN_MSG_DATA_MAX]` with `CAN_MSG_DATA_MAX_CANFD 64`, and `CANDEV.cflags` bit 31 (`CANDEV_CFLAG_CANFD`). `CAN_MSG` has no per-frame FDF, BRS or ESI flags, so the frame format is a driver policy:

- CAN FD is on by default (`-C` for classic only). Frames longer than 8 bytes go out as CAN FD with BRS (`-S` turns BRS off; `-A` sends every frame as CAN FD).
- Lengths are rounded up to the next CAN FD size (12, 16, 20, 24, 32, 48, 64) and zero padded: a 10-byte frame from QNX reached the host as `124  [12]  00 01 02 03 04 05 06 07 08 09 00 00`, a 33-byte frame as `125  [48]` with 15 zero bytes.
- The driver rejects `CAN_DEVCTL_WRITE_CANMSG_EXT` above 64 bytes (8 in classic mode), because libcan itself does not check the length.
- RX accepts classic and CAN FD frames alike. Because the API cannot report FDF/BRS/ESI to clients, an opt-in use of `CAN_MSG.rsv[0]` would need QNX's agreement first; it is not implemented.

QEMU's host bridge enables CAN FD only when the host interface MTU is 72 (`CANFD_MTU`), so `host-vcan.sh` sets `mtu 72`.

## 7. Devctl and canctl

Handled by the driver: `GET/SET_MID`, `GET/SET_MFILTER`, `GET/SET_TIMESTAMP`, `GET_INFO`, `GET_STATS`, `ERROR` (FAULT_STATE word, REC/TEC, ERR_NORM/ERR_FD, STATUS), `SET_TIMING` (nominal, restarts the controller), `DEBUG_INFO` (register dump to slog2). The stock `canctl` works against it: `canctl -u1,tx1 -i` printed queue sizes, `Bitrate: 500000 Baud`, prescaler 4, TS1 43, TS2 6. canctl's `-w` takes a raw MID, so a standard id must be shifted left by 18 (`-w 0x123,0,hello` went out as id 0x000).

## 8. Errors and bus-off

Enabled interrupts: EWLI, FCSI, BEI, DOI (plus RBNEI and TXBHCI).

- BEI: ERR_CAPT error type goes to the matching stats counter (bit, CRC, form, ACK, stuff).
- FCSI and EWLI: FAULT_STATE (ERA, ERP, BOF) is compared with the last state; error passive and bus-off increment their counters, bus-off logs TEC and REC to slog2.
- Bus-off recovery: with `-a` the driver issues COMMAND.ERCRST, so the core rejoins after 128 times 11 recessive bits. Without it the controller stays off until `CAN_DEVCTL_SET_TIMING` or a driver restart, so a system integrator can choose.
- A TXT buffer in the error state (retransmit limit) is retired and counted as a frame error.

None of this can be triggered in QEMU 6.2. Test plan: a fault-injection build of the hardware layer that replays FCSI/BEI sequences into the service loop, and a check on real CTU CAN FD silicon if we can borrow a board.

## 9. Restart and recovery

SIGTERM makes `can_resmgr_start()` return; the driver disables the core and its interrupts, detaches, destroys the devices and calls `can_resmgr_fini()`. After `slay -s KILL dev-can-ctucanfd` the kernel removed `/dev/can1`; a fresh start reset the core and the next 64-byte frame reached vcan0. From the shell, `time` gave `0.01s real` for the driver start until the shell got control back, and `ls /dev/can1` right after showed `rx0` and `tx1`.

Planned: a supervisor (a respawn loop in the image, or HAM if it is in the target image) and clients that reopen on error.

## 10. Testing in QEMU

```
scripts/host-vcan.sh vcan0                 # vcan module, mtu 72, link up
qtarget tc create; qtarget tc stop         # any mkqnximage target
scripts/qemu-can.sh ~/targets/tc vcan0     # same QEMU args + CAN bus + ctucan_pci + bridge
source ~/qnx800/qnxsdp-env.sh && make
scripts/gate-test.sh tc vcan0
```

`qemu-can.sh` takes the exact `QEMU Command:` line mkqnximage logged, swaps `-nographic` for `-display none -daemonize` (QEMU refuses the pair), adds a monitor socket and appends `-object can-bus,id=canbus0-bus -object can-host-socketcan,id=canhost0,if=vcan0,canbus=canbus0-bus -device ctucan_pci,canbus0=canbus0-bus,canbus1=canbus0-bus`. The pidfile stays `output/qemu.pid`, so `qtarget status|ssh|stop` keep working.

Gate run (`gate-test.sh`, QNX 8.0 x86_64 under KVM):

```
== QNX -> host (vcan0)
  vcan0  TX B -  123  [64]  00 01 02 03 ... 3E 3F
  vcan0  TX - -  7DF   [8]  02 01 05 00 00 00 00 00
== host -> QNX (/dev/can1/rx0)
/dev/can1/rx0  456  [64]  C0 C1 C2 ... FE FF
/dev/can1/rx0  1ABCDEF0  [04]  DE AD BE EF
== paced burst: 400 x 64-byte CAN FD frames, 1 ms apart
stats: tx 2 rx 402 irq 403 (rx 401 tx 2) frame_err 0 hw_overflow 0 swq_full 0 bus_off 0
PASS: 64-byte CAN FD both ways, classic and 29-bit ids, 400/400 burst frames
```

`B` in candump's `-x` column is the bit rate switch flag. An unpaced 500-frame burst (`cangen -g 0`) overflowed the 102-frame FIFO as expected (`hw_overflow 2`), and the next frame after it was still received, so overrun recovery works.

Planned tests: every length 0 to 64 both ways (DLC rounding), RTR, standard and extended ids, several mailboxes with filters, queue-full behaviour, restart under load, two QNX nodes on one QEMU bus (two boards), and aarch64le builds.

## 11. Judge demo storyline (under 3 minutes)

1. Problem (20 s): CAN FD software for QNX normally needs a CAN FD board on the desk. QEMU already emulates one, but QNX had no driver for it.
2. Architecture (30 s): one diagram. QEMU ctucan_pci, PCI server, dev-can-ctucanfd on libcan, `/dev/can1/*`, the QEMU bus bridged to host vcan0.
3. Live ECU (60 s): a simulated battery management ECU on QNX (to be written: `ecu-sim`) sends a 64-byte CAN FD status frame every 10 ms and answers a diagnostic request. The host runs `candump -t d vcan0` to show the 10 ms period, and `cansend` plays the tester.
4. Fault and recovery (40 s): `slay -s KILL dev-can-ctucanfd` mid-stream. The kernel, the ECU process and the shell survive; the supervisor restarts the driver, the ECU reopens `/dev/can1/tx1` and candump shows the gap, then the period resumes. On a microkernel the CAN driver is just a process that can crash and come back.
5. Proof (30 s): `gate-test.sh` PASS, the burst statistics, `canctl -i`, the repository.

## 12. Known limits and next steps

- One TXT buffer in flight; four with rotating priorities next.
- Software acceptance filtering only; hardware filters next.
- Error and bus-off paths are written but cannot be exercised in QEMU 6.2.
- Explicit IST (section 3.3) and a watchdog pulse are designed, half validated.
- To write: `ecu-sim`, supervisor, a broader test suite, README with build and run steps, the demo video.
