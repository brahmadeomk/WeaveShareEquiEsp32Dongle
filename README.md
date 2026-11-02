# USB ⇄ RS485 (Modbus RTU) Dongle with Bus Diagnostics — Arduino Nano ESP32

Firmware that turns the **ESP32 Modbus Gateway board** (from
[brahmadeomk/Gateway](https://github.com/brahmadeomk/Gateway)) into a transparent
USB-to-RS485 converter, like a Waveshare *USB TO RS485* dongle. Plug it into a PC
and it shows up as a normal COM port (`COMx` on Windows, `/dev/ttyACM0` on
Linux, `/dev/cu.usbmodem*` on macOS). Any Modbus RTU master software — Modbus
Poll, QModMaster, pymodbus, SCADA drivers, VFD/meter setup tools — then talks
through it to the RS485 bus.

Unlike a plain dongle, it also **watches every transaction it carries and
explains what is wrong with the bus** on a WiFi web page. It can also
**auto-scan** for devices whose baud rate, parity or slave ID you don't know.

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
| WiFi | Onboard | Access Point for the diagnostics page, plus optional site WiFi |
| USB-C | — | PC link (USB CDC) |

## Converter features

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
  half-duplex RS485 protocols. The diagnostics only **observe** the traffic:
  nothing is changed, delayed, retried or filtered.
- **Low latency.** Replies are passed to USB as soon as the bytes arrive. If a
  request arrives split across USB packets, DE stays asserted until the whole
  request has been sent.
- **Same task layout as the Gateway.** The RS485 bridge runs alone on core 1.
  WiFi and the web server run on core 0, so web activity can never delay the
  bus.
- **RGB LED status** (the USB port carries only data, never debug text):

  | LED | Meaning |
  |---|---|
  | White flash | Booted |
  | Blue flash | PC → RS485 transmit |
  | Green flash | RS485 → PC receive |
  | Red blink every 2 s | Diagnosis verdict FAULT |
  | Yellow blink every 2 s | Diagnosis verdict WARN |
  | Purple blink every 1 s | Auto-scan running |
  | Red flash | USB RX overflow, or mark/space parity requested (falls back to none) |

## Diagnostics (WiFi page)

### 1. Transaction statistics
Every request the PC sends is checked as a Modbus RTU frame. The reply that
follows is framed by the Modbus length rules for that function code, and the
result is classified:

| Result | Meaning |
|---|---|
| OK | Valid reply from the same slave with the same function code |
| EXCEPTION | Valid exception reply. The code (01 Illegal Function, 02 Illegal Data Address, ...) is recorded |
| TIMEOUT | No reply within the diagnostic reply timeout, or before the PC sent its next request |
| CRC/LENGTH | Bytes came back, but never as a valid frame |
| WRONG ID | A valid frame came back from a different slave ID |
| LATE REPLY | A valid reply arrived after the request had already been counted as timed out |
| FOREIGN | A valid frame that nobody here asked for (another master on the bus) |
| NOT RTU | A PC request with no valid RTU CRC (Modbus ASCII or another protocol) |

**Per slave:** requests, OK, success %, timeouts, CRC errors, exception codes,
wrong-ID and late replies. Also the response latency (min / avg / max, about 1 ms
resolution) and the time since the last reply.

**Bus-wide:** noise bytes and stray lead bytes, UART framing, parity and break
errors, overflows, and byte counts. The last 20 transactions are shown with
their raw request and reply bytes in hex.

### 2. Bus health and diagnosis
- **Idle line level:** while the bus is quiet, the receiver output is sampled.
  - **HIGH** is correct.
  - **LOW** means A/B are swapped, or there's no fail-safe bias.
  - **UNSTABLE** means the bus is floating and picking up noise.
- **Rule-based diagnosis:** the counters are turned into findings, each with a
  cause and a fix. Each finding is rated FAULT, WARN, INFO or OK. Examples:
  - *Slave 3 never answers.* The page lists what to check, in order. If garbled
    bytes were seen, it says the settings are probably mismatched and points to
    Auto-scan.
  - *Every reply is corrupted* → baud/parity mismatch. *2 % corrupted replies* →
    termination, shielding or grounding.
  - *Stopped responding* after working: that device's power or cable.
  - *Exception 02* → explains 0-based vs 40001-style register numbering.
    Exception 01 → try FC03 vs FC04. Exception 03 → quantity limits.
  - *Another master is active*, *stray byte in front of replies* (bias
    missing), *replies from another ID* (duplicate address), *slow
    responder*.
- **Recommended master response timeout:** worked out from the slowest reply
  seen (2 × the worst latency + 20 ms, at least 100 ms).
- **LED verdict:** red blink = FAULT, yellow blink = WARN (every 2 s, when the
  bus is quiet).

### 3. Auto-scan
Auto-scan finds devices when you don't know their slave ID, baud rate or
parity. It asks every address the same simple question at each setting and
sees who answers. It only reads; it never changes anything in a device.

#### Using it
1. **Close the Modbus master on the PC.** While the scan owns the bus, data
   from the PC is discarded (and counted).
2. Open **http://192.168.4.1 → Auto-scan**.
3. Choose:

   | Option | Default |
   |---|---|
   | Slave ID range | 1–247 |
   | Probe | FC03 Read Holding Register (or FC04 Read Input Register), address 0 |
   | Reply timeout per probe | 100 ms (20–2000 ms) |
   | Baud rates | 9600, 19200, 38400, 115200 (also 57600, 4800, 2400, 1200) |
   | Formats | 8N1, 8E1 (also 8O1, 8N2) |
   | Smart stop | On |

4. Press **Start scan**. The page shows a time estimate first, then progress
   and results every second. **Stop** takes effect within one probe and keeps
   the results found so far.

#### What happens on the bus
```
for each setting (baud + format), common ones first:
    switch the RS485 port to that setting, wait 20 ms
    for ID = first .. last:
        send   [ID] [03] [00 00] [00 01] [CRC]      "read 1 register"
        wait up to the per-probe timeout for a reply
        valid reply from that ID                    -> FOUND
        bytes or framing errors, but no valid frame -> mark setting "garbled"
    if smart stop is on and something was found     -> stop after this setting
switch the port back to the PC's settings
```

- **Order of settings:** the ticked baud rates are tried in this order: 9600,
  19200, 38400, 115200, 57600, 4800, 2400, 1200. Each one is tried with the
  ticked formats in the order 8N1, 8E1, 8O1, 8N2. The default is 4 baud rates ×
  2 formats = 8 settings, and the maximum is 32.
- **One probe at a time:** before each probe the dongle waits the Modbus
  3.5-character gap plus 2 ms. It then sends the probe with the same DE/RE
  handling as normal traffic. It stops waiting as soon as a complete reply has
  arrived, so a device that answers in 10 ms doesn't cost the full timeout.
- **What counts as found:** any frame with a valid CRC from the ID that was
  asked, **including an exception reply.** A device that answers "exception
  02, illegal address" because it has no register 0 is still listed, as
  *device present*. So the default probe finds most devices.
- **Garbled hint:** sometimes bytes come back, or the UART reports framing or
  parity errors, but no valid frame can be decoded. That setting is then listed
  under *Garbled replies at…*. Usually a device is talking at a nearby baud
  rate or a different parity, so tick more settings and scan again.
- **Smart stop:** once a setting finds a device, the scan still checks all the
  remaining IDs at that setting, so every device sharing it is found. Then it
  stops instead of trying the other settings. Untick it if your bus mixes
  devices with different settings.
- **Results:** each device found is listed with its ID, baud rate, format and
  response time. The page then shows the settings to use in your master, for
  example *Use these port settings: 19200 8E1*. If nothing answered, it shows a
  wiring and settings checklist.
- **Afterwards:** the dongle switches back to the PC's port settings, and
  normal bridging resumes.

#### How long it takes
The worst case for each setting, when nobody answers, is the number of IDs ×
(timeout + about 15 ms):

| Scan | Approximate worst case |
|---|---|
| 1 setting, IDs 1–247, 100 ms timeout | 28 s |
| Default scan (8 settings), nothing found | 4 min |
| Device at 9600 8N1, smart stop on | 28 s in total |
| 8 settings, IDs 1–32 | 30 s |

To speed it up, narrow the ID range (many sites use 1–32) or tick fewer
settings.

#### Limits
- Only Modbus RTU with 8 data bits is scanned. Modbus ASCII and 7-bit devices
  are not.
- A device slower than the per-probe timeout is missed. Use 300–500 ms for
  slow meters or devices behind a gateway.
- Two devices with the same ID reply at the same time and collide. That ID
  shows as garbled or is missed, and the diagnostics page flags it during
  normal polling.
- A device that ignores FC03 completely, without even an exception reply, needs
  the FC04 probe.

## Web access, login and recovery (Gateway conventions)

| Item | Default |
|---|---|
| WiFi Access Point | **ModbusDongle**, password **12345678** (changeable, minimum 8 characters) |
| Address | **http://192.168.4.1** (or the IP shown in Settings once site WiFi is joined) |
| Web login | **admin / admin**. A red banner is shown until you change it under Settings → Security |
| Hostname | `Dongle-XXXXXX` (last 3 bytes of the MAC address) |

- **Login:** one admin session at a time. The password is stored salted and
  hashed (SHA-256). Sessions time out after 30 minutes idle. 5 failed logins
  lock the login page for 60 s.
- **Settings page:**
  - the diagnostic reply timeout and the "down after N timeouts" threshold
  - the site WiFi network
  - the AP name and password
  - the web username and password
  - a reboot button
- **Storage:** all settings are kept in flash and survive reboots and
  re-flashing the same firmware.
- **Forgot the password?** The USB port carries Modbus data, so the Gateway's
  serial recovery command is reached through a *console baud rate*:
  1. Open the dongle's COM port at **74880 baud** in a serial terminal (Arduino
     Serial Monitor, PuTTY, ...). Nothing is bridged at this baud rate.
  2. Send `factory-reset-auth`. The web login goes back to admin/admin and the
     AP password to 12345678. Nothing else is changed.

  `status` prints the AP name, IP address, health and counters. `help` lists
  the commands.

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
5. Open `UsbRs485Dongle/UsbRs485Dongle.ino` and upload. The folder holds three
   files (`UsbRs485Dongle.ino`, `web_ui.ino`, `dongle_types.h`), and the IDE
   opens them as tabs. No external libraries are needed.

arduino-cli:

```sh
arduino-cli compile --fqbn arduino:esp32:nano_nora UsbRs485Dongle
arduino-cli upload  --fqbn arduino:esp32:nano_nora -p /dev/ttyACM0 UsbRs485Dongle
```

To go back to the Gateway, flash the Gateway sketch the same way. Both
firmwares run on the same board without any changes to it.

## Configuration (top of `UsbRs485Dongle.ino`)

| Define | Default | Meaning |
|---|---|---|
| `FOLLOW_HOST_LINE_CODING` | 1 | 0 = always use the `DEFAULT_*` settings below and ignore the PC's settings |
| `DEFAULT_BAUD` / `_DATA_BITS` / `_PARITY` / `_STOP_BITS` | 9600 8N1 | Settings used at boot, or always when the define above is 0 |
| `DE_SETTLE_US` | 300 | Driver-enable settle time before the first bit |
| `ENFORCE_T35_GAP` | 1 | Wait for Modbus T3.5 bus silence before transmitting |
| `CONSOLE_BAUD` | 74880 | Opening the port at this baud enters the text console instead of bridging |
| `ALLOW_1200BAUD_REBOOT` | 1 | Opening the port at **1200 baud** reboots the board into the bootloader (the Arduino upload "touch"). Set to 0 if your bus really runs at 1200 baud; then use the double-tap-reset bootloader for uploads. |
| `MIN_BAUD` / `MAX_BAUD` | 300 / 2 000 000 | Host baud rates outside this range are ignored |
| `DEFAULT_AP_SSID` / `DEFAULT_AP_PASSWORD` | ModbusDongle / 12345678 | Factory AP settings (changeable on the web page) |
| `DEFAULT_DIAG_RESPONSE_TIMEOUT_MS` | 1000 | Factory reply timeout for the statistics (changeable on the web page) |

## Testing

`tools/modbus_rtu_test.py` sends a read-registers request through the dongle
and decodes the reply. It only needs `pyserial`:

```sh
pip install pyserial
python tools/modbus_rtu_test.py COM5 --baud 9600 --parity N --slave 1 --fc 3 --addr 0 --count 2 --repeat 10
```

The full bench test procedure, with pass criteria and a results sheet, is in
[`docs/BENCH_TEST.md`](docs/BENCH_TEST.md). It includes deliberate fault tests,
such as swapped A/B, a wrong baud rate or a second master, that check each
diagnosis.

`tests/host/run.sh` checks the firmware on a PC, with no board needed. It
compiles the sketch against minimal Arduino/ESP32 stand-ins and runs tests of
the transaction analyzer and diagnosis engine on simulated traffic:

```sh
sh tests/host/run.sh    # needs g++ and python3
```

## Differences from a Waveshare dongle

- Mark and space parity aren't supported by the ESP32 UART, so they fall back
  to no parity (red LED flash).
- 1.5 stop bits is rounded to 2.
- The DTR/RTS modem lines are ignored, apart from the 1200-baud reboot and the
  74880-baud console described above.
- There's no on-board isolation or TVS protection beyond what your Gateway
  board's transceiver stage already has.
