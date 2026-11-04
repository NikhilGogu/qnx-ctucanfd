#!/usr/bin/env python3
# Copyright 2026 The ctucanfd authors
# SPDX-License-Identifier: Apache-2.0
"""End-to-end tests for dev-can-ctucanfd.

Runs on the build box against a QEMU target started with scripts/qemu-can.sh
(ctucan_pci bridged to a host vcan interface). The host side uses a raw
SocketCAN socket, so every frame is checked in full: identifier, format,
length, every payload byte including padding, and sequence numbers.

    scripts/tests/ctutest.py [--target tc] [--peer tc2] [--if vcan0] [--list] [TEST ...]

Tests whose name starts with fi_ run against the fault-injection build
(make FI=1). two_nodes needs a second target on the same host interface
(--peer) and is skipped without one. Exit status 0 only if no selected test
failed.
"""
import argparse
import hashlib
import os
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
QTARGET = os.environ.get("QTARGET", os.path.expanduser("~/bin/qtarget"))
EFF, RTR = 0x80000000, 0x40000000
BRS = 0x01
FD_LENS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]
TEST = "/tmp/canfd-test"
RX0, TX1 = "/dev/can1/rx0", "/dev/can1/tx1"


def fd_len(n):
    return next(x for x in FD_LENS if x >= n)


def pattern(seq, n):
    """The sequenced payload canfd-test seqsend/seqrecv use."""
    return bytes(((seq >> (8 * i)) & 0xFF) if i < 4 else ((seq + i) & 0xFF) for i in range(n))


def seq_of(data):
    return int.from_bytes(data[:4], "little")


class Fail(Exception):
    pass


class Skip(Exception):
    pass


PEER = None     # second target for two_nodes (--peer)


def check(cond, msg):
    if not cond:
        raise Fail(msg)


class Target:
    def __init__(self, name):
        self.name = name
        self.variant = None
        self.args = None

    def ssh(self, cmd, timeout=180):
        p = subprocess.run([QTARGET, self.name, "ssh", cmd], capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr

    def out(self, cmd, timeout=180):
        return self.ssh(cmd, timeout)[1]

    deployed = {}       # remote path -> md5 verified on the target after the copy

    def put(self, local, remote):
        """Copies and then checks the md5 on the target: a failed or partial copy never leaves an old binary."""
        want = hashlib.md5(open(local, "rb").read()).hexdigest()
        err = ""
        for _ in range(12):
            p = subprocess.run([QTARGET, self.name, "put", local, remote], capture_output=True, text=True)
            if p.returncode == 0:
                got = self.out(f"md5sum {remote}").split()
                if got and got[0] == want:
                    Target.deployed[f"{self.name}:{os.path.basename(remote)}"] = want
                    return
                err = f"md5 on the target {got[:1]} differs from {want}"
            else:
                err = p.stderr.strip()
            time.sleep(0.5)
        raise Fail(f"put {local}: {err}")

    def bg(self, cmd, log):
        self.ssh(f"rm -f {log}; ({cmd} >{log} 2>&1 &)")

    def driver(self, args="", variant="x86_64", force=False):
        """(Re)starts the driver unless it already runs with these arguments."""
        if not force and self.variant == variant and self.args == args and self.alive():
            return
        self.stop_all()
        build = os.path.join(ROOT, "build", variant)
        for f in ("dev-can-ctucanfd", "canfd-test"):
            self.put(os.path.join(build, f), f"/tmp/{f}")
        rc, out = self.ssh(f"/tmp/dev-can-ctucanfd {args} >/tmp/drv.log 2>&1 </dev/null; echo rc=$?; "
                           "cat /tmp/drv.log; for i in 1 2 3 4 5 6 7 8 9 10; do "
                           "ls /dev/can1 2>/dev/null | grep -q tx && break; sleep 0.1; done; ls /dev/can1")
        check("rc=0" in out and "CTU CAN FD" in out and re.search(r"^tx\d+$", out, re.M),
              f"driver start failed: {out.strip()}")
        self.variant, self.args = variant, args

    def stop_all(self):
        # the supervisor first, or it would start the driver again
        self.ssh("slay -f -s KILL ctucan-supervisor >/dev/null 2>&1; "
                 "slay -f -s KILL dev-can-ctucanfd canfd-test ecu-sim >/dev/null 2>&1; true")
        self.variant = None

    def alive(self):
        return "dev-can-ctucanfd" in self.out("pidin -p dev-can-ctucanfd -f a 2>/dev/null")

    def xstats(self, dev=RX0):
        out = self.out(f"{TEST} xstats {dev}")
        vals = dict((k, int(v, 0)) for k, v in re.findall(r"(\w+) (0x[0-9a-f]+|\d+)", out))
        check("hw_pulses" in vals, f"xstats failed: {out.strip()}")
        return vals

    def stats(self, dev=RX0):
        out = self.out(f"{TEST} info {dev}")
        m = re.search(r"^stats: (.*)$", out, re.M)
        check(m is not None, f"info failed: {out.strip()}")
        s = m.group(1)
        vals = dict((k, int(v)) for k, v in re.findall(r"(\w+) (\d+)", s))
        vals["tx"] = int(re.search(r"tx (\d+)", s).group(1))
        vals["rx"] = int(re.search(r"rx (\d+)", s).group(1))
        return vals


LINE = re.compile(r"^(\S+)  ([0-9A-F]+)  \[(\d+)\] ((?: [0-9A-F]{2})*)( RTR)?  ts=\d+$")


def parse_recv(text):
    """Frames printed by canfd-test recv."""
    frames = []
    for ln in text.splitlines():
        m = LINE.match(ln)
        if m:
            frames.append(dict(id=int(m.group(2), 16), ext=len(m.group(2)) == 8, len=int(m.group(3)),
                               data=bytes.fromhex(m.group(4).replace(" ", "")), rtr=m.group(5) is not None))
    return frames


class Bus:
    def __init__(self, ifname):
        s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        s.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        s.bind((ifname,))
        self.s, self.ifname = s, ifname

    def drain(self):
        while select.select([self.s], [], [], 0)[0]:
            self.s.recv(80)

    def send(self, can_id, data=b"", ext=False, fd=None, brs=True, rtr=False, dlc=None):
        cid = can_id | (EFF if ext else 0) | (RTR if rtr else 0)
        if fd is None:
            fd = len(data) > 8
        if fd:
            self.s.send(struct.pack("=IBBBB64s", cid, fd_len(len(data)), BRS if brs else 0, 0, 0, data))
        else:
            self.s.send(struct.pack("=IB3x8s", cid, dlc if dlc is not None else len(data), data))

    def recv(self, n, timeout, ids=None):
        """Up to n frames (optionally only these ids) within timeout seconds."""
        out, end = [], time.monotonic() + timeout
        while len(out) < n:
            left = end - time.monotonic()
            if left <= 0 or not select.select([self.s], [], [], left)[0]:
                break
            b = self.s.recv(80)
            t = time.monotonic()
            cid = struct.unpack_from("=I", b)[0]
            if len(b) == 72:
                _, ln, fl, _, _, d = struct.unpack("=IBBBB64s", b)
                fd = True
            else:
                _, ln, d = struct.unpack("=IB3x8s", b)
                fl, fd = 0, False
            f = dict(id=cid & (0x1FFFFFFF if cid & EFF else 0x7FF), ext=bool(cid & EFF), rtr=bool(cid & RTR),
                     fd=fd, brs=bool(fl & BRS), len=ln, data=d[:ln], t=t)
            if ids is None or f["id"] in ids:
                out.append(f)
        return out


class Capture(threading.Thread):
    """Receives on the host while the target sends, so the socket buffer never overflows."""

    def __init__(self, bus, n, timeout, ids=None):
        super().__init__()
        self.args, self.frames = (bus, n, timeout, ids), []
        self.start()

    def run(self):
        bus, n, timeout, ids = self.args
        self.frames = bus.recv(n, timeout, ids)

    def wait(self):
        self.join()
        return self.frames


def check_seq_stream(frames, count, length, what):
    seqs = [seq_of(f["data"]) for f in frames]
    check(len(frames) == count, f"{what}: {len(frames)} of {count} frames")
    check(seqs == list(range(count)), f"{what}: sequence broken near {next((i for i, s in enumerate(seqs) if s != i), -1)}")
    bad = [f for f in frames if f["data"][:length] != pattern(seq_of(f["data"]), length)]
    check(not bad, f"{what}: {len(bad)} payload(s) wrong")


# ---- tests on the normal build -------------------------------------------

def t_tx_lengths(t, bus):
    """QNX -> host, every length 0..64: DLC rounding, zero padding, FD and BRS flags."""
    t.driver()
    bus.drain()
    rc, out = t.ssh(f"for L in $(seq 0 64); do {TEST} send {TX1} 3A0 $L >/dev/null || echo FAIL $L; done")
    fr = bus.recv(65, 10, ids={0x3A0})
    check("FAIL" not in out, f"send failed: {out.strip()}")
    check(len(fr) == 65, f"{len(fr)} of 65 frames on the host")
    for n, f in enumerate(fr):
        want = fd_len(n) if n > 8 else n
        check(f["len"] == want, f"length {n}: host saw [{f['len']}], want [{want}]")
        check(f["fd"] == (n > 8) and f["brs"] == (n > 8), f"length {n}: fd {f['fd']} brs {f['brs']}")
        check(f["data"] == bytes(range(n)) + bytes(want - n), f"length {n}: payload or padding wrong")
    return "65 lengths, padding and FD/BRS flags exact"


def t_rx_lengths(t, bus):
    """host -> QNX, classic 0..8 and every CAN FD length: payload checked byte for byte."""
    t.driver()
    sent = [(0x3B0, bytes((i * 7 + n) & 0xFF for i in range(n)), False) for n in range(9)]
    sent += [(0x3B1, bytes((i * 5 + n) & 0xFF for i in range(n)), True) for n in FD_LENS]
    t.bg(f"{TEST} recv {RX0} {len(sent)}", "/tmp/rxl.log")
    time.sleep(0.5)
    for cid, data, fd in sent:
        bus.send(cid, data, fd=fd)
        time.sleep(0.002)
    time.sleep(1)
    got = parse_recv(t.out("cat /tmp/rxl.log"))
    check(len(got) == len(sent), f"{len(got)} of {len(sent)} frames on QNX")
    for (cid, data, fd), g in zip(sent, got):
        check(g["id"] == cid and g["len"] == len(data) and g["data"] == data,
              f"id {cid:X} len {len(data)}: got id {g['id']:X} [{g['len']}] {g['data'].hex()}")
    return f"{len(sent)} frames (classic 0..8, FD {FD_LENS[0]}..{FD_LENS[-1]}) exact"


def t_rtr(t, bus):
    """Remote frames both ways; an RTR that would need CAN FD is refused."""
    t.driver()
    t.bg(f"{TEST} recv {RX0} 3", "/tmp/rtr.log")
    time.sleep(0.5)
    bus.send(0x321, rtr=True, dlc=0)
    bus.send(0x321, rtr=True, dlc=4)
    bus.send(0x12345678, ext=True, rtr=True, dlc=8)
    time.sleep(0.5)
    got = parse_recv(t.out("cat /tmp/rtr.log"))
    want = [(0x321, False, 0), (0x321, False, 4), (0x12345678, True, 8)]
    check([(g["id"], g["ext"], g["len"]) for g in got] == want and all(g["rtr"] for g in got),
          f"QNX received {got}")
    bus.drain()
    rc, out = t.ssh(f"{TEST} send {TX1} 321r 4")
    fr = bus.recv(1, 3, ids={0x321})
    check(rc == 0 and len(fr) == 1 and fr[0]["rtr"] and fr[0]["len"] == 4 and not fr[0]["fd"],
          f"host saw {fr} ({out.strip()})")
    rc, out = t.ssh(f"{TEST} send {TX1} 321r 12")
    check(rc != 0 and "Invalid argument" in out, f"RTR with 12 bytes was not refused: {out.strip()}")
    return "RTR dlc 0/4/8 to QNX, dlc 4 to host, RTR len 12 refused (EINVAL)"


def t_ids(t, bus):
    """Standard and extended ids both ways, including extended ids whose low 18 bits are zero."""
    t.driver()
    ids = [(0x000, False), (0x7FF, False), (0x00000000, True), (0x04000000, True), (0x1FFFFFFF, True)]
    t.bg(f"{TEST} recv {RX0} {len(ids)}", "/tmp/ids.log")
    time.sleep(0.5)
    for cid, ext in ids:
        bus.send(cid, b"\x5a\xa5", ext=ext)
    time.sleep(0.5)
    got = [(g["id"], g["ext"]) for g in parse_recv(t.out("cat /tmp/ids.log"))]
    check(got == ids, f"QNX received {got}")
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 000 1 && {TEST} send {TX1} 7FF 1 && {TEST} send {TX1} 0x 1 && "
          f"{TEST} send {TX1} 4000000x 1 && {TEST} send {TX1} 1FFFFFFFx 1")
    fr = [(f["id"], f["ext"]) for f in bus.recv(5, 3)]
    check(fr == ids, f"host received {fr}")
    return "11-bit 0x000/0x7FF and 29-bit 0x0/0x4000000/0x1FFFFFFF exact both ways"


