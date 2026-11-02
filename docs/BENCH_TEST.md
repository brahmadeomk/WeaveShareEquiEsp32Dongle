# Bench Test Procedure: USB ⇄ RS485 Dongle (Arduino Nano ESP32)

This procedure checks the `UsbRs485Dongle` firmware on the Gateway board hardware
before it's used in the field. Run the tests in order, because later tests assume
the earlier ones passed. Record the results in the table in §9.

| Item | Value |
|---|---|
| Firmware commit | `git rev-parse --short HEAD` → ________ |
| Board serial / ID | ________ |
| Arduino core + version | e.g. `arduino:esp32` 2.0.18 → ________ |
| Tester / date | ________ |

---

## 1. Equipment

| # | Item | Purpose |
|---|---|---|
| 1 | Gateway board (Nano ESP32 + RS485 transceiver on D2/D3/D4) | Device under test (DUT) |
| 2 | USB-C data cable | PC link |
| 3 | PC with Python 3 and `pip install pyserial pymodbus` | Master side |
| 4 | **One** Modbus RTU slave. Either a real device (energy meter, VFD, PLC) **or** a second USB-RS485 adapter (e.g. Waveshare) running the slave simulator in §1.1 | Bus partner |
| 5 | Twisted pair for A/B (+ GND), 120 Ω resistor | Bus wiring and termination |
| 6 | Logic analyser or 2-channel scope (optional, needed for test T6) | Turnaround timing |
| 7 | Modbus Poll / QModMaster (optional) | GUI cross-check |

### 1.1 Slave simulator (if you have no real slave)

Connect a second USB-RS485 adapter to the same A/B pair and run this on the PC:

```sh
# Serves holding registers 0..99 = 0,1,2,... and input registers 0..99 = 1000,1001,...
python - <<'EOF'
import sys
from pymodbus.server import StartSerialServer
from pymodbus.datastore import (ModbusSequentialDataBlock, ModbusSlaveContext,
                                ModbusServerContext)
port, baud, parity = sys.argv[1] if len(sys.argv) > 1 else "COM6", 9600, "N"
store = ModbusSlaveContext(hr=ModbusSequentialDataBlock(0, list(range(200))),
                           ir=ModbusSequentialDataBlock(0, [1000 + i for i in range(200)]))
StartSerialServer(context=ModbusServerContext(slaves={1: store}, single=False),
                  port=port, baudrate=baud, parity=parity, stopbits=1, bytesize=8)
EOF
```

To use a different baud rate or parity, change `baud` and `parity` in the script
and restart it. Its settings must match what the master opens the DUT with. (The
`pymodbus` 3.x API is shown. On older versions the import names differ slightly.)

### 1.2 Wiring

```
 PC ──USB── DUT (Gateway board)  A ───────┬──────── A   Slave / 2nd adapter
                                 B ───────┼──────── B
                                GND ──────┼──────── GND
                                       120 Ω across A–B at the far end (and one at the DUT
                                       if the bus is long; not needed for <2 m bench cable)
```

Logic analyser (T6): CH1 = **D3 (TX)**, CH2 = **D4 (DE/RE)**, CH3 = **D2 (RX)**, GND = board GND.

---

## 2. Pre-power checks

| # | Check | Pass criterion |
|---|---|---|
| P1 | Visual: transceiver, A/B screw terminals, no solder bridges near D2–D4 | OK |
| P2 | Relay/DO loads (D11, D12, D13, A4) connected as in the field, or disconnected | Recorded |
| P3 | Modem board (if fitted) | Either state is fine; the firmware won't pulse PWRKEY |

---

## 3. Flash and enumeration

| Test | Steps | Pass criterion |
|---|---|---|
| **T1 Flash** | Arduino IDE: Board = *Arduino Nano ESP32*, USB Mode = default, Core Debug Level = **None** → Upload `UsbRs485Dongle.ino` | Compiles with no errors and uploads |
| **T2 Boot** | Unplug, then replug USB | RGB LED shows a short **white flash**, then stays off |
| **T3 Enumeration** | Windows: Device Manager → Ports. Linux: `ls /dev/ttyACM*`. macOS: `ls /dev/cu.usbmodem*` | One new COM/ttyACM port appears |
| **T4 Safe outputs** | Measure D11, D12, D13, A4 to GND; watch the relays during boot | All about 0 V. No relay clicks at any point during boot |
| **T5 Quiet port** | Open the port at 9600 in a serial terminal and send nothing for 10 s | **No bytes** are received (no boot text or debug output) |

