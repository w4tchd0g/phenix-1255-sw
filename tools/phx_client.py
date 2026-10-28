#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Phenix-1255 contributors
"""Phenix-1255 protocol test client (Ethernet), Python standard library only.

Examples:
  phx_client.py --host phenix-1255.local info
  phx_client.py --host 192.168.2.10 rx --seconds 5
  phx_client.py --host 192.168.2.10 tone --offset 50000 --seconds 3
  phx_client.py --host 192.168.2.10 late
  phx_client.py --host 192.168.2.10 timeout

The 'tone' test transmits. Use it only with an attenuator or a dummy load
and on a frequency you are allowed to transmit on.
"""

import argparse
import cmath
import math
import socket
import struct
import sys
import threading
import time

PROTO_VERSION = 1
CTRL_MAGIC = 0x4350
SAMPLE_MAGIC = 0x5350
OP_RESPONSE = 0x80

OP = {
    "HELLO": 0x01, "KEEPALIVE": 0x02, "GET_INFO": 0x03, "GET_STATUS": 0x04,
    "SET_FREQ": 0x10, "SET_RATE": 0x11, "SET_GAIN": 0x12, "SET_RX_INPUT": 0x13,
    "SET_TX_OUTPUT": 0x14, "READ_REG": 0x20, "WRITE_REG": 0x21,
    "STREAM_RX": 0x30, "STREAM_TX": 0x31, "EVENT": 0x7F,
}
GAIN = {"LNA": 0, "PGA": 1, "DAC": 2, "MIXER": 3}

CTRL_HDR = struct.Struct("<HBBHH")
SAMPLE_HDR = struct.Struct("<HBBHHIIIIq")
MAX_SAMPLES = 360
INFO = struct.Struct("<HBB32s32sIIIHHI8I")
STATUS = struct.Struct("<qIIIIII")


class PhxError(Exception):
    pass