def t_write_path(t, bus):
    """Plain write() after a remote-frame devctl: a data frame with the mailbox id."""
    t.driver()
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 321r 4 >/dev/null; {TEST} write {TX1} A1A2A3")
    fr = bus.recv(2, 3)
    check(len(fr) == 2 and fr[1]["id"] == 0x100 and not fr[1]["rtr"] and fr[1]["data"] == b"\xa1\xa2\xa3",
          f"host saw {fr}")
    return "write() -> 100 [3] A1 A2 A3, data frame"


def t_extended_mailbox(t, bus):
    """-x: plain write() uses a 29-bit id, never inferred from its bits."""
    t.driver("-x")
    bus.drain()
    t.ssh(f"{TEST} write {TX1} 0102")
    fr = bus.recv(1, 3)
    t.driver()
    check(len(fr) == 1 and fr[0]["ext"] and fr[0]["id"] == 0x100, f"host saw {fr}")
    return "-x write() -> 00000100 extended"


def t_readers(t, bus):
    """Four blocked readers on one mailbox: every frame reaches exactly one, all four finish."""
    t.driver()
    for k in range(4):
        t.bg(f"{TEST} recv {RX0} 50", f"/tmp/rd{k}.log")
    time.sleep(0.8)
    x0 = t.xstats()
    for q in range(200):
        bus.send(0x2A2, pattern(q, 8))
        time.sleep(0.002)
    time.sleep(1)
    seqs, counts = [], []
    for k in range(4):
        got = parse_recv(t.out(f"cat /tmp/rd{k}.log"))
        counts.append(len(got))
        seqs += [seq_of(g["data"]) for g in got if g["data"] == pattern(seq_of(g["data"]), 8)]
    left = t.out("pidin -p canfd-test -f a 2>/dev/null | grep -c canfd-test").strip()
    check(counts == [50] * 4, f"reader counts {counts}")
    check(sorted(seqs) == list(range(200)), f"{len(set(seqs))} distinct of 200 frames")
    check(left == "0", f"{left} reader(s) still blocked")
    return f"4 readers x 50, 200 distinct frames, reports {t.xstats()['reports'] - x0['reports']}"


