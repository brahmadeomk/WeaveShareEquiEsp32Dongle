#!/usr/bin/env python3
"""Quick end-to-end test for the USB-RS485 dongle.

Sends a Modbus RTU "Read Holding/Input Registers" request through the
dongle's COM port and prints the decoded reply. Needs only pyserial:

    pip install pyserial
    python modbus_rtu_test.py COM5 --baud 9600 --parity N --slave 1 --addr 0 --count 2
    python modbus_rtu_test.py /dev/ttyACM0 --baud 19200 --parity E --fc 4
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--parity", choices="NEO", default="N")
    ap.add_argument("--stop", type=int, choices=(1, 2), default=1)
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--fc", type=int, choices=(3, 4), default=3)
    ap.add_argument("--addr", type=int, default=0)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=1.0)
    ap.add_argument("--repeat", type=int, default=1)
    a = ap.parse_args()

    ser = serial.Serial(a.port, a.baud, bytesize=8, parity=a.parity,
                        stopbits=a.stop, timeout=a.timeout)
    time.sleep(0.1)  # let the dongle apply the new line coding

    req = struct.pack(">BBHH", a.slave, a.fc, a.addr, a.count)
    req += struct.pack("<H", crc16(req))
    expected = 5 + 2 * a.count
    ok = 0

    for i in range(a.repeat):
        ser.reset_input_buffer()
        t0 = time.perf_counter()
        ser.write(req)
        rx = ser.read(expected)
        dt = (time.perf_counter() - t0) * 1000

        print(f"[{i + 1}] TX {req.hex(' ')}")
        print(f"    RX {rx.hex(' ') or '(nothing)'}  ({dt:.1f} ms)")

        if len(rx) == 5 and rx[1] & 0x80:
            print(f"    slave exception code {rx[2]}")
            continue
        if len(rx) != expected or crc16(rx[:-2]) != struct.unpack("<H", rx[-2:])[0]:
            print("    BAD/NO RESPONSE (check wiring, A/B polarity, baud, slave id)")
            continue
        regs = struct.unpack(f">{a.count}H", rx[3:-2])
        print(f"    registers {a.addr}..{a.addr + a.count - 1}: {list(regs)}")
        ok += 1

    ser.close()
    print(f"{ok}/{a.repeat} OK")
    return 0 if ok == a.repeat else 1


if __name__ == "__main__":
    sys.exit(main())