---

## 4. Functional Modbus tests

The commands below use `tools/modbus_rtu_test.py`. Replace `COM5` with the DUT's
port. Each run prints `n/n OK` when it passes.

| Test | Command / steps | Pass criterion |
|---|---|---|
| **F1 Read holding (FC3)** | `python tools/modbus_rtu_test.py COM5 --baud 9600 --slave 1 --fc 3 --addr 0 --count 2 --repeat 10` | `10/10 OK`. Values match the slave (simulator: `[0, 1]`). **Blue** LED flash, then **green** |
| **F2 Read input (FC4)** | `... --fc 4 --addr 0 --count 4 --repeat 10` | `10/10 OK` (simulator: `[1000..1003]`) |
| **F3 Exception** | `... --fc 3 --addr 9999 --count 1` | Prints `slave exception code 2` (illegal address). The DUT passes the exception through unchanged |
| **F4 Wrong slave ID** | `... --slave 77` | `BAD/NO RESPONSE` after a 1 s timeout. The next F1 run passes again straight away |
| **F5 Large frame** | `... --count 125 --repeat 10` (255-byte reply) | `10/10 OK` |
| **F6 GUI cross-check** | Modbus Poll / QModMaster: RTU on the DUT's COM port, poll 10 holding registers every 100 ms for 2 min | 0 errors or timeouts in the tool's counters |
| **F7 Write (FC6/FC16)** | In the GUI tool, write a value to holding register 10 (FC6), then a block of 10 registers (FC16), and read them back | The values read back match what was written |

---

## 5. Line-coding (port settings) tests

These tests check that the DUT copies the PC's settings onto the RS485 UART. For
each row, set the **slave** to the same settings first, then run F1 with matching
arguments, `--repeat 20`.

| Test | Baud | Parity | Stop | Script args | Pass |
|---|---|---|---|---|---|
| **L1** | 1200* | N | 1 | *see L-note* | ☐ |
| **L2** | 2400 | N | 1 | `--baud 2400` | ☐ |
| **L3** | 4800 | E | 1 | `--baud 4800 --parity E` | ☐ |
| **L4** | 9600 | N | 1 | `--baud 9600` | ☐ |
| **L5** | 9600 | E | 1 | `--baud 9600 --parity E` | ☐ |
| **L6** | 9600 | O | 1 | `--baud 9600 --parity O` | ☐ |
| **L7** | 9600 | N | 2 | `--baud 9600 --stop 2` | ☐ |
| **L8** | 19200 | E | 1 | `--baud 19200 --parity E` | ☐ |
| **L9** | 38400 | N | 1 | `--baud 38400` | ☐ |
| **L10** | 57600 | N | 1 | `--baud 57600` | ☐ |
| **L11** | 115200 | N | 1 | `--baud 115200` | ☐ |
| **L12** | 230400 | N | 1 | `--baud 230400` (only if the slave supports it) | ☐ |

Pass criterion for each row: `20/20 OK`.

**Negative check L13:** with the slave at 9600 E1, run the master at 9600 **N**1.
The test must fail (no response or a CRC error). This confirms that parity really
reaches the wire.

**Switch check L14:** run L4, then L8, then L4 again, back-to-back without
unplugging. All three runs must pass. This confirms the DUT reconfigures on every
port open.

**L-note (1200 baud):** with `ALLOW_1200BAUD_REBOOT 1` (the default), opening the
port at 1200 baud reboots the board into the bootloader. **Expected result:** the
COM port disappears or re-enumerates, and you see the white boot flash after you
re-flash or power-cycle. Record "rebooted as designed". To test real 1200-baud
traffic, rebuild with `ALLOW_1200BAUD_REBOOT 0` and run
`--baud 1200 --timeout 2 --repeat 10`.

---

## 6. Timing tests (logic analyser)

Capture CH1 TX (D3), CH2 DE (D4) and CH3 RX (D2) during one F1 transaction at
9600 and at 115200 baud.

| Test | Measure | Pass criterion |
|---|---|---|
| **T6a DE lead** | DE rising edge → first TX start bit | about 300 µs (`DE_SETTLE_US`), between 250 and 500 µs |
| **T6b DE release** | Last TX stop bit end → DE falling edge | **< 50 µs**. It must be well before the slave's reply starts |
| **T6c No gap in frame** | Gaps between characters within the TX frame | No gap > 1.5 characters (9600: 1.72 ms; 115200: 750 µs) |
| **T6d Reply intact** | RX line vs. reply printed by the script | All reply bytes are captured. The first byte isn't clipped |
| **T6e T3.5 gap** | Run `--repeat 50 --timeout 0.2` and measure end of reply (RX) → next DE rise | ≥ 3.5 characters (9600: ≥ 4.0 ms; >19200 baud: ≥ 1.75 ms) |