def t_writers(t, bus):
    """Four writers at once on one TX mailbox: each stream complete and in order."""
    t.driver()
    bus.drain()
    cap = Capture(bus, 400, 15, ids={0x2C0, 0x2C1, 0x2C2, 0x2C3})
    cmd = " & ".join(f"{TEST} seqsend {TX1} 2C{k} 16 100 500 >/tmp/wr{k}.log 2>&1" for k in range(4))
    t.ssh(cmd + "; wait")
    fr = cap.wait()
    for k in range(4):
        check_seq_stream([f for f in fr if f["id"] == 0x2C0 + k], 100, 16, f"writer {k}")
    return "4 writers x 100 frames, every stream in order"


def t_rx_overflow(t, bus):
    """No reader: the 64-frame mailbox queue keeps the newest frames and counts the drops."""
    t.driver()
    s0 = t.stats()
    for q in range(100):
        bus.send(0x2A3, pattern(q, 8))
        time.sleep(0.001)
    time.sleep(0.5)
    got = parse_recv(t.out(f"{TEST} recv {RX0} 64"))
    s1 = t.stats()
    seqs = [seq_of(g["data"]) for g in got]
    check(seqs == list(range(36, 100)), f"queue held {seqs[:3]}..{seqs[-3:]} ({len(seqs)})")
    check(s1["swq_full"] - s0["swq_full"] == 36, f"swq_full +{s1['swq_full'] - s0['swq_full']}")
    return "kept seq 36..99, swq_full +36"


def t_bidir(t, bus):
    """Sustained traffic both ways: 2000 frames of 64 bytes each way, 2 ms apart."""
    t.driver()
    bus.drain()
    t.bg(f"{TEST} seqrecv {RX0} 2000 30000", "/tmp/bd.log")
    time.sleep(0.5)
    got = []
    rx = threading.Thread(target=lambda: got.extend(bus.recv(2000, 25, ids={0x2B1})))
    rx.start()
    tx = threading.Thread(target=lambda: t.ssh(f"{TEST} seqsend {TX1} 2B1 64 2000 2000 >/tmp/bs.log 2>&1"))
    tx.start()
    nxt = time.monotonic()
    for q in range(2000):
        bus.send(0x2A1, pattern(q, 64))
        nxt += 0.002
        time.sleep(max(0, nxt - time.monotonic()))
    tx.join()
    rx.join()
    time.sleep(0.5)
    log = t.out("cat /tmp/bd.log")
    check("got 2000 of 2000, bad 0, missing 0, out_of_order 0" in log, f"QNX side: {log.strip()}")
    check_seq_stream(got, 2000, 64, "host side")
    return "2000 + 2000 frames, both streams complete and in order"


def t_devctl_sizes(t, bus):
    """devctls with payloads shorter than their command are refused."""
    t.driver()
    rc, out = t.ssh(f"{TEST} badsize {TX1}")
    check(rc == 0, out.strip())
    return " / ".join(out.strip().splitlines())


def t_timing(t, bus):
    """SET_TIMING: SJW above a phase segment refused; a valid change restarts and traffic continues."""
    t.driver(force=True)
    out_bad = t.out(f"{TEST} timing {RX0} 4 43 6 7")
    out_ok = t.out(f"{TEST} timing {RX0} 4 43 6 4")
    info = t.out(f"{TEST} info {RX0}")
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 3C0 12")
    fr = bus.recv(1, 3, ids={0x3C0})
    x = t.xstats()
    check("Invalid argument" in out_bad, f"sjw 7 > ph2 6 accepted: {out_bad.strip()}")
    check("No error" in out_ok and "sjw 4" in info and "bit_rate: 500000" in info, f"{out_ok.strip()} / {info}")
    check(len(fr) == 1 and x["restarts"] == 1, f"after restart: host {fr}, restarts {x['restarts']}")
    return "sjw 7 > ph2 refused; brp 4 ts1 43 ts2 6 sjw 4 accepted, 1 restart, TX works after"


def t_spoofed_irq_pulse(t, bus):
    """A client pulse with the interrupt code is not a mask obligation the driver can meet."""
    t.driver()
    x0 = t.xstats()
    t.ssh(f"{TEST} pulse {RX0} 0")
    x1 = t.xstats()
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 3C1 8")
    fr = bus.recv(1, 3, ids={0x3C1})
    check(x1["unmask_err"] == x0["unmask_err"] + 1, f"unmask_err {x0['unmask_err']} -> {x1['unmask_err']}")
    check(len(fr) == 1, "TX failed after the spoofed pulse")
    return "InterruptUnmask refused (unmask_err +1), driver keeps working"


def t_sigterm(t, bus):
    """SIGTERM reaches the cleanup path: quiesce, core disabled, paths removed."""
    t.driver()
    t.ssh("slay dev-can-ctucanfd")
    time.sleep(1)
    log = t.out("slog2info 2>/dev/null | grep dev-can-ctucanfd | tail -3; ls /dev/can1 2>&1")
    t.variant = None
    check("stopped: tx" in log and "No such file" in log, f"no clean stop: {log.strip()}")
    return re.search(r"stopped: .*", log).group(0)


def order_errors(frames):
    seqs = [seq_of(f["data"]) for f in frames]
    return sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)


def t_tx_order(t, bus):
    """A writer with no pacing through four TXT buffers: frames leave complete and in FIFO order."""
    t.driver(force=True)
    bus.drain()
    cap = Capture(bus, 2000, 20, ids={0x2F0})
    t.ssh(f"{TEST} seqsend {TX1} 2F0 8 2000 0 >/tmp/ord.log 2>&1")
    got = cap.wait()
    x = t.xstats()
    check_seq_stream(got, 2000, 8, "host side")
    check(x["tx_order_err"] == 0, f"{x}")
    return (f"2000/2000 in order; max in flight {x['tx_max_inflight']}, "
            f"{x['tx_batches']} multi-buffer TX commands")


CDLINE = re.compile(r"^\((\d+\.\d+)\) \S+ ([0-9A-F]+)(?:##([0-9A-F])|#)([0-9A-F]*)")


class Candump:
    """candump -L on the host; frames come back with candump's own timestamps."""

    def __init__(self, ifname, filt):
        self.path = f"/tmp/ctutest-candump-{os.getpid()}.log"
        self.f = open(self.path, "w")
        self.p = subprocess.Popen(["candump", "-L", f"{ifname},{filt}"], stdout=self.f, stderr=subprocess.STDOUT)
        time.sleep(0.3)

    def stop(self):
        self.p.terminate()
        self.p.wait()
        self.f.close()
        frames = []
        for ln in open(self.path):
            m = CDLINE.match(ln)
            if m:
                frames.append(dict(t=float(m.group(1)), id=int(m.group(2), 16), fdflags=m.group(3),
                                   data=bytes.fromhex(m.group(4))))
        os.unlink(self.path)
        return frames


def deploy_tools(t):
    t.stop_all()
    for f in ("dev-can-ctucanfd", "canfd-test", "ecu-sim", "ctucan-supervisor", "ctucan-hangsrv"):
        t.put(os.path.join(ROOT, "build", "x86_64", f), f"/tmp/{f}")


def wait_path(t, path):
    out = t.out(f"for i in $(seq 1 50); do [ -e {path} ] && echo up && break; sleep 0.1; done")
    check("up" in out, f"{path} did not appear")


