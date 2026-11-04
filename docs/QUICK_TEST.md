# Dongle Quick Test: the simple version

**For:** technicians testing a dongle before it ships or goes to site.
**Time:** about 15 minutes per dongle.
**You need:** a Windows PC or a Raspberry Pi, the dongle, and a second USB-RS485
adapter (e.g. Waveshare).

One command does most of the testing and prints **PASS** or **FAIL** for each
test. When something fails, it tells you what to do. After that you do 4 short
checks by hand.

> The detailed engineering procedure is in [`BENCH_TEST.md`](BENCH_TEST.md).
> You only need it when an engineer asks for it.

---

## Step 1: Install (only once per computer)

**Windows PC**
1. Install Python from **python.org**. On the first installer screen, tick
   **"Add python.exe to PATH"**.
2. Open **Command Prompt** and type:
   ```
   pip install pyserial
   ```
3. Unzip the project folder (the same ZIP that has the firmware). The test is
   in its **`tools`** folder.

**Raspberry Pi**
```
sudo apt install python3-serial
sudo usermod -aG dialout $USER        (then log out and log in again)
```

---

## Step 2: Connect

```
     PC / Raspberry Pi
      |            |
     USB          USB
      |            |
   DONGLE        SECOND ADAPTER (Waveshare)
     A  ---------- A
     B  ---------- B
    GND ---------- GND
```
- Use **3 short wires** (under 2 m). On the bench no extra resistor is needed:
  the dongle already has its 120 Ω on the board.
- Use **data USB cables**. Charge-only cables don't work.
- Close every other program that might use the ports (Arduino Serial Monitor,
  Modbus Poll, PuTTY).

---

## Step 3: Run the test (one command)

**Windows:** open the **`tools`** folder in File Explorer, click the address bar,
type **`cmd`** and press **Enter**. A command window opens in that folder. Type:
```
python quick_test.py
```

**Raspberry Pi:** in the project folder type:
```
python3 tools/quick_test.py
```

The test finds the ports by itself and takes about **1 minute**. A good dongle
looks like this:
```
======================================================================
 DONGLE QUICK TEST    2026-11-03 10:15
======================================================================
 1. Find the dongle and the second adapter ................... PASS
 2. Dongle port stays quiet .................................. PASS
 3. Read a register through the dongle (9600 8N1) ............ PASS
 4. Device error reply passes through ........................ PASS
 5. No reply from a device that doesn't exist ................ PASS
 6. Dongle follows the PC's port settings .................... PASS
 7. Wrong parity is rejected ................................. PASS
 8. Endurance (many requests in a row) ....................... PASS
----------------------------------------------------------------------
RESULT: ALL 8 TESTS PASSED  ->  DONGLE OK
Report saved: C:\...\tools\quick_test_report_2026-11-03_1015.txt
```

---

## Step 4: Read the result

| You see | Do this |
|---|---|
| **ALL TESTS PASSED → DONGLE OK** | Go to step 5 |
| A **FAIL** | Read the **What to do** line under that test, fix it, then run the test again |
| Several **FAIL** or **SKIPPED** | Fix only the **first** FAIL. The others usually have the same cause |

The most common failures:

| Failed test | Usual reason | Fix |
|---|---|---|
| 1 Find the dongle | Charge-only USB cable, or the dongle isn't flashed | Use a data cable. At power-up the dongle LED shows **red → green → blue**. `python quick_test.py --list` shows the ports |
| 1 (more than one adapter) | Other USB serial devices are plugged in | Unplug them, or name the ports: `python quick_test.py --dongle COM5 --adapter COM6` |
| 3 Read a register | Wiring | A to A, B to B, GND to GND. Still failing? Swap A and B at one end |
| Any test: "Serial port error" | Another program has the port open | Close Serial Monitor, Modbus Poll, PuTTY, then run again |
| 2 Port stays quiet | Firmware built with debug output on | Re-flash with Arduino IDE → Tools → **Core Debug Level: None** |
| 6 or 8 | Bad cable or connector | Replace the wires, run again |

Still failing? Send the **report file** (its name is printed at the end) to the
engineer.

---

## Step 5: Four checks by hand

Tick each box.

| ✔ | Check | How | OK when |
|---|---|---|---|
| ☐ | **1. LED** | Unplug the dongle and plug it in again. Watch the LED. Then run the quick test | At power-up: **red → green → blue** once. Then a short **green** blink every 2 s. While the test runs: fast **blue/green** flicker. Right after the full test: **red** blink every 2 s. That's expected, because the test causes errors on purpose. It turns green after **Reset statistics** on the web page |
| ☐ | **2. Relays stay off** | Only if relay outputs are connected: watch them while plugging in | **No relay clicks** |
| ☐ | **3. WiFi page** | Connect a phone or laptop to WiFi **ModbusDongle** (password **12345678**). Open **http://192.168.4.1** and log in with **admin / admin**. Press **Reset statistics**. On the test PC run `python quick_test.py --traffic` | The **Diagnostics** page opens. After `--traffic`, the top bar turns **green: "Bus healthy"** with slave 1 at 100 % |
| ☐ | **4. Change the password** | On the WiFi page: **Settings → Security**, set a new password twice, **Save Security** | "Security settings saved". The red "default login" banner is gone |

> The full quick test causes errors on purpose (wrong ID, wrong parity), so
> right after it the page shows red findings. That's expected: press **Reset
> statistics**, then use `--traffic` as above.
>
> **Shortcut for check 3:** connect the test PC itself to the WiFi
> **ModbusDongle**, then run `python quick_test.py --web`. This adds 4 automatic
> tests: login, traffic counting, problem detection and auto-scan.

---

## Step 6: Record

| Field | Value |
|---|---|
| Dongle serial / label | |
| Date | |
| Tested by | |
| Quick test result | ☐ ALL PASSED   ☐ FAILED (attach the report file) |
| Hand checks 1–4 | ☐ all OK |
| New web password stored | ☐ yes |
| **Dongle OK to ship** | ☐ YES   ☐ NO |

---

## On site (installers only)

Before connecting the dongle to a customer bus, measure with a multimeter:

| Measure | When | Good |
|---|---|---|
| Resistance between A and B at the dongle | Everything **powered off** | **50–65 Ω** (about 100–120 Ω = the 120 Ω at the far sensor end is missing) |
| Voltage from A (red lead) to B (black lead) | Powered, no traffic | **+0.3 to +0.8 V** (negative = A and B swapped) |

Then open the WiFi page. The **Diagnostics** page tells you what's wrong and
how to fix it, in plain words.

---

## Without a second adapter

Test against a real Modbus device (meter, VFD, PLC) whose settings you know:
```
python quick_test.py --real-device --slave 1 --baud 9600 --parity N
```
`--slave` is the device ID. `--parity` is N, E or O. Use `--fc 4` if the device
has only input registers.