If the analyser can't capture this, record the scope screenshots or "N/A".

---

## 7. Robustness / soak

| Test | Steps | Pass criterion |
|---|---|---|
| **R1 Soak** | `python tools/modbus_rtu_test.py COM5 --baud 19200 --parity E --count 10 --repeat 5000 --timeout 0.5` (about 10 min) | **≥ 99.9 %** OK (≤ 5 failures). Ideally 5000/5000 |
| **R2 Fast poll** | GUI tool polling every 20 ms (or as fast as the slave allows) for 10 min | 0 CRC errors. Any timeouts are attributable to the slave |
| **R3 Port close/reopen** | Close and reopen the COM port 20 times in the GUI tool, polling between each | Polling resumes every time with no replug |
| **R4 Host closed while bus active** | Start R2, close the GUI tool mid-poll, wait 30 s, reopen | Device recovers. No replug needed |
| **R5 USB hot-plug** | Unplug and replug USB 10 times while the slave stays powered | Enumerates every time. F1 passes after each replug |
| **R6 Bus disconnected** | Pull the A/B wires and run F1. Reconnect and run F1 again | Times out cleanly while disconnected, then passes once reconnected. No replug needed |
| **R7 A/B swapped** | Swap A and B and run F1. Restore the wiring | Fails while swapped (no damage), then passes once restored |
| **R8 Thermal** | After R1, touch-check the transceiver and the ESP32 | Not hot (< 50 °C) |

---

## 8. LED behaviour

| Test | Condition | Expected LED |
|---|---|---|
| **E1** | Power-up | Short white flash |
| **E2** | F1 running | Blue flash (TX), then green flash (RX) on each poll |
| **E3** | Slave disconnected, F1 running | Blue flashes only |
| **E4** | Open the port with **mark** parity (e.g. pyserial `parity='M'`) | Red flash about 0.5 s. Traffic continues with no parity |

---

## 9. Results record

| Test | Result (Pass / Fail / N/A) | Notes / measured values |
|---|---|---|
| T1 Flash | | |
| T2 Boot | | |
| T3 Enumeration | | Port name: |
| T4 Safe outputs | | |
| T5 Quiet port | | |
| F1 FC3 | | |
| F2 FC4 | | |
| F3 Exception | | |
| F4 Wrong slave | | |
| F5 Large frame | | |
| F6 GUI poll | | Errors: |
| F7 Write | | |
| L1–L12 | | Failed rows: |
| L13 Parity negative | | |
| L14 Switch | | |
| T6a DE lead | | µs: |
| T6b DE release | | µs: |
| T6c Frame gaps | | |
| T6d Reply intact | | |
| T6e T3.5 gap | | ms: |
| R1 Soak | | ___ / 5000 |
| R2 Fast poll | | |
| R3 Reopen | | |
| R4 Host closed | | |
| R5 Hot-plug | | |
| R6 Bus disconnect | | |
| R7 A/B swap | | |
| R8 Thermal | | |
| E1–E4 LEDs | | |

**Overall verdict:** ☐ PASS  ☐ FAIL. Signed: __________  Date: __________

---

## 10. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Sketch doesn't compile: *"Select the Arduino Nano ESP32 with its default USB mode"* | **Tools → USB Mode** is set to Debug/Hardware CDC. Change it back to the default |
| Garbage or text appears on the port with no traffic (T5 fails) | Core Debug Level isn't **None**. Rebuild with it set to None |
| Every request times out, but the blue LED flashes | A/B swapped, slave ID/baud/parity mismatch, missing GND reference, or DE/RE (D4) not wired to the transceiver |
| Fails only at low baud with "first byte missing" | Check T6b. DE must drop right after the last stop bit |
| Fails only at high baud | Cable length or termination, or the transceiver's rated speed (MAX485 ≈ 2.5 Mbps max, some clones much lower) |
| CRC errors that come and go under load (R1/R2) | Termination or biasing missing, ground loop, or noisy cable. Retest with a short bench cable |
| Board vanishes when the port opens at 1200 baud | Expected: the 1200-baud bootloader touch (see L-note) |
| Can't upload any more | Double-tap the reset button to enter the bootloader, then upload |