def t_restart_under_load(t, bus, kills=3):
    """slay -s KILL under load: the supervisor restarts the driver, ecu-sim reopens; gap from candump."""
    deploy_tools(t)
    t.bg("/tmp/ctucan-supervisor -- /tmp/dev-can-ctucanfd", "/tmp/sup.log")
    wait_path(t, TX1)
    t.bg("/tmp/ecu-sim -p 1000 -i 2F8", "/tmp/ecu.log")
    cd = Candump(bus_if(bus), "2F8:7FF")
    load = subprocess.Popen(["cangen", bus_if(bus), "-I", "2A6", "-L", "64", "-f", "-b", "-g", "2", "-D", "i"])
    time.sleep(2)
    for _ in range(kills):
        t.ssh("slay -s KILL dev-can-ctucanfd")
        time.sleep(2)
    load.terminate()
    load.wait()
    frames = cd.stop()
    t.ssh("slay -s TERM ecu-sim; sleep 0.3")
    sup, ecu = t.out("cat /tmp/sup.log"), t.out("cat /tmp/ecu.log")
    t.stop_all()
    seqs = [seq_of(f["data"]) for f in frames]
    holes = [(b["t"] - a["t"], sa, sb) for a, b, sa, sb in zip(frames, frames[1:], seqs, seqs[1:])
             if b["t"] - a["t"] > 0.005]
    check(len(holes) == kills, f"{len(holes)} gaps over 5 ms for {kills} kills: {holes[:6]}")
    check(all(b > a for a, b in zip(seqs, seqs[1:])), "sequence went backwards")
    ready = [float(x) for x in re.findall(r"ready \([^)]*\) ([\d.]+) ms after spawn", sup)]
    check(sup.count("died") == kills and len(ready) == kills + 1, f"supervisor log:\n{sup}")
    reopen = [float(x) for x in re.findall(r"telemetry reopened \S+ after ([\d.]+) ms", ecu)]
    gaps_ms = ", ".join(f"{g * 1000:.1f}" for g, _, _ in holes)
    lost = ", ".join(str(b - a - 1) for _, a, b in holes)
    return (f"{kills} kills under host load: telemetry gaps {gaps_ms} ms (1 ms period; periods lost {lost}); "
            f"driver ready {', '.join(f'{r:.1f}' for r in ready[1:])} ms after respawn; "
            f"ecu-sim reopened after {', '.join(f'{r:.1f}' for r in reopen)} ms; {len(frames)} frames")


def t_supervisor_readiness(t, bus):
    """Supervisor: a start that never becomes ready is killed and retried, a path served by another
    process is not readiness, and spawn failures count against -n."""
    deploy_tools(t)
    out_a = t.out("/tmp/ctucan-supervisor -n 2 -r 300 -- /system/bin/sleep 30; echo exit=$?", timeout=60)
    t.ssh("/tmp/dev-can-ctucanfd >/dev/null 2>&1 </dev/null")
    wait_path(t, TX1)
    out_b = t.out("/tmp/ctucan-supervisor -n 1 -r 500 -- /tmp/dev-can-ctucanfd; echo exit=$?", timeout=60)
    t.stop_all()
    out_c = t.out("/tmp/ctucan-supervisor -n 2 -- /tmp/no-such-driver; echo exit=$?", timeout=60)
    check(out_a.count("not ready within 300 ms") == 3 and "restart limit 2 reached" in out_a and "exit=1" in out_a,
          out_a)
    check(out_b.count("path served by another process") == 2 and "restart limit 1 reached" in out_b, out_b)
    check(out_c.count("spawn /tmp/no-such-driver") == 3 and "restart limit 2 reached" in out_c, out_c)
    return ("never ready: killed after 300 ms x3, then 'restart limit 2 reached'; path served by another "
            "driver: not taken as ready (x2); spawn failure x3 counted against -n 2")


def t_supervisor_hang(t, bus):
    """A server that accepts the open and never answers the devctl: the probe is abandoned at its
    deadline, the server is killed and started again, and the supervisor stops at -n."""
    deploy_tools(t)
    t0 = time.monotonic()
    out = t.out("/tmp/ctucan-supervisor -n 1 -r 1000 -- /tmp/ctucan-hangsrv; echo exit=$?", timeout=60)
    dt = time.monotonic() - t0
    t.ssh("slay -f -s KILL ctucan-hangsrv >/dev/null 2>&1; true")
    check(out.count("not ready within 1000 ms (Connection timed out), killed") == 2 and
          "restart limit 1 reached" in out and "exit=1" in out, out)
    check(dt < 15, f"supervisor took {dt:.1f} s")
    return (f"server never replies: not ready within 1000 ms (probe timed out) x2, killed each time, "
            f"'restart limit 1 reached', supervisor done in {dt:.1f} s")


def t_ecu_sim(t, bus, seconds=30):
    """ecu-sim: 64-byte telemetry every 10 ms checked on the host (CRC, sequence, period); one diagnostic exchange."""
    deploy_tools(t)
    t.driver(force=True)
    t.put(os.path.join(ROOT, "build", "x86_64", "ecu-sim"), "/tmp/ecu-sim")
    t.bg("/tmp/ecu-sim", "/tmp/ecu.log")
    cd = Candump(bus_if(bus), "18F:7FF,7E8:7FF")
    time.sleep(seconds / 2)
    t_req = time.time()
    bus.send(0x7DF, bytes([2, 0x01, 0x5B, 0, 0, 0, 0, 0]))
    time.sleep(seconds / 2)
    frames = cd.stop()
    t.ssh("slay -s TERM ecu-sim; sleep 0.3")
    log = t.out("cat /tmp/ecu.log")
    tel = [f for f in frames if f["id"] == 0x18F]
    rsp = [f for f in frames if f["id"] == 0x7E8]
    check(len(rsp) == 1 and rsp[0]["data"][:3] == bytes([3, 0x41, 0x5B]), f"diagnostic response {rsp}")
    soc = rsp[0]["data"][3] * 100 / 255
    bad_crc = sum(1 for f in tel if crc8(f["data"][:63]) != f["data"][63])
    seqs = [seq_of(f["data"]) for f in tel]
    check(bad_crc == 0 and seqs == list(range(seqs[0], seqs[0] + len(seqs))), f"{bad_crc} bad CRC, sequence holes")
    d = sorted((b["t"] - a["t"]) * 1000 for a, b in zip(tel, tel[1:]))
    n = len(d)
    mean = sum(d) / n
    sd = (sum((x - mean) ** 2 for x in d) / n) ** 0.5
    p = lambda q: d[min(n - 1, int(q * n))]
    return (f"{len(tel)} telemetry frames, CRC and sequence clean; host-side period (QEMU timing) mean "
            f"{mean:.3f} ms, sd {sd:.3f}, min {d[0]:.3f}, p1 {p(0.01):.3f}, p99 {p(0.99):.3f}, max {d[-1]:.3f} ms; "
            f"7DF 02 01 5B answered by 7E8 03 41 5B (SOC {soc:.0f} %) {(rsp[0]['t'] - t_req) * 1000:.1f} ms after "
            f"cansend; {log.strip().splitlines()[-1]}")


