> **Technicians:** use the short [`QUICK_TEST.md`](QUICK_TEST.md) instead. This document is
> the full engineering procedure.

# Bench Test Procedure: USB ⇄ RS485 Dongle (Arduino Nano ESP32)

This procedure checks the `UsbRs485Dongle` firmware (v1.2: converter + WiFi diagnostics + auto-scan + sniffer) on the Gateway board hardware
before it's used in the field. Run the tests in order, because later tests assume
the earlier ones passed. Record the results in the table in §13.

Testing from a Raspberry Pi? Follow [`RASPBERRY_PI_SOP.md`](RASPBERRY_PI_SOP.md):
it carries out these tests step by step from a Pi terminal.

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
| 5 | Twisted pair for A/B (+ GND), 120 Ω resistor for the sensor end | Bus wiring and termination (the dongle end's 120 Ω is on the board) |
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
                 DONGLE END                                  SENSOR (FAR) END
 PC ──USB── DUT (Gateway board)                              Slave / last sensor
            [120 Ω on board] A ──────── twisted pair ──────── A ┐
                             B ──────────────────────────────── B ┴ [120 Ω: CHECK THIS]
                           GND ──────────────────────────────── GND
 Dongle end: 120 Ω already fitted.   Sensor end: fit 120 Ω across A–B, or switch
                                     on the device's termination DIP switch.
 Devices in between (if any): termination OFF.
```

Logic analyser (T6): CH1 = **D3 (TX)**, CH2 = **D4 (DE/RE)**, CH3 = **D2 (RX)**, GND = board GND.

---

## 2. Pre-power checks

| # | Check | Pass criterion |
|---|---|---|
| P1 | Visual: transceiver, A/B screw terminals, no solder bridges near D2–D4 | OK |
| P2 | Relay/DO loads (D11, D12, D13, A4) connected as in the field, or disconnected | Recorded |
| P3 | Modem board (if fitted) | Either state is fine; the firmware won't pulse PWRKEY |
| P4 | Bias resistors: **330 Ω** A to +5 V and **330 Ω** B to GND, fitted at one point on the bus only | Fitted as specified |
| P4b | **Sensor-end termination.** The dongle end has 120 Ω on the board. Check that the last device at the far end has 120 Ω across A-B (resistor or DIP switch) and that no device in the middle is terminated. With **everything powered off**, measure the resistance A-B at the dongle | **About 50-65 Ω.** About 100-120 Ω means the sensor-end termination is missing; below 45 Ω means too many terminators. Record: ______ Ω |
| P5 | **After power-on, with no traffic:** measure A to B with a multimeter (red lead on A, black lead on B) | **Pass: +0.3 V to +0.8 V** (about +0.7 V measured on our setup). +0.2 to +0.3 V is marginal: check for extra bias or termination. Below +0.2 V means bias missing; negative means A/B swapped; above +1 V means termination missing. Record the reading: ______ V |

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

## 8. Web page and login

Connect a phone or laptop to the dongle's WiFi and open **http://192.168.4.1**.

| Test | Steps | Pass criterion |
|---|---|---|
| **W1 AP visible** | Scan for WiFi networks after boot | **ModbusDongle** is listed. It connects with password **12345678** |
| **W2 Login required** | Open `http://192.168.4.1/` while logged out | Redirected to the login page |
| **W3 Default login** | Log in with admin / admin | Diagnostics page opens with the red *Default web login in use* banner |
| **W4 Change password** | Settings → Security: set a new password (≥ 4 characters, typed twice) and save. Log out, then log in with the new password | Saved message shown. Old password rejected, new one accepted. Banner gone |
| **W5 Lockout** | Log out and enter a wrong password 5 times | The 5th attempt shows *locked for 60 seconds*. The correct password is refused until 60 s have passed |
| **W6 Session timeout** | Log in, leave the page closed for 31 min, then reload | Back at the login page |
| **W7 Live refresh** | Run F1 with `--repeat 100` while watching the Diagnostics page | Requests and OK counters rise every 2 s. The Slave 1 row and *Recent transactions* fill in. Health = **Bus healthy** |
| **W8 No impact on bridging** | Repeat R1 (5000 requests) with the Diagnostics page open and refreshing | Same pass criterion as R1. The page counters match the script (requests = 5000, OK = script OK count) |
| **W9 Site WiFi (optional)** | Settings → Site WiFi: enter your network and save | Within about 30 s the Settings page shows *connected, IP x.x.x.x*. The page also opens at that IP. The AP keeps working |
| **W10 AP settings** | Change the AP name and password, then reconnect | The new AP name and password work. A password shorter than 8 characters is rejected |
| **W11 Settings survive reboot** | Settings → Reboot, wait, log in | Password, AP and diagnostic settings are unchanged |
| **W12 Credential recovery** | Close the master software. Open the COM port at **74880** baud in a serial terminal and type `status`, then `factory-reset-auth` | `status` prints the AP name, IP and counters. The reset prints *AUTH RESET OK* and reboots. Login is admin/admin and the AP password is 12345678 again. Diagnostic settings and site WiFi are kept |
| **W13 Console doesn't bridge** | While in 74880 console mode, watch the RS485 TX line or the slave | Nothing is transmitted on RS485 |

---

## 9. Diagnosis tests (deliberate faults)

Start each test with **Reset statistics** on the Diagnostics page. Then create
the fault, poll with F1 (`--repeat 20` unless stated) and read the **findings**
at the top of the page. The expected verdict is the minimum. Other findings
caused by the same fault are acceptable.

For every finding, also check that the page shows a **Likely cause**, a
numbered **Remedy** list and, where relevant, an action button. Then apply
the remedy, press **Reset statistics**, poll again and confirm that the
finding clears.

| Test | Fault to create | Expected finding (verdict) |
|---|---|---|
| **D1 Healthy baseline** | None. F1 with `--repeat 50` | **Bus healthy** (OK), success 100 %, plus a recommended master timeout. Idle level **HIGH (correct)** |
| **D2 Wrong slave ID** | Poll `--slave 77` (no such device) | **Slave 77 never answers** (FAULT), with the wiring/ID/settings checklist |
| **D3 Wrong baud rate** | Slave at 19200, master opens at 9600 | **Slave N never answers** or **every reply is corrupted** (FAULT), mentioning baud/parity mismatch. Usually also **UART framing/parity errors** (WARN) |
| **D4 Wrong parity** | Slave at 9600 E1, master at 9600 N1 | As D3. The finding names the PC's port settings (`9600 8N1`) |
| **D5 A/B swapped** | Swap the A and B wires at the dongle (bus with 330 Ω bias) | A-B at the dongle reads **negative** (about -0.3 to -0.8 V). **RS485 bus idles LOW** (FAULT) within about 3 s of the bus going quiet. Polls fail. Restore the wiring: A-B reads +0.3 to +0.8 V again and the idle level goes back to HIGH (Reset statistics to clear old findings) |
| **D5b Bias removed** (optional) | Disconnect the 330 Ω bias resistors | A-B reads near 0 V. **Idle line level unstable** or **RS485 bus idles LOW**, sometimes with **Stray byte in front of replies**. Refit the bias: A-B reads +0.3 to +0.8 V again |
| **D6 Device stops** | Poll continuously (`--repeat 200`), then power off the slave halfway | **Slave N stopped responding** (FAULT), showing how many requests succeeded first |
| **D7 Illegal address** | `--addr 9999` (or any register the device doesn't have) | **Illegal data address (exception 02)** (WARN), with the 0-based / 4xxxx numbering hint |
| **D8 Unsupported function** | Poll with a function code the device lacks (e.g. FC04 on a holding-register-only device) | **Function not supported (exception 01)** (WARN) |
| **D9 Second master** | Connect a second master (e.g. another USB-RS485 adapter running Modbus Poll) to the same bus and let it poll while the dongle is idle | **Another device is talking on the bus** (WARN) |
| **D10 Slow slave / short timeout** | Set Settings → Diagnostics reply timeout to 20 ms and poll a slave that answers in > 20 ms | **LATE REPLY** entries appear in *Recent transactions*. If every reply is late: **every reply arrives too late** (FAULT). If only some are late: **intermittent timeouts** (WARN). Either way it recommends a longer master timeout. Set the timeout back to 1000 ms afterwards |
| **D11 Noise / termination** (optional) | Long cable with the **sensor-end** 120 Ω removed, or a noise source next to the cable | **Corrupted replies** or **Noise between frames** (WARN), with the termination/shielding advice |
| **D12 Non-RTU traffic** | Send ASCII text from a serial terminal at 9600 (e.g. `hello`) | **PC traffic is not Modbus RTU** (INFO) |
| **D13 LED verdict** | During D2 (FAULT) and D7 (WARN), with no polling running | Red blink every 2 s (FAULT), yellow blink every 2 s (WARN) |

---

## 10. Auto-scan tests

Stop the Modbus master software before each scan (closing the COM port is enough).

| Test | Steps | Pass criterion |
|---|---|---|
| **S1 Find a known device** | Slave at 9600 8N1, ID 1. Auto-scan with the defaults (IDs 1–247, 9600/19200/38400/115200 × 8N1/8E1, smart stop on) | ID 1 is listed at **9600 8N1** within about 30 s. The scan stops after that setting. The page suggests *Use these port settings: 9600 8N1* |
| **S2 Find unknown settings** | Set the slave to 19200 8E1, ID 23 (don't tell the scanner). Run S1's scan again | ID 23 found at **19200 8E1**. The earlier settings may be listed under *Garbled replies at* |
| **S3 Several devices** | Two or more slaves with different IDs, same settings | All IDs are found in one scan |
| **S4 Exception counts as found** | Probe an address the device doesn't have (e.g. address 9999) | The device is still listed, with *Exception 2 (device present)* |
| **S5 Stop button** | Start a full-range scan and press **Stop** after about 10 s | The state changes to STOPPED within about 1 s and the results so far are kept |
| **S6 Nothing on the bus** | Disconnect A/B and scan IDs 1–10 | *No device answered* with the wiring checklist |
| **S7 PC settings restored** | After S1, open the master at 9600 8N1 and run F1 | F1 passes. The Diagnostics page shows the PC's settings again |
| **S8 PC data during scan** | Start a scan, then run F1 against the dongle while it scans | F1 gets no replies. After the scan, the Diagnostics page shows *PC data was discarded during a scan* (INFO). Nothing from the PC was sent on the bus |
| **S9 Validation** | Try ID range 0–300, a timeout of 5 ms, and no baud rate selected | Each is refused with a clear message. No scan starts |

---

## 11. Sniffer tests

You need a second master that polls the slave: another USB-RS485 adapter
running Modbus Poll or QModMaster, or a PLC. Connect the dongle to the same A/B
pair. **The dongle's own COM port doesn't need to be open.**

| Test | Steps | Pass criterion |
|---|---|---|
| **SN1 Listen-only** | Start the second master polling (FC03, 1 s interval). On the dongle open **Sniffer**, choose the bus settings manually and press **Start sniffing**. Watch the second master's error counter for 2 min | The second master shows **no new errors** while the dongle sniffs. The LED blinks cyan |
| **SN2 Decoding** | As SN1 | MASTER and SLAVE rows alternate for each poll. Details show the address and quantity, and the reply values with their addresses, matching what the second master displays. Gap values match the poll interval |
| **SN3 Never transmits** | As SN1, with a logic analyser or scope on **D4 (DE/RE)**. Also send data from the PC to the dongle's COM port | D4 stays **LOW** for the whole session. The status line counts the PC bytes as discarded. Nothing from the PC appears on the bus |
| **SN4 Auto-detect** | Set the slave and second master to 19200 8E1. Start the sniffer with **Auto-detect** | The status changes from *Auto-detecting...* to **Sniffing at 19200 8E1 (auto-detected)** within about 10 s |
| **SN5 Auto-detect without traffic** | Stop the second master, start Auto-detect, wait 40 s | It keeps cycling (*pass 2, 3...*) and doesn't lock onto a setting. Start the master: it locks within one pass |
| **SN6 Exceptions and writes** | From the second master, read a register that doesn't exist, then write a register (FC06) and several registers (FC16) | The exception row is yellow with *Exception 2 Illegal Data Address*. Write requests show the values. Replies show *OK (write confirmed)* |
| **SN7 Passive diagnosis** | As SN1, add a poll to a slave ID that doesn't exist. Open **Diagnostics** | *Sniffer mode (listen-only)* is shown, and **Slave N never answers** with its remedy. The working slave shows a 100 % success rate and its latency |
| **SN8 Copy to PC** | Tick *Copy the raw bus traffic*, open the dongle's COM port in a serial terminal (hex view) at any baud rate | The terminal shows the raw request and reply bytes |
| **SN9 CSV** | After 1 min of SN1 press **Download CSV** | The file opens in a spreadsheet with one row per frame (seq, time, gap, direction, ID, function, details, raw hex) |
| **SN10 Stop and return** | Press **Stop**, stop the second master, then run F1 through the dongle | F1 passes: the dongle bridges normally again with the PC's port settings |
| **SN11 Exclusive with scan** | While sniffing, try to start Auto-scan | Refused with *The sniffer is running - stop it on the Sniffer page first* |

---

## 12. LED behaviour

| Test | Condition | Expected LED |
|---|---|---|
| **E1** | Power-up | Short white flash |
| **E2** | F1 running | Blue flash (TX), then green flash (RX) on each poll |
| **E3** | Slave disconnected, F1 running | Blue flashes only |
| **E4** | Open the port with **mark** parity (e.g. pyserial `parity='M'`) | Red flash about 0.5 s. Traffic continues with no parity |
| **E5** | Diagnosis verdict FAULT (e.g. D2), bus quiet | Red blink every 2 s |
| **E6** | Diagnosis verdict WARN (e.g. D7), bus quiet | Yellow blink every 2 s |
| **E7** | Auto-scan running | Purple blink every 1 s, plus blue/green flashes per probe |
| **E8** | Sniffer running | Cyan blink every 1 s, plus a green flash per received frame. Never blue (no transmit) |

---

## 13. Results record

| Test | Result (Pass / Fail / N/A) | Notes / measured values |
|---|---|---|
| P4b Termination (A-B resistance, power off) | | Ω: |
| P5 Idle A-B voltage | | V: |
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
| W1–W13 Web / login | | Failed: |
| D1–D13 Diagnosis | | Failed: |
| S1–S9 Auto-scan | | Failed: |
| SN1–SN11 Sniffer | | Failed: |
| E1–E8 LEDs | | |

**Overall verdict:** ☐ PASS  ☐ FAIL. Signed: __________  Date: __________

---

## 14. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Sketch doesn't compile: *"Select the Arduino Nano ESP32 with its default USB mode"* | **Tools → USB Mode** is set to Debug/Hardware CDC. Change it back to the default |
| Garbage or text appears on the port with no traffic (T5 fails) | Core Debug Level isn't **None**. Rebuild with it set to None |
| Every request times out, but the blue LED flashes | A/B swapped, slave ID/baud/parity mismatch, missing GND reference, or DE/RE (D4) not wired to the transceiver |
| Fails only at low baud with "first byte missing" | Check T6b. DE must drop right after the last stop bit |
| Fails only at high baud | Cable length, or the sensor-end 120 Ω termination missing (check P4b), or the transceiver's rated speed (MAX485 ≈ 2.5 Mbps max, some clones much lower) |
| CRC errors that come and go under load (R1/R2) | Sensor-end termination (P4b) or biasing missing, ground loop, or noisy cable. Retest with a short bench cable |
| Idle level LOW or UNSTABLE on the page | Measure A-B at idle: it should be +0.3 to +0.8 V with the 330 Ω bias. Negative means A/B swapped; near 0 V means the bias is missing or unpowered |
| Sniffer shows only `?` garbage rows | Wrong baud rate or parity: restart with Auto-detect. If it never locks, check A/B polarity at the dongle and that the master is really polling |
| Sniffer shows MASTER rows but no SLAVE rows | The slaves aren't answering the other master. Check *Diagnostics* for the per-slave findings and remedies |
| Web page doesn't open | Check you are connected to the **ModbusDongle** WiFi and browsing to `http://192.168.4.1` (not https). Mobile phones may drop a WiFi network that has no internet access: turn off mobile data or tell the phone to stay connected |
| Forgot the web or AP password | Open the COM port at **74880** baud and send `factory-reset-auth` (W12) |
| Findings from an earlier fault still shown | Counters are cumulative: press **Reset statistics** after fixing a fault |
| Every poll shows TIMEOUT on the page but the master gets replies | The diagnostic reply timeout is shorter than the slave's reply time. Raise it under Settings → Diagnostics |
| Board vanishes when the port opens at 1200 baud | Expected: the 1200-baud bootloader touch (see L-note) |
| Can't upload any more | Double-tap the reset button to enter the bootloader, then upload |
