#!/usr/bin/env python3
"""Minimal Modbus RTU slave simulator for bench-testing the dongle.

Runs on a second USB-RS485 adapter (e.g. a Waveshare dongle on a Raspberry Pi)
wired to the same A/B pair as the dongle under test. Needs only pyserial:

    pip install pyserial
    python3 modbus_slave_sim.py /dev/ttyUSB0 --baud 9600 --parity N --slave 1
    python3 modbus_slave_sim.py /dev/ttyUSB0 --baud 19200 --parity E --slave 1 --slave 5 --delay 30

Data served by every simulated slave:
    holding registers 0..N-1 = 0, 1, 2, ...      (FC03 read, FC06/FC16 write)
    input registers   0..N-1 = 1000, 1001, ...   (FC04)
    coils / discrete inputs 0..N-1 = 0,1,0,1...  (FC01/FC02, FC05/FC15 write)
Addresses >= N answer exception 02 (illegal data address); unsupported
function codes answer exception 01. N = --regs (default 100).
"""
import argparse
import struct
import sys
import time

import serial


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(pdu: bytes) -> bytes:
    return pdu + struct.pack("<H", crc16(pdu))


def crc_ok(frame: bytes) -> bool:
    return len(frame) >= 4 and crc16(frame[:-2]) == struct.unpack("<H", frame[-2:])[0]


def request_length(buf: bytes):
    """Expected length of the request at the start of buf, or None if unknown yet."""
    if len(buf) < 2:
        return None
    fc = buf[1]
    if fc in (1, 2, 3, 4, 5, 6):
        return 8
    if fc in (15, 16):
        return 9 + buf[6] if len(buf) >= 7 else None
    return None


class Slave:
    def __init__(self, regs: int):
        self.n = regs
        self.hr = list(range(regs))
        self.ir = [1000 + i for i in range(regs)]
        self.coils = [i % 2 for i in range(regs)]

    def handle(self, sid: int, pdu: bytes) -> bytes:
        fc = pdu[0]

        def exc(code):
            return bytes([sid, fc | 0x80, code])

        if fc in (1, 2, 3, 4):
            if len(pdu) != 5:
                return exc(3)
            addr, qty = struct.unpack(">HH", pdu[1:5])
            limit = 2000 if fc in (1, 2) else 125
            if qty < 1 or qty > limit:
                return exc(3)
            if addr + qty > self.n:
                return exc(2)
            if fc in (1, 2):
                bits = self.coils[addr:addr + qty]
                data = bytearray((qty + 7) // 8)
                for i, b in enumerate(bits):
                    if b:
                        data[i // 8] |= 1 << (i % 8)
                return bytes([sid, fc, len(data)]) + bytes(data)
            table = self.hr if fc == 3 else self.ir
            vals = table[addr:addr + qty]
            return bytes([sid, fc, 2 * qty]) + struct.pack(">%dH" % qty, *vals)
        if fc == 5:
            addr, val = struct.unpack(">HH", pdu[1:5])
            if addr >= self.n:
                return exc(2)
            if val not in (0x0000, 0xFF00):
                return exc(3)
            self.coils[addr] = 1 if val else 0
            return bytes([sid]) + pdu[:5]
        if fc == 6:
            addr, val = struct.unpack(">HH", pdu[1:5])
            if addr >= self.n:
                return exc(2)
            self.hr[addr] = val
            return bytes([sid]) + pdu[:5]
        if fc == 15:
            addr, qty = struct.unpack(">HH", pdu[1:5])
            if addr + qty > self.n:
                return exc(2)
            data = pdu[6:]
            for i in range(qty):
                self.coils[addr + i] = (data[i // 8] >> (i % 8)) & 1
            return bytes([sid]) + pdu[:5]
        if fc == 16:
            addr, qty = struct.unpack(">HH", pdu[1:5])
            if qty < 1 or qty > 123 or pdu[5] != 2 * qty:
                return exc(3)
            if addr + qty > self.n:
                return exc(2)
            self.hr[addr:addr + qty] = struct.unpack(">%dH" % qty, pdu[6:6 + 2 * qty])
            return bytes([sid]) + pdu[:5]
        return exc(1)


def hexs(b: bytes) -> str:
    return b.hex(" ").upper()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--parity", choices="NEO", default="N")
    ap.add_argument("--stop", type=int, choices=(1, 2), default=1)
    ap.add_argument("--slave", type=int, action="append", help="slave ID to simulate (repeatable, default 1)")
    ap.add_argument("--regs", type=int, default=100, help="registers/coils per slave (default 100)")
    ap.add_argument("--delay", type=float, default=0.0, help="extra reply delay in ms (simulate a slow device)")
    ap.add_argument("--quiet", action="store_true", help="don't print every frame")
    a = ap.parse_args()

    ids = a.slave or [1]
    slaves = {sid: Slave(a.regs) for sid in ids}
    ser = serial.Serial(a.port, a.baud, bytesize=8, parity=a.parity, stopbits=a.stop, timeout=0)
    gap = max(0.020, 3.5 * 11 / a.baud)  # generous: USB adapters deliver bytes in bursts
    print("Simulating slave ID(s) %s on %s at %d 8%s%d - Ctrl+C to stop" % (
        ",".join(map(str, ids)), a.port, a.baud, a.parity, a.stop), flush=True)

    buf = b""
    last = time.monotonic()
    served = 0
    try:
        while True:
            data = ser.read(256)
            now = time.monotonic()
            if data:
                buf += data
                last = now
            want = request_length(buf)
            complete = want is not None and len(buf) >= want and crc_ok(buf[:want])
            if not complete and not (buf and now - last > gap):
                if not data:
                    time.sleep(0.0005)
                continue

            frame = buf[:want] if complete else buf
            buf = buf[len(frame):]
            if not crc_ok(frame):
                if not a.quiet:
                    print("  ignored (bad CRC / not a frame): %s" % hexs(frame), flush=True)
                continue
            sid = frame[0]
            if sid != 0 and sid not in slaves:
                if not a.quiet:
                    print("  not for us (ID %d): %s" % (sid, hexs(frame)), flush=True)
                continue
            targets = list(slaves.items()) if sid == 0 else [(sid, slaves[sid])]
            reply = None
            for tid, s in targets:
                reply = s.handle(tid, frame[1:-2])
            if sid == 0:
                if not a.quiet:
                    print("RX %s  (broadcast, no reply)" % hexs(frame), flush=True)
                continue
            out = with_crc(reply)
            if a.delay:
                time.sleep(a.delay / 1000.0)
            ser.write(out)
            ser.flush()
            served += 1
            if not a.quiet:
                print("RX %s  ->  TX %s" % (hexs(frame), hexs(out)), flush=True)
            elif served % 100 == 0:
                print("%d requests served" % served, flush=True)
    except KeyboardInterrupt:
        print("\n%d request(s) served" % served)
    return 0


if __name__ == "__main__":
    sys.exit(main())