def t_two_nodes(t, bus, seconds=30):
    """Two QNX nodes on one bus (--peer): ecu-sim on the target, canfd-test tester on the peer, candump observing."""
    if PEER is None:
        raise Skip("needs a second target on the same host interface (--peer)")
    p = Target(PEER)
    for n in (t, p):
        deploy_tools(n)
        n.driver(force=True)
    cd = Candump(bus_if(bus), "18F:7FF,1E0:7FF,1E8:7FF,7DF:7FF,7E8:7FF")
    t.bg("/tmp/ecu-sim", "/tmp/ecu.log")
    time.sleep(0.5)
    rc, out = p.ssh(f"{TEST} tester {RX0} {TX1} {seconds}", timeout=seconds + 30)
    time.sleep(0.3)
    frames = cd.stop()
    t.ssh("slay -s TERM ecu-sim; sleep 0.3")
    elog = t.out("cat /tmp/ecu.log").strip().splitlines()
    p.stop_all()
    bus.drain()
    check(rc == 0 and "tester: PASS" in out, f"tester on {p.name}: {out.strip()}")
    n_tel, s0, s1 = map(int, re.search(r"telemetry 18F: (\d+) frames, seq (\d+)\.\.(\d+)", out).groups())
    n_echo = int(re.search(r"echo 1E0->1E8: (\d+) sent", out).group(1))
    rt = [float(x) for x in re.search(r"round trip ms min ([\d.]+), p50 ([\d.]+), p99 ([\d.]+), max ([\d.]+)",
                                      out).groups()]
    n_diag = int(re.search(r"diag 7DF->7E8: (\d+) sent", out).group(1))
    by = lambda i: [f for f in frames if f["id"] == i]
    tel, req, rsp = by(0x18F), by(0x1E0), by(0x1E8)
    # candump -L writes CAN FD flags as one hex digit after "##"; bit 0 is BRS
    fdbrs = lambda fs: all(f["fdflags"] is not None and int(f["fdflags"], 16) & 1 and len(f["data"]) == 64
                           for f in fs)
    # the host saw every telemetry period, and the peer got each one in the range it reports
    tseq = [seq_of(f["data"]) for f in tel]
    check(tseq == list(range(tseq[0], tseq[0] + len(tseq))), f"telemetry sequence holes on {bus_if(bus)}")
    check(all(crc8(f["data"][:63]) == f["data"][63] for f in tel) and fdbrs(tel), "telemetry frame on the host")
    check(tseq[0] <= s0 and s1 <= tseq[-1] and n_tel == s1 - s0 + 1, f"peer range {s0}..{s1} vs host {tseq[0]}..")
    for what, fs in (("echo requests", req), ("echo replies", rsp)):
        check([seq_of(f["data"]) for f in fs] == list(range(n_echo)), f"{what} on {bus_if(bus)}: sequence")
        check(all(f["data"] == pattern(seq_of(f["data"]), 64) for f in fs) and fdbrs(fs), f"{what}: payload")
    check(len(by(0x7DF)) == n_diag and len(by(0x7E8)) == n_diag, f"diagnostic frames on {bus_if(bus)}")
    check(f"echoes {n_echo}" in elog[-1] and f"diagnostic answers {n_diag}" in elog[-1], f"ecu-sim: {elog[-1]}")
    turn = sorted((b["t"] - a["t"]) * 1000 for a, b in zip(req, rsp))
    q = lambda v, x: v[min(len(v) - 1, int(x * len(v)))]
    return (f"{t.name} -> {p.name}: {n_tel} telemetry frames (64-byte CAN FD with BRS), seq {s0}..{s1}, CRC and "
            f"sequence clean on {p.name} and on {bus_if(bus)}; {p.name} -> {t.name} -> {p.name}: {n_echo}/{n_echo} "
            f"64-byte echo round trips, every byte checked on {p.name} and on {bus_if(bus)}; QEMU timing: round trip "
            f"on {p.name}'s clock min {rt[0]:.3f} ms, p50 {rt[1]:.3f}, p99 {rt[2]:.3f}, max {rt[3]:.3f}; of that, "
            f"{t.name} between request and "
            f"reply on {bus_if(bus)} p50 {q(turn, 0.5):.3f} ms, p99 {q(turn, 0.99):.3f}, max {turn[-1]:.3f}; "
            f"{n_diag}/{n_diag} diagnostic requests answered; peer: "
            f"{'; '.join(l[8:] for l in out.strip().splitlines() if 'telemetry' in l)}; {elog[-1]}")


def crc8(data):
    c = 0xFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x1D) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def bus_if(bus):
    return bus.ifname


# ---- tests on the fault-injection build ---------------------------------

FI = "x86_64-fi"


def fi(t, scenario, arg=0, arg2=0, budget=0, flags=0):
    out = t.out(f"{TEST} fi {RX0} {scenario} {arg} {arg2} {budget} {flags}")
    check("No error" in out, f"fault injection request failed: {out.strip()}")


def ticker(t, ms):
    """The fault-injection script advances on CTUCAN_DEVCTL_FI_TICK, sent every millisecond."""
    t.bg(f"{TEST} fitick {RX0} {ms}", "/tmp/tick.log")


def send_burst(bus, cid, count, length=64, paced=0.0, groups=0, pause=0.05, payload=None):
    for q in range(count):
        bus.send(cid, payload(q) if payload else pattern(q, length))
        if paced:
            time.sleep(paced)
        elif groups and q % groups == groups - 1:
            time.sleep(pause)


def monitor_line(t):
    m = re.search(r"monitor: .*", t.out("cat /tmp/mon.log"))
    check(m is not None, "no monitor output")
    return m.group(0), int(re.search(r"(\d+) answered while", m.group(0)).group(1))


def t_burst_batches(t, bus):
    """300 frames in bursts of 90 (above the 64-frame batch): every frame arrives, devctls answered between batches."""
    t.driver("-q 512", force=True)
    x0 = t.xstats()
    t.bg(f"{TEST} seqrecv {RX0} 300 20000", "/tmp/bb.log")
    t.bg(f"{TEST} monitor {TX1} 2500", "/tmp/mon.log")
    time.sleep(0.5)
    send_burst(bus, 0x2A8, 300, groups=90)
    time.sleep(2.5)
    x, log = t.xstats(), t.out("cat /tmp/bb.log")
    mon, between = monitor_line(t)
    check("got 300 of 300, bad 0, missing 0, out_of_order 0" in log, log.strip())
    check(x["deferred"] > x0["deferred"] and x["unmask_err"] == 0 and between > 0, f"{x} / {mon}")
    return (f"300/300 in order; {x['hw_pulses'] - x0['hw_pulses']} interrupt pulses, "
            f"{x['deferred'] - x0['deferred']} left work for the next, largest batch {x['rx_max_batch']} frames; {mon}")


def t_fi_reset_storm(t, bus):
    """A restart whose reset fails while frames sit in the FIFO and an interrupt is queued: the stopped
    core's output stays off, the queued pulse is unmasked once, no storm; the next restart works."""
    t.driver("-q 512", variant=FI, force=True)
    fi(t, 4, 5000, 4)
    t.ssh(f"{TEST} seqsend {TX1} 2E8 16 4 0 >/dev/null")
    fi(t, 6, 1)
    # 400 ms of host frames, so some arrive while the restart waits for the held frames (an ssh
    # round trip to start the devctl takes about 100 ms)
    load = threading.Thread(target=send_burst, args=(bus, 0x2AD, 400), kwargs=dict(paced=0.001))
    load.start()
    time.sleep(0.05)
    out1 = t.out(f"{TEST} timing {RX0} 4 43 6 6")
    load.join()
    x1 = t.xstats()
    time.sleep(1.0)
    x2 = t.xstats()
    regs = t.out(f"{TEST} info {RX0}")
    out2 = t.out(f"{TEST} timing {RX0} 4 43 6 6")
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 2EA 8")
    fr = bus.recv(1, 3, ids={0x2EA})
    pulses = x2["hw_pulses"] - x1["hw_pulses"]
    check("timed out" in out1 and x1["restart_fail"] == 1, f"first restart: {out1.strip()} {x1}")
    check(x1["stopped_pulses"] >= 1, f"no interrupt pulse arrived while stopped: {x1}")
    check(pulses == 0, f"{pulses} interrupt pulses in 1 s with the core stopped")
    check("No error" in out2 and len(fr) == 1, f"second restart: {out2.strip()}, host {fr}")
    return (f"reset failed with RX frames left in the FIFO; {x1['stopped_pulses']} queued pulse(s) unmasked with "
            f"the output off; 0 pulses in the next second; second restart, TX works")


