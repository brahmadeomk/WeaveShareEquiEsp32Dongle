# USB ⇄ RS485 (Modbus RTU) Dongle — Arduino Nano ESP32

Firmware that turns the **ESP32 Modbus Gateway board** (from
[brahmadeomk/Gateway](https://github.com/brahmadeomk/Gateway)) into a transparent
USB-to-RS485 converter, like a Waveshare *USB TO RS485* dongle. Plug it into a PC
and it shows up as a normal COM port (`COMx` on Windows, `/dev/ttyACM0` on
Linux, `/dev/cu.usbmodem*` on macOS). Any Modbus RTU master software — Modbus
Poll, QModMaster, pymodbus, SCADA drivers, VFD/meter setup tools — then talks
through it to the RS485 bus.

The hardware doesn't change. The firmware uses the same pins and the same
RS485 handling as the Gateway firmware.

## Hardware (same as the Gateway)

| Function | Pin(s) | Used by the dongle |
|---|---|---|
| RS485 (Modbus RTU) | RX = D2, TX = D3, DE/RE = D4 | **Yes**: Serial1, DE/RE controlled automatically |
| Cellular modem UART | D5 / D6 | No |
| Modem PWRKEY | A5 | Held at its idle level (HIGH), no power-on pulse |
| Digital Out 1–4 | D11, D12, D13, A4 | Driven LOW (off) so relays stay off |
| Digital In / Analog In | D7–D10 / A0–A3 | No |
| USB-C | — | PC link (USB CDC) |

## Features

- **Follows the host's port settings.** The baud rate, data bits (5–8), parity
  (N/E/O) and stop bits (1/2) that the PC application opens the COM port with
  are applied to the RS485 UART, the same way a CH343/FT232 dongle behaves. If
  the host never sets them, the port defaults to **9600 8N1**.
- **Automatic DE/RE direction control**, using the Gateway's proven timing:
  - a 300 µs driver settle after DE goes high
  - `flush()` waits until the last stop bit has been sent
  - DE is released straight away, inside a critical section, so the slave's
    reply is never cut off
- **Modbus T3.5 inter-frame silence** is enforced before each transmission.
  It's computed from the live baud rate with the same formulas as the Gateway.
- **Byte-transparent.** Works for Modbus RTU, Modbus ASCII and other
  half-duplex RS485 protocols.
- **Low latency.** The UART RX timeout is 1 character, so replies are passed to
  USB right away. If a request arrives split across USB packets, DE stays
  asserted until the whole request has been sent.
- **RGB LED status** (the USB port carries only data, never debug text):

  | LED | Meaning |
  |---|---|
  | White flash | Booted |
  | Blue flash | PC → RS485 transmit |
  | Green flash | RS485 → PC receive |
  | Red flash | USB RX overflow, or mark/space parity requested (falls back to none) |

## Build & flash

1. Arduino IDE 2.x → Boards Manager → install **Arduino ESP32 Boards**
   (`arduino:esp32`). The Espressif `esp32` core also works (2.0.x or 3.x, board
   `nano_nora`).
2. Select **Tools → Board → Arduino Nano ESP32**.
3. Leave **USB Mode** at its default (TinyUSB CDC). The *Debug / Hardware CDC*
   mode can't report the host's baud and parity, and the sketch stops compiling
   with an error if it's selected.
4. Set **Core Debug Level** to **None**. Otherwise the ESP-IDF logs could be
   mixed into the data stream on the USB port.
5. Open `UsbRs485Dongle/UsbRs485Dongle.ino` and upload. No external libraries
   are needed.

arduino-cli:

```sh
arduino-cli compile --fqbn arduino:esp32:nano_nora UsbRs485Dongle
arduino-cli upload  --fqbn arduino:esp32:nano_nora -p /dev/ttyACM0 UsbRs485Dongle
```

To go back to the Gateway, flash the Gateway sketch the same way. Both
firmwares run on the same board without any changes to it.

## Configuration (top of the sketch)

| Define | Default | Meaning |
|---|---|---|
| `FOLLOW_HOST_LINE_CODING` | 1 | 0 = always use the `DEFAULT_*` settings below and ignore the PC's settings |
| `DEFAULT_BAUD` / `_DATA_BITS` / `_PARITY` / `_STOP_BITS` | 9600 8N1 | Settings used at boot, or always when the define above is 0 |
| `DE_SETTLE_US` | 300 | Driver-enable settle time before the first bit |
| `ENFORCE_T35_GAP` | 1 | Wait for Modbus T3.5 bus silence before transmitting |
| `ALLOW_1200BAUD_REBOOT` | 1 | Opening the port at **1200 baud** reboots the board into the bootloader (the Arduino upload "touch"). Set to 0 if your bus really runs at 1200 baud; then use the double-tap-reset bootloader for uploads. |
| `MIN_BAUD` / `MAX_BAUD` | 300 / 2 000 000 | Host baud rates outside this range are ignored |

## Testing

`tools/modbus_rtu_test.py` sends a read-registers request through the dongle
and decodes the reply. It only needs `pyserial`:

```sh
pip install pyserial
python tools/modbus_rtu_test.py COM5 --baud 9600 --parity N --slave 1 --fc 3 --addr 0 --count 2 --repeat 10
```

The full bench test procedure, with the pass criteria and a results sheet, is in
[`docs/BENCH_TEST.md`](docs/BENCH_TEST.md).

If you get no response, check these first: A/B swapped, the slave ID, the baud
rate and parity, and the 120 Ω termination at the ends of a long bus.

## Differences from a Waveshare dongle

- Mark and space parity aren't supported by the ESP32 UART, so they fall back
  to no parity (red LED flash).
- 1.5 stop bits is rounded to 2.
- The DTR/RTS modem lines are ignored, apart from the 1200-baud reboot
  described above.
- There's no on-board isolation or TVS protection beyond what your Gateway
  board's transceiver stage already has.