class PhxClient:
    def __init__(self, host, ctrl_port=1255, sample_port=1256):
        self.host = socket.gethostbyname(host)
        self.sample_port = sample_port
        self.tcp = socket.create_connection((self.host, ctrl_port), timeout=3)
        self.tcp.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.udp.bind(("", 0))
        self.udp.settimeout(1.0)
        self.seq = 0
        self.lock = threading.Lock()
        self.responses = {}
        self.cond = threading.Condition()
        self.events = []
        self.closed = False
        self.keepalive_enabled = True
        threading.Thread(target=self._reader, daemon=True).start()
        info = self.call("HELLO", struct.pack("<HH", PROTO_VERSION, self.udp.getsockname()[1]))
        self.info = self._parse_info(info)
        threading.Thread(target=self._keepalive, daemon=True).start()

    @staticmethod
    def _parse_info(data):
        f = INFO.unpack(data[:INFO.size])
        return {
            "proto_version": f[0], "transport": f[1], "fw_version": f[3].split(b"\0")[0].decode(),
            "serial": f[4].split(b"\0")[0].decode(), "xtal_hz": f[5], "freq_min_hz": f[6],
            "freq_max_hz": f[7], "max_packet_samples": f[8], "tx_buffer_samples": f[10],
            "rates": list(f[11:11 + f[2]]),
        }

    def _recv_all(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.tcp.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("connection closed by device")
            buf += chunk
        return buf

    def _reader(self):
        try:
            self.tcp.settimeout(None)
            while True:
                magic, ver, op, seq, length = CTRL_HDR.unpack(self._recv_all(CTRL_HDR.size))
                payload = self._recv_all(length) if length else b""
                if op == OP["EVENT"]:
                    code, _, detail = struct.unpack("<HHi", payload[:8])
                    self.events.append((time.time(), code, detail))
                    print(f"EVENT code={code} detail={detail}", file=sys.stderr)
                    continue
                with self.cond:
                    self.responses[seq] = payload
                    self.cond.notify_all()
        except (ConnectionError, OSError):
            with self.cond:
                self.closed = True
                self.cond.notify_all()

    def _keepalive(self):
        while not self.closed:
            time.sleep(0.25)
            if self.keepalive_enabled and not self.closed:
                try:
                    self.call("KEEPALIVE")
                except (PhxError, OSError):
                    return

    def call(self, name, payload=b"", timeout=2.0):
        with self.lock:
            self.seq = (self.seq + 1) & 0xFFFF
            seq = self.seq
            self.tcp.sendall(CTRL_HDR.pack(CTRL_MAGIC, PROTO_VERSION, OP[name], seq,
                                           len(payload)) + payload)
        deadline = time.time() + timeout
        with self.cond:
            while seq not in self.responses:
                if self.closed:
                    raise PhxError("connection closed")
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise PhxError(f"{name}: no response")
                self.cond.wait(remaining)
            resp = self.responses.pop(seq)
        status = struct.unpack("<i", resp[:4])[0]
        if status != 0:
            raise PhxError(f"{name} failed: status {status}")
        return resp[4:]

    # Commands
    def status(self):
        f = STATUS.unpack(self.call("GET_STATUS")[:STATUS.size])
        return dict(zip(("counter", "rate", "state", "rx_dropped", "tx_late", "tx_early",
                         "fifo_errors"), f))

    def set_freq(self, tx, hz):
        frf, actual = struct.unpack("<II", self.call("SET_FREQ", struct.pack("<B3xI", tx, hz)))
        return frf * self.info["xtal_hz"] / 2**20

    def set_rate(self, rate):
        self.call("SET_RATE", struct.pack("<I", rate))

    def set_gain(self, name, db):
        return struct.unpack("<hH", self.call("SET_GAIN", struct.pack("<Bxh", GAIN[name], db)))[0]

    def set_u8(self, name, value):
        return self.call(name, struct.pack("<B3x", value))

    def stream(self, tx, enable):
        resp = self.set_u8("STREAM_TX" if tx else "STREAM_RX", 1 if enable else 0)
        return struct.unpack("<q", resp)[0]

    def recv_rx(self):
        data, _ = self.udp.recvfrom(2048)
        f = SAMPLE_HDR.unpack(data[:SAMPLE_HDR.size])
        hdr = dict(zip(("magic", "version", "type", "flags", "count", "seq", "rx_dropped",
                        "tx_late", "tx_early", "timestamp"), f))
        samples = struct.unpack(f"<{2 * hdr['count']}h", data[SAMPLE_HDR.size:
                                                             SAMPLE_HDR.size + 4 * hdr["count"]])
        return hdr, samples

    def send_tx(self, seq, timestamp, iq):
        payload = struct.pack(f"<{len(iq)}h", *iq)
        pkt = SAMPLE_HDR.pack(SAMPLE_MAGIC, PROTO_VERSION, 2, 0, len(iq) // 2, seq, 0, 0, 0,
                              timestamp) + payload
        self.udp.sendto(pkt, (self.host, self.sample_port))

    def close(self):
        self.closed = True
        self.tcp.close()
        self.udp.close()


def tone_power(samples, fs, freq):
    """Share of the AC power at freq (dB) and the AC power (dBFS)."""
    n = len(samples) // 2
    z = [complex(samples[2 * k], samples[2 * k + 1]) / 32767.0 for k in range(n)]
    mean = sum(z) / n
    z = [v - mean for v in z]
    total = sum(abs(v) ** 2 for v in z) / n
    w = -2j * math.pi * freq / fs
    c = sum(v * cmath.exp(w * k) for k, v in enumerate(z)) / n
    return 10 * math.log10(abs(c) ** 2 / total + 1e-20), 10 * math.log10(total + 1e-20)


def cmd_info(cli, args):
    for k, v in cli.info.items():
        print(f"{k:20s} {v}")
    for k, v in cli.status().items():
        print(f"{k:20s} {v}")


def cmd_rx(cli, args):
    cli.set_rate(args.rate)
    start = cli.stream(False, True)
    t0 = time.time()
    expected = None
    packets = gaps = lost_samples = 0
    first_ts = last_ts = None
    while time.time() - t0 < args.seconds:
        hdr, _ = cli.recv_rx()
        ts = hdr["timestamp"]
        if first_ts is None:
            first_ts, t_first = ts, time.time()
        if expected is not None and ts != expected:
            gaps += 1
            lost_samples += ts - expected
        expected = ts + hdr["count"]
        last_ts, t_last = ts, time.time()
        packets += 1
    cli.stream(False, False)
    st = cli.status()
    rate = (last_ts - first_ts) / (t_last - t_first) if t_last > t_first else 0
    print(f"start counter {start}, {packets} packets, {gaps} gaps ({lost_samples} samples)")
    print(f"timestamp rate vs host clock: {rate:.0f} S/s (nominal {args.rate})")
    print(f"device: rx_dropped {st['rx_dropped']} fifo_errors {st['fifo_errors']}")
    return 0 if gaps == 0 and st["fifo_errors"] == 0 else 1


def cmd_tone(cli, args):
    fs = args.rate
    cli.set_rate(fs)
    cli.set_freq(0, args.freq)
    cli.set_freq(1, args.freq)
    cli.set_u8("SET_RX_INPUT", 0)
    for name, db in (("LNA", args.lna), ("PGA", args.pga), ("DAC", args.dac),
                     ("MIXER", args.mixer)):
        cli.set_gain(name, db)

    cli.stream(False, True)
    cli.stream(True, True)
    amp = int(32767 * args.amplitude)
    step = 2 * math.pi * args.offset / fs
    lead = int(fs * args.lead_ms / 1000)
    next_tx = None
    seq = 0
    capture = []
    t0 = time.time()
    try:
        while time.time() - t0 < args.seconds:
            hdr, iq = cli.recv_rx()
            now = hdr["timestamp"] + hdr["count"]
            if next_tx is None:
                next_tx = now + lead
            while next_tx < now + lead:
                iq_tx = []
                for k in range(MAX_SAMPLES):
                    ph = step * (next_tx + k)
                    iq_tx += [int(amp * math.cos(ph)), int(amp * math.sin(ph))]
                cli.send_tx(seq, next_tx, iq_tx)
                seq += 1
                next_tx += MAX_SAMPLES
            if time.time() - t0 > args.seconds / 2 and len(capture) < 8192:
                capture += iq
    finally:
        cli.stream(True, False)
        cli.stream(False, False)
    st = cli.status()
    p_pos, total = tone_power(capture, fs, args.offset)
    p_neg, _ = tone_power(capture, fs, -args.offset)
    print(f"RX AC power {total:.1f} dBFS, at {args.offset:+d} Hz {p_pos:.1f} dB, "
          f"at {-args.offset:+d} Hz {p_neg:.1f} dB")
    print(f"TX packets sent {seq}, device tx_late {st['tx_late']} tx_early {st['tx_early']} "
          f"rx_dropped {st['rx_dropped']} fifo_errors {st['fifo_errors']}")
    return 0 if p_pos > -1.0 and st["tx_late"] == 0 else 1


def cmd_burst(cli, args):
    """Send bursts of valid TX packets and check that none is lost or late."""
    cli.stream(False, True)
    cli.stream(True, True)
    rc = 0
    seq = 0
    for burst in (2, 10, 33, 60):
        before = cli.status()
        # 5 ms ahead of the current counter, within the device TX buffer
        start = before["counter"] + args.rate // 200
        for k in range(burst):
            cli.send_tx(seq, start + MAX_SAMPLES * k, [0, 0] * MAX_SAMPLES)
            seq += 1
        time.sleep(0.2)
        after = cli.status()
        late = after["tx_late"] - before["tx_late"]
        early = after["tx_early"] - before["tx_early"]
        print(f"burst {burst:3d}: late +{late} early +{early}")
        rc |= late != 0 or early != 0
    cli.stream(True, False)
    cli.stream(False, False)
    return rc


def cmd_late(cli, args):
    cli.stream(False, True)
    cli.stream(True, True)
    before = cli.status()
    now = before["counter"]
    cli.send_tx(0, now, [0, 0] * MAX_SAMPLES)          # in the past
    cli.send_tx(1, now + 10**9, [0, 0] * MAX_SAMPLES)  # far in the future
    time.sleep(0.2)
    after = cli.status()
    cli.stream(True, False)
    cli.stream(False, False)
    late = after["tx_late"] - before["tx_late"]
    early = after["tx_early"] - before["tx_early"]
    print(f"tx_late +{late}, tx_early +{early}")
    return 0 if late == 1 and early == 1 else 1


def cmd_timeout(cli, args):
    cli.stream(False, True)
    cli.stream(True, True)
    print("TX streaming, PA enabled:", bool(cli.status()["state"] & 4))
    cli.keepalive_enabled = False
    t0 = time.time()
    while not cli.closed and time.time() - t0 < 5:
        time.sleep(0.05)
    print(f"device closed the session after {time.time() - t0:.2f} s" if cli.closed
          else "session was NOT closed")
    cli2 = PhxClient(args.host, args.ctrl_port, args.sample_port)
    st = cli2.status()
    print("new session: state", hex(st["state"]), "(PA enabled)" if st["state"] & 4 else "(PA off)")
    cli2.close()
    return 0 if cli.closed and not st["state"] & 4 else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--host", default="phenix-1255.local")
    p.add_argument("--ctrl-port", type=int, default=1255)
    p.add_argument("--sample-port", type=int, default=1256)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    s = sub.add_parser("rx")
    s.add_argument("--seconds", type=float, default=5)
    s.add_argument("--rate", type=int, default=600000)
    s = sub.add_parser("tone")
    s.add_argument("--seconds", type=float, default=3)
    s.add_argument("--rate", type=int, default=600000)
    s.add_argument("--freq", type=int, default=434000000)
    s.add_argument("--offset", type=int, default=50000)
    s.add_argument("--amplitude", type=float, default=0.3)
    s.add_argument("--lead-ms", type=float, default=20)
    s.add_argument("--lna", type=int, default=0)
    s.add_argument("--pga", type=int, default=0)
    s.add_argument("--dac", type=int, default=0)
    s.add_argument("--mixer", type=int, default=30)
    sub.add_parser("late")
    s = sub.add_parser("burst")
    s.add_argument("--rate", type=int, default=600000)
    sub.add_parser("timeout")
    args = p.parse_args()

    cli = PhxClient(args.host, args.ctrl_port, args.sample_port)
    try:
        rc = {"info": cmd_info, "rx": cmd_rx, "tone": cmd_tone, "late": cmd_late,
              "burst": cmd_burst, "timeout": cmd_timeout}[args.cmd](cli, args)
    finally:
        cli.close()
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