def t_fi_reader_nowake(t, bus):
    """Returns that wake nobody: retried for 8 pulses, then the mailbox's frames are dropped and counted;
    bounded pulses, and once wake-ups work again the same reader gets the next frames."""
    t.driver("-q 256", variant=FI, force=True)
    fi(t, 0, flags=8)
    t.bg(f"{TEST} recv {RX0} 3", "/tmp/nw.log")
    time.sleep(0.5)
    x0 = t.xstats()
    send_burst(bus, 0x2AE, 5, length=8, paced=0.01)
    time.sleep(0.5)
    x1 = t.xstats()
    fi(t, 0)
    send_burst(bus, 0x2AE, 3, length=8, paced=0.01)
    time.sleep(0.5)
    got = parse_recv(t.out("cat /tmp/nw.log"))
    log = t.out("slog2info 2>/dev/null | grep 'woke no reader' | tail -1")
    pulses = x1["hw_pulses"] - x0["hw_pulses"]
    stuck = x1["notify_stuck"] - x0["notify_stuck"]
    check(stuck >= 1 and x1["rx_discarded"] - x0["rx_discarded"] == 5, f"{x1}")
    check(8 <= pulses <= 8 * 5 + 5, f"{pulses} interrupt pulses for 5 frames")
    check(len(got) == 3 and "woke no reader" in log, f"after wake-ups resumed: {got}; log {log.strip()}")
    return (f"5 frames, {stuck} recoveries after 8 fruitless returns each, 5 frames dropped and counted, "
            f"{pulses} pulses in all; then the blocked reader got the next 3 frames")


def t_fi_busoff_abort_notick(t, bus):
    """Bus-off with aborts that take 100 ms, no FI ticker and no traffic: the 20 ms abort bound holds."""
    t.driver(variant=FI, force=True)
    fi(t, 4, 5000, 4)
    t.ssh(f"{TEST} seqsend {TX1} 2E9 16 4 0 >/dev/null")
    x0 = t.xstats()
    t0 = time.monotonic()
    fi(t, 3, 2, 1)      # bus-off at once, slow aborts, no ticker started
    x1 = t.xstats()
    check(x0["inflight"] == 4, f"in flight before bus-off: {x0['inflight']}")
    check(x1["busoff_hold"] == 1 and x1["inflight"] == 0 and x1["tx_abort_forced"] == 4, f"{x1}")
    return "4 frames stuck aborting dropped and counted at bus-off within the request (tx_abort_forced 4), hold kept"


def t_fi_rejected_normal(t, bus):
    """The normal build answers the fault-injection devctls like any unknown command."""
    t.driver(force=True)
    out = t.out(f"{TEST} fi {RX0} 1; {TEST} fitick {RX0} 5")
    check(out.count("Inappropriate I/O control operation") == 2 and "No error" not in out, out.strip())
    return "CTUCAN_DEVCTL_FI and CTUCAN_DEVCTL_FI_TICK -> ENOTTY"


def t_fi_warn(t, bus):
    """Warning limit up and down while FAULT_STATE stays error active: counted from STATUS.EWL."""
    t.driver(variant=FI, force=True)
    fi(t, 1)
    ticker(t, 200)
    time.sleep(0.5)
    x, s = t.xstats(), t.stats()
    check(x["ewl_up"] == 1 and x["ewl_down"] == 1, f"ewl up {x['ewl_up']} down {x['ewl_down']}")
    check(s["warn"] == 1 and s["passive"] == 0 and s["bus_off"] == 0, f"stats {s}")
    check(x["fault_state"] == 1, f"fault state {x['fault_state']}")
    return "ewl_up 1, ewl_down 1, warn 1, FAULT_STATE error active throughout"


def t_fi_passive(t, bus):
    """Error passive and back to error active; traffic continues."""
    t.driver(variant=FI, force=True)
    fi(t, 2)
    ticker(t, 200)
    time.sleep(0.5)
    x, s = t.xstats(), t.stats()
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 3D0 8")
    fr = bus.recv(1, 3, ids={0x3D0})
    check(s["passive"] == 1 and s["bus_off"] == 0 and x["fault_state"] == 1, f"stats {s}, state {x['fault_state']}")
    check(len(fr) == 1, "TX failed after error passive")
    return "passive 1, back to error active, TX works"


def gaps(frames):
    return [round((b["t"] - a["t"]) * 1000, 1) for a, b in zip(frames, frames[1:])]


def t_fi_busoff_auto(t, bus):
    """Bus-off with -a: TX held, recovery requested, TX resumes after FAULT_STATE reads error active."""
    t.driver("-a", variant=FI, force=True)
    bus.drain()
    cap = Capture(bus, 300, 8, ids={0x2D0})
    ticker(t, 1500)
    t.bg(f"{TEST} seqsend {TX1} 2D0 16 300 1000", "/tmp/bo.log")
    time.sleep(0.15)
    fi(t, 3)
    got = cap.wait()
    x, s = t.xstats(), t.stats()
    check_seq_stream(got, 300, 16, "host side")
    check(s["bus_off"] == 1 and x["busoff_recovered"] == 1 and x["fault_state"] == 1, f"{s} {x}")
    return f"300/300 in order, bus_off 1, recovered 1, largest gap {max(gaps(got))} ms (1 ms pacing)"


def t_fi_busoff_latch(t, bus):
    """After bus-off, FAULT_STATE reading none and then passive does not release TX; only error active does."""
    t.driver("-a", variant=FI, force=True)
    bus.drain()
    cap = Capture(bus, 200, 8, ids={0x2D2})
    ticker(t, 1500)
    t.bg(f"{TEST} seqsend {TX1} 2D2 16 200 2000", "/tmp/bl.log")
    time.sleep(0.15)
    # timed on the target: the scenario takes about 110 ms, an ssh round trip about 100 ms
    out = t.out(f"{TEST} fi {RX0} 3 1 0 0 0 && sleep 0.06 && {TEST} xstats {RX0}")
    mid = dict((k, int(v, 0)) for k, v in re.findall(r"(\w+) (0x[0-9a-f]+|\d+)", out))
    check("busoff_hold" in mid, out.strip())
    got = cap.wait()
    x = t.xstats()
    check_seq_stream(got, 200, 16, "host side")
    g = max(gaps(got))
    check(mid["busoff_hold"] == 1 and x["busoff_hold"] == 0 and x["busoff_recovered"] == 1, f"{mid} {x}")
    check(g >= 95, f"largest gap {g} ms: TX was not held through the none and passive phases (100 ms)")
    return (f"hold still set 60 ms in (FAULT_STATE {mid['fault_state']}); 200/200 in order; "
            f"largest gap {g} ms against 2 ms pacing")


def t_fi_busoff_manual(t, bus):
    """Bus-off without -a: TX stays held (queue fills, writers get EAGAIN) until CTUCAN_DEVCTL_RECOVER."""
    t.driver(variant=FI, force=True)
    fi(t, 3)
    ticker(t, 4000)
    time.sleep(0.1)
    bus.drain()
    t.bg(f"{TEST} seqsend {TX1} 2D1 16 100 0", "/tmp/bm.log")
    held = bus.recv(100, 1.0, ids={0x2D1})
    x = t.xstats()
    t.ssh(f"{TEST} recover {RX0}")
    got = bus.recv(100, 5, ids={0x2D1})
    time.sleep(0.3)
    x2, log = t.xstats(), t.out("cat /tmp/bm.log")
    check(not held, f"{len(held)} frame(s) left during bus-off")
    check(x["fault_state"] == 4 and x2["busoff_recovered"] == 1 and x2["fault_state"] == 1, f"{x} {x2}")
    check_seq_stream(got, 100, 16, "after recovery")
    retries = int(re.search(r"(\d+) EAGAIN", log).group(1))
    check(retries > 0, f"writer never saw EAGAIN: {log.strip()}")
    return f"0 frames while bus-off, writer EAGAIN x{retries}, 100/100 in order after recovery"


def t_fi_busoff_stuck_abort(t, bus):
    """Bus-off with frames in flight whose aborts take 100 ms: dropped at the 20 ms bound, TX resumes later."""
    t.driver("-a", variant=FI, force=True)
    fi(t, 4, 5000, 4)
    ticker(t, 2000)
    t.ssh(f"{TEST} seqsend {TX1} 2E3 16 4 0 >/dev/null")
    x0 = t.xstats()
    fi(t, 3, 0, 1)
    time.sleep(0.5)
    x1 = t.xstats()
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 2E4 8")
    fr = bus.recv(1, 3, ids={0x2E4})
    check(x0["inflight"] == 4, f"in flight before bus-off: {x0['inflight']}")
    check(x1["tx_abort_forced"] == 4 and x1["busoff_recovered"] == 1 and x1["inflight"] == 0, f"{x1}")
    check(len(fr) == 1, "TX did not resume after the slow aborts finished")
    return "4 frames stuck aborting -> dropped and counted at the bound (tx_abort_forced 4), recovery, TX resumes"


def t_fi_txdelay(t, bus):
    """Late TX completions: four frames in flight, queue fills, FIFO order kept."""
    t.driver(variant=FI, force=True)
    fi(t, 4, 20, 100)
    ticker(t, 3000)
    bus.drain()
    cap = Capture(bus, 100, 10, ids={0x2E0})
    t.ssh(f"{TEST} seqsend {TX1} 2E0 16 100 0 >/tmp/td.log 2>&1")
    got = cap.wait()
    time.sleep(0.3)
    x, log = t.xstats(), t.out("cat /tmp/td.log")
    check_seq_stream(got, 100, 16, "host side")
    check(x["tx_max_inflight"] == 4 and x["tx_order_err"] == 0, f"{x}")
    return f"100/100 in order, max in flight {x['tx_max_inflight']}, {log.strip()}"


def t_fi_tx_budget(t, bus):
    """RX budget of one frame per pulse while held TX completions are released three at a time: none lost."""
    t.driver("-q 512", variant=FI, force=True)
    fi(t, 4, 5, 300, 1)
    ticker(t, 6000)
    bus.drain()
    t.bg(f"{TEST} seqrecv {RX0} 300 20000", "/tmp/tb.log")
    time.sleep(0.5)
    cap = Capture(bus, 300, 15, ids={0x2E5})
    t.bg(f"{TEST} seqsend {TX1} 2E5 16 300 0", "/tmp/tbs.log")
    send_burst(bus, 0x2A9, 300, paced=0.002)   # paced: one frame per pulse must keep up with the FIFO
    got = cap.wait()
    time.sleep(0.5)
    x, log = t.xstats(), t.out("cat /tmp/tb.log")
    check_seq_stream(got, 300, 16, "host side")
    check("got 300 of 300, bad 0, missing 0, out_of_order 0" in log, log.strip())
    check(x["tx_ok"] == 300 and x["tx_order_err"] == 0 and x["rx_max_batch"] == 1, f"{x}")
    return (f"300 TX + 300 RX complete and in order with 1 RX frame per pulse; tx_ok {x['tx_ok']}, "
            f"{x['tx_batches']} multi-buffer TX commands, {x['hw_pulses']} pulses")


def t_fi_quiesce(t, bus):
    """SET_TIMING with frames that do not finish: wait, abort, drop and count, restart, resume.
    QEMU sent these frames when they were set ready; this tests the driver's bookkeeping only."""
    t.driver(variant=FI, force=True)
    fi(t, 4, 5000, 4)
    t.ssh(f"{TEST} seqsend {TX1} 2E1 16 4 0 >/dev/null")
    x0 = t.xstats()
    out = t.out(f"{TEST} timing {RX0} 4 43 6 6")
    x1 = t.xstats()
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 2E2 8")
    fr = bus.recv(1, 3, ids={0x2E2})
    check(x0["inflight"] == 4, f"in flight before restart: {x0['inflight']}")
    check("No error" in out and x1["tx_aborted"] - x0["tx_aborted"] == 4 and x1["restarts"] == 1, f"{out} {x1}")
    check(len(fr) == 1, "TX failed after the restart")
    return "4 frames in flight -> aborted and counted (tx_aborted +4), restart, TX resumes"


def t_fi_reset_retry(t, bus):
    """A restart whose reset times out leaves the core stopped; the next restart brings TX and RX back."""
    t.driver(variant=FI, force=True)
    fi(t, 6, 1)
    out1 = t.out(f"{TEST} timing {RX0} 4 43 6 6")
    x1 = t.xstats()
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 2E6 8")
    lost = bus.recv(1, 1, ids={0x2E6})
    out2 = t.out(f"{TEST} timing {RX0} 4 43 6 6")
    queued = bus.recv(1, 3, ids={0x2E6})
    t.bg(f"{TEST} recv {RX0} 1", "/tmp/rr.log")
    time.sleep(0.5)
    bus.send(0x2AA, pattern(7, 8))
    time.sleep(0.5)
    got = parse_recv(t.out("cat /tmp/rr.log"))
    x2 = t.xstats()
    check("timed out" in out1 and x1["restart_fail"] == 1, f"first restart: {out1.strip()} {x1}")
    check(not lost, "a frame left while the core was stopped")
    check("No error" in out2 and len(queued) == 1 and x2["restarts"] == 1, f"second restart: {out2.strip()} {x2}")
    check(len(got) == 1 and got[0]["data"] == pattern(7, 8), f"RX after the second restart: {got}")
    return "reset timeout -> ETIMEDOUT, core stopped, nothing sent; second restart -> queued frame sent, RX works"


def t_fi_state_clear(t, bus):
    """Scenario OFF disarms a pending RX corruption; a core reset ends a bus-off overlay."""
    t.driver(variant=FI, force=True)
    x0 = t.xstats()
    fi(t, 5, 2, 0)
    fi(t, 0)
    t.bg(f"{TEST} seqrecv {RX0} 10 5000", "/tmp/sc.log")
    time.sleep(0.5)
    send_burst(bus, 0x2AB, 10, paced=0.003)
    time.sleep(0.5)
    log, x1 = t.out("cat /tmp/sc.log"), t.xstats()
    fi(t, 3)
    ticker(t, 300)
    time.sleep(0.4)
    x2 = t.xstats()
    t.ssh(f"{TEST} timing {RX0} 4 43 6 6")
    bus.drain()
    t.ssh(f"{TEST} send {TX1} 2E7 8")
    fr = bus.recv(1, 3, ids={0x2E7})
    x3 = t.xstats()
    check("got 10 of 10, bad 0" in log and x1["rx_flushes"] == x0["rx_flushes"], f"after OFF: {log.strip()} {x1}")
    check(x2["fault_state"] == 4 and x2["busoff_hold"] == 1, f"bus-off not reached: {x2}")
    check(x3["fault_state"] != 4 and x3["busoff_hold"] == 0 and len(fr) == 1, f"after reset: {x3}, host {fr}")
    return "OFF: 10/10 frames, no flush; bus-off then restart: overlay and hold gone, TX works"


def t_fi_batches(t, bus):
    """Forced small batches (4 frames per pulse): 90-frame burst complete, devctls answered between batches."""
    t.driver("-q 256", variant=FI, force=True)
    fi(t, 0, budget=4)
    t.bg(f"{TEST} seqrecv {RX0} 90 10000", "/tmp/race.log")
    t.bg(f"{TEST} monitor {TX1} 1500", "/tmp/mon.log")
    time.sleep(0.5)
    x0 = t.xstats()
    send_burst(bus, 0x2A4, 90)
    time.sleep(1.5)
    x, log = t.xstats(), t.out("cat /tmp/race.log")
    mon, between = monitor_line(t)
    check("got 90 of 90, bad 0, missing 0, out_of_order 0" in log, log.strip())
    check(x["unmask_err"] == 0 and x["rx_max_batch"] <= 4 and between > 0, f"{x} / {mon}")
    return (f"90/90 in order in {x['hw_pulses'] - x0['hw_pulses']} interrupt pulses of at most 4 frames, "
            f"unmask_err 0; {mon}")


def t_fi_reader_budget(t, bus):
    """One RX mailbox return per pulse, readers on four mailboxes: RBNEI stays set until all are woken."""
    t.driver("-r 4 -q 256", variant=FI, force=True)
    fi(t, 0, flags=4)
    for k in range(4):
        t.bg(f"{TEST} recv /dev/can1/rx{k} 10", f"/tmp/rb{k}.log")
    time.sleep(0.8)
    x0 = t.xstats()
    send_burst(bus, 0x2AC, 10, length=8, paced=0.005)
    time.sleep(1.5)
    counts = [len(parse_recv(t.out(f"cat /tmp/rb{k}.log"))) for k in range(4)]
    left = t.out("pidin -p canfd-test -f a 2>/dev/null | grep -c canfd-test").strip()
    x = t.xstats()
    t.driver(force=True)
    check(counts == [10] * 4 and left == "0", f"reader counts {counts}, {left} still blocked")
    check(x["notify_deferred"] > x0["notify_deferred"], f"{x}")
    return (f"4 mailboxes x 10 frames, every reader finished; {x['notify_deferred'] - x0['notify_deferred']} "
            f"pulses held RBNEI for readers, {x['reports'] - x0['reports']} mailbox returns")


def fi_order_run(t, bus, cid, flags):
    t.driver(variant=FI, force=True)
    fi(t, 4, 5, 1000, 0, flags)
    ticker(t, 6000)
    bus.drain()
    cap = Capture(bus, 1000, 20, ids={cid})
    t.ssh(f"{TEST} seqsend {TX1} {cid:X} 8 1000 0 >/dev/null 2>&1")
    return cap.wait(), t.xstats()


def t_fi_tx_order(t, bus):
    """Rotating priorities with held completions: buffers refilled several at a time, order kept."""
    got, x = fi_order_run(t, bus, 0x2F2, 0)
    check_seq_stream(got, 1000, 8, "host side")
    check(x["tx_batches"] > 0 and x["tx_order_err"] == 0 and x["tx_max_inflight"] == 4, f"{x}")
    return f"1000/1000 in order across {x['tx_batches']} multi-buffer TX commands"


def t_fi_tx_order_fixed(t, bus):
    """Negative control: stage-one fixed priorities (buffer n gets n + 1) put the same frames out of order."""
    got, x = fi_order_run(t, bus, 0x2F1, 2)
    n = order_errors(got)
    check(len(got) == 1000 and n > 0 and x["tx_batches"] > 0, f"{len(got)} frames, {n} order breaks, {x}")
    return f"{n} order breaks in 1000 frames across {x['tx_batches']} multi-buffer TX commands"


FFW_LOOKALIKE = 0x00009A8F      # a valid format word: 64-byte CAN FD frame, BRS, RWCNT 19


def lookalike(q):
    return q.to_bytes(4, "little") + FFW_LOOKALIKE.to_bytes(4, "little") * 15


def t_fi_rx_corrupt(t, bus):
    """A format word whose count disagrees with its DLC: FIFO flushed at once, nothing fabricated from
    payloads that look like format words, adjacent frames either delivered intact or counted as flushed."""
    t.driver("-q 256", variant=FI, force=True)
    result = []
    for mode, paced, name in ((1, 0.0, "RWCNT one short, frames back to back"), (2, 0.003, "RWCNT 31, paced")):
        x0 = t.xstats()
        fi(t, 5, mode, 5)
        t.bg(f"{TEST} recv {RX0} 0", "/tmp/rxc.log")
        time.sleep(0.5)
        send_burst(bus, 0x2A7, 20, paced=paced, payload=lookalike)
        time.sleep(0.8)
        t.ssh("slay -f canfd-test >/dev/null 2>&1; true")
        got = parse_recv(t.out("cat /tmp/rxc.log"))
        x = t.xstats()
        seqs = [seq_of(g["data"]) for g in got]
        bad = [g for g in got if g["id"] != 0x2A7 or g["data"] != lookalike(seq_of(g["data"]))]
        fl, lost = x["rx_flushes"] - x0["rx_flushes"], x["rx_flushed"] - x0["rx_flushed"]
        check(not bad, f"{name}: {len(bad)} fabricated or corrupt frame(s) delivered")
        check(seqs == sorted(set(seqs)) and seqs[:5] == [0, 1, 2, 3, 4] and 5 not in seqs, f"{name}: {seqs}")
        check(fl == 1 and len(got) + lost == 20, f"{name}: flushes {fl}, delivered {len(got)} + flushed {lost}")
        result.append(f"{name}: {len(got)} delivered intact, {lost} flushed, 1 flush, none fabricated")
    return "; ".join(result)


TESTS = [v for k, v in sorted(globals().items()) if k.startswith("t_")]
ORDER = ["tx_lengths", "rx_lengths", "rtr", "ids", "write_path", "extended_mailbox", "readers", "writers",
         "rx_overflow", "bidir", "burst_batches", "tx_order", "devctl_sizes", "timing", "spoofed_irq_pulse",
         "fi_rejected_normal", "sigterm", "restart_under_load", "supervisor_readiness", "supervisor_hang",
         "ecu_sim", "two_nodes", "fi_warn",
         "fi_passive", "fi_busoff_auto", "fi_busoff_latch", "fi_busoff_manual", "fi_busoff_stuck_abort",
         "fi_busoff_abort_notick", "fi_txdelay", "fi_tx_budget", "fi_quiesce", "fi_reset_retry", "fi_reset_storm",
         "fi_state_clear", "fi_batches", "fi_reader_budget", "fi_reader_nowake", "fi_tx_order", "fi_tx_order_fixed",
         "fi_rx_corrupt"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default="tc")
    ap.add_argument("--peer", help="second target on the same host interface, for two_nodes")
    ap.add_argument("--if", dest="ifname", default="vcan0")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    by_name = {f.__name__[2:]: f for f in TESTS}
    names = [n for n in ORDER if n in by_name] + sorted(set(by_name) - set(ORDER))
    if a.list:
        for n in names:
            print(f"{n:20} {by_name[n].__doc__}")
        return 0
    sel = a.tests or names
    global PEER
    PEER = a.peer
    t, bus = Target(a.target), Bus(a.ifname)
    failed = skipped = 0
    for n in sel:
        t0 = time.monotonic()
        try:
            detail = by_name[n](t, bus)
            print(f"PASS {n}: {detail} ({time.monotonic() - t0:.1f} s)", flush=True)
        except Skip as e:
            skipped += 1
            print(f"SKIP {n}: {e}", flush=True)
        except (Fail, subprocess.TimeoutExpired, AttributeError, ValueError, IndexError) as e:
            failed += 1
            print(f"FAIL {n}: {type(e).__name__}: {e} ({time.monotonic() - t0:.1f} s)", flush=True)
    print(f"deployed files checked by md5 on the target after copying: {len(Target.deployed)} "
          f"({', '.join(sorted(Target.deployed))})")
    print(f"{len(sel) - failed - skipped} of {len(sel) - skipped} passed" + (f", {skipped} skipped" if skipped else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
