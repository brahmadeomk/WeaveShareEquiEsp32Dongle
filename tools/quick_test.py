#!/usr/bin/env python3
"""ONE-COMMAND QUICK TEST for the USB-RS485 dongle.

Runs the dongle's converter tests automatically and prints PASS / FAIL for
each one, with what to do if something fails. Works on Windows and on a
Raspberry Pi. Needs only pyserial (Windows: pip install pyserial,
Raspberry Pi: sudo apt install python3-serial).

SET-UP: plug the dongle AND a second USB-RS485 adapter (e.g. Waveshare) into
this computer, and wire them together: A to A, B to B, GND to GND.
The second adapter plays the Modbus device ("slave") during the test.

    python quick_test.py                 run the test (ports are found automatically)
    python quick_test.py --list          show the serial ports on this computer
    python quick_test.py --web           also test the WiFi page (connect this computer
                                         to the WiFi "ModbusDongle" first)
    python quick_test.py --long          longer endurance test (5000 requests)
    python quick_test.py --traffic       only send 50 good requests (for looking at the WiFi page:
                                         press "Reset statistics" there first - it then shows green)

    python quick_test.py --dongle COM5 --adapter COM6      choose the ports yourself

No second adapter? Test against a real Modbus device instead:
    python quick_test.py --real-device --slave 1 --baud 9600 --parity N
"""
import argparse
import datetime
import os
import struct
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is missing. Windows: pip install pyserial   Raspberry Pi: sudo apt install python3-serial")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import modbus_slave_sim as sim  # noqa: E402

ARDUINO_VID = 0x2341

if os.name == "nt":
    os.system("")  # enables colours in the Windows terminal
USE_COLOUR = sys.stdout.isatty()


def colour(text, code):
    return "\033[%sm%s\033[0m" % (code, text) if USE_COLOUR else text


GREEN, RED, YELLOW, BOLD = "1;32", "1;31", "1;33", "1"


# ---------------------------------------------------------------- ports
def list_ports():
    ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)
    out = []
    for p in ports:
        if p.vid is None:
            continue  # built-in UARTs (e.g. /dev/ttyAMA0 on a Pi), not USB
        kind = "DONGLE (Arduino Nano ESP32)" if p.vid == ARDUINO_VID else "USB serial adapter"
        out.append((p.device, kind, p.description or "", p.vid))
    return out


def find_ports(want_adapter):
    ports = list_ports()
    dongles = [p for p in ports if p[3] == ARDUINO_VID]
    others = [p for p in ports if p[3] != ARDUINO_VID]
    dongle = dongles[0][0] if len(dongles) == 1 else None
    adapter = others[0][0] if (want_adapter and len(others) == 1) else None
    return dongle, adapter, dongles, others


# ---------------------------------------------------------------- Modbus master
def crc16(data):
    return sim.crc16(data)


def poll(port, baud, parity, stop, slave, fc=3, addr=0, count=2, n=10, timeout=0.5, interval=0.02, give_up_after=0):
    """Send n identical read requests through the dongle. Returns counters."""
    r = {"ok": 0, "exc": 0, "none": 0, "bad": 0, "values": None, "exc_code": None, "lat": []}
    req = struct.pack(">BBHH", slave, fc, addr, count)
    req += struct.pack("<H", crc16(req))
    expected = 5 + 2 * count
    with serial.Serial(port, baud, bytesize=8, parity=parity, stopbits=stop, timeout=timeout) as ser:
        time.sleep(0.3)  # the dongle applies the new port settings
        ser.reset_input_buffer()
        for i in range(n):
            if i and interval:
                time.sleep(interval)
            ser.reset_input_buffer()
            t0 = time.perf_counter()
            ser.write(req)
            rx = ser.read(expected)
            r["lat"].append((time.perf_counter() - t0) * 1000)
            if len(rx) == 0:
                r["none"] += 1
            elif len(rx) == 5 and rx[1] & 0x80 and crc16(rx[:3]) == struct.unpack("<H", rx[3:5])[0]:
                r["exc"] += 1
                r["exc_code"] = rx[2]
            elif len(rx) == expected and crc16(rx[:-2]) == struct.unpack("<H", rx[-2:])[0]:
                r["ok"] += 1
                r["values"] = list(struct.unpack(">%dH" % count, rx[3:-2]))
            else:
                r["bad"] += 1
            if give_up_after and i + 1 == give_up_after and r["ok"] == 0:
                r["gave_up"] = i + 1  # nothing works: don't wait for every remaining timeout
                break
    return r


def fmt_setting(baud, parity, stop):
    return "%d 8%s%d" % (baud, parity, stop)


# ---------------------------------------------------------------- simulated slave on the 2nd adapter
class SimSlave:
    def __init__(self, port):
        self.port = port
        self.ser = None
        self.thread = None
        self.stop_evt = None

    def start(self, baud, parity="N", stop=1, ids=(1,)):
        """(Re)start the simulated device. The port stays open between tests and
        is only reconfigured - closing and reopening a COM port can be slow."""
        self._stop_thread()
        if self.ser is None:
            self.ser = serial.Serial(self.port, baud, bytesize=8, parity=parity, stopbits=stop, timeout=0)
        else:
            self.ser.baudrate, self.ser.parity, self.ser.stopbits = baud, parity, stop
        self.ser.reset_input_buffer()
        self.stop_evt = threading.Event()
        slaves = {i: sim.Slave(100) for i in ids}
        self.thread = threading.Thread(target=sim.serve, args=(self.ser, slaves, self.stop_evt),
                                       kwargs={"quiet": True, "log": lambda m: None}, daemon=True)
        self.thread.start()
        time.sleep(0.2)

    def _stop_thread(self):
        if self.thread:
            self.stop_evt.set()
            self.thread.join(2)
            self.thread = None

    def stop(self):
        self._stop_thread()
        if self.ser:
            self.ser.close()
            self.ser = None


# ---------------------------------------------------------------- test runner
class Runner:
    def __init__(self):
        self.results = []
        self.lines = []

    def out(self, text=""):
        print(text, flush=True)
        self.lines.append(text)

    def run(self, title, fn):
        n = len(self.results) + 1
        label = "%2d. %s " % (n, title)
        print(label + "." * max(3, 62 - len(label)) + " ", end="", flush=True)
        try:
            ok, happened, todo = fn()
        except SystemExit as e:
            ok, happened, todo = False, str(e), ""
        except serial.SerialException as e:
            ok, happened, todo = False, "Serial port error: %s" % e, \
                "Close any other program using the port (Arduino Serial Monitor, Modbus Poll, PuTTY) and run again."
        except Exception as e:  # keep going with the other tests
            ok, happened, todo = False, "%s: %s" % (type(e).__name__, e), "Run again; if it repeats, send the report file to the engineer."
        verdict = colour("PASS", GREEN) if ok else colour("FAIL", RED)
        print(verdict, flush=True)
        self.lines.append(label + "." * max(3, 62 - len(label)) + " " + ("PASS" if ok else "FAIL"))
        if happened:
            self.out("      Result     : " + happened)
        if not ok and todo:
            self.out("      " + colour("What to do", YELLOW) + " : " + todo if USE_COLOUR else "      What to do : " + todo)
        self.results.append((title, ok))
        return ok


def skip(runner, title, reason):
    n = len(runner.results) + 1
    label = "%2d. %s " % (n, title)
    dots = "." * max(3, 62 - len(label))
    print(label + dots + " " + colour("SKIPPED", YELLOW), flush=True)
    runner.lines.append(label + dots + " SKIPPED")
    runner.out("      Result     : " + reason)
    runner.results.append((title, None))  # None = skipped


def summarise(runner, args):
    passed = sum(1 for _, ok in runner.results if ok)
    total = len(runner.results)
    failed = [str(i + 1) for i, (_, ok) in enumerate(runner.results) if ok is False]
    skipped = [str(i + 1) for i, (_, ok) in enumerate(runner.results) if ok is None]
    runner.out("-" * 70)
    if passed == total and getattr(args, "traffic", False):
        runner.out(colour("DONE - now look at the WiFi page (this was not the full quick test)", GREEN))
    elif passed == total:
        runner.out(colour("RESULT: ALL %d TESTS PASSED  ->  DONGLE OK" % total, GREEN))
    else:
        txt = "RESULT: %d of %d passed  ->  NOT OK   failed: test %s" % (passed, total, ", ".join(failed))
        if skipped:
            txt += "   skipped: test %s" % ", ".join(skipped)
        runner.out(colour(txt, RED))
        runner.out("Fix the first failed test, then run the quick test again.")
    name = "quick_test_report_%s.txt" % datetime.datetime.now().strftime("%Y-%m-%d_%H%M")
    path = os.path.join(args.report_dir, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(runner.lines) + "\n")
    runner.out("Report saved: %s" % os.path.abspath(path))
    return passed == total


# ---------------------------------------------------------------- the tests
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="show the serial ports and exit")
    ap.add_argument("--dongle", help="dongle port (COM5, /dev/ttyACM0); found automatically if omitted")
    ap.add_argument("--adapter", help="second adapter port (COM6, /dev/ttyUSB0); found automatically if omitted")
    ap.add_argument("--real-device", action="store_true", help="test against a real Modbus device instead of a second adapter")
    ap.add_argument("--slave", type=int, default=1, help="real device: slave ID (default 1)")
    ap.add_argument("--baud", type=int, default=9600, help="real device: baud rate (default 9600)")
    ap.add_argument("--parity", choices="NEO", default="N", help="real device: parity N/E/O (default N)")
    ap.add_argument("--stop", type=int, choices=(1, 2), default=1, help="real device: stop bits (default 1)")
    ap.add_argument("--fc", type=int, choices=(3, 4), default=3, help="real device: 3 = holding, 4 = input registers")
    ap.add_argument("--addr", type=int, default=0, help="real device: register address to read (default 0)")
    ap.add_argument("--long", action="store_true", help="endurance test with 5000 requests instead of 1000")
    ap.add_argument("--traffic", action="store_true", help="only send 50 good requests (healthy traffic for the WiFi page)")
    ap.add_argument("--web", action="store_true", help="also test the WiFi page (connect to WiFi ModbusDongle first)")
    ap.add_argument("--host", default="192.168.4.1", help="dongle web address (default 192.168.4.1)")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default=os.environ.get("DONGLE_PASSWORD", "admin"))
    ap.add_argument("--report-dir", default=".", help="where to save the report file")
    args = ap.parse_args()

    if args.list:
        ports = list_ports()
        if not ports:
            print("No USB serial ports found. Check the USB cables (they must be data cables).")
        for dev, kind, desc, _ in ports:
            print("  %-22s %-30s %s" % (dev, kind, desc))
        return 0

    r = Runner()
    use_adapter = not args.real_device
    st = {"dongle": args.dongle, "adapter": args.adapter}
    simslave = None

    r.out("=" * 70)
    r.out(colour(" DONGLE QUICK TEST", BOLD) + "    " + datetime.datetime.now().strftime("%Y-%m-%d %H:%M"))
    r.out("=" * 70)

    # 1 - ports
    def t_find():
        auto_d, auto_a, dongles, others = find_ports(use_adapter)
        if not st["dongle"]:
            if len(dongles) > 1:
                return False, "More than one dongle connected (%s)." % ", ".join(d[0] for d in dongles), \
                    "Unplug the extra dongle, or choose one with --dongle."
            st["dongle"] = auto_d
        if not st["dongle"]:
            return False, "No dongle (Arduino Nano ESP32) found.", \
                "Plug the dongle in with a DATA USB cable (the LED flashes white at power-up). " \
                "Run 'python quick_test.py --list' to see the ports. Windows: look in Device Manager > Ports (COM & LPT)."
        if use_adapter and not st["adapter"]:
            if len(others) > 1:
                return False, "Several USB serial adapters found: %s." % ", ".join(o[0] for o in others), \
                    "Unplug the ones you don't need, or choose one with --adapter COMx."
            st["adapter"] = auto_a
            if not st["adapter"]:
                return False, "Dongle found on %s, but no second USB-RS485 adapter." % st["dongle"], \
                    "Plug in the second adapter (it plays the Modbus device), or test a real device: " \
                    "python quick_test.py --real-device --slave 1 --baud 9600 --parity N"
        msg = "Dongle on %s" % st["dongle"]
        msg += (", second adapter on %s" % st["adapter"]) if use_adapter else ", real device mode"
        return True, msg, ""

    if not r.run("Find the dongle and the second adapter", t_find):
        summarise(r, args)
        return 1
    D = st["dongle"]

    # 2 - silence
    def t_quiet():
        with serial.Serial(D, 9600, timeout=2) as s:
            data = s.read(64)
        if data:
            return False, "The dongle sent %d byte(s) by itself: %s" % (len(data), data[:20]), \
                "Re-flash the firmware with Arduino IDE > Tools > Core Debug Level = None. If a device on the bus " \
                "talks by itself, disconnect it for this test."
        return True, "No unexpected data in 2 s", ""

    r.run("Dongle port stays quiet", t_quiet)

    if args.traffic:
        def t_traffic():
            if use_adapter:
                sl = SimSlave(st["adapter"])
                try:
                    sl.start(9600, "N", 1)
                    res = poll(D, 9600, "N", 1, slave=1, n=50, interval=0.1)
                finally:
                    sl.stop()
            else:
                res = poll(D, args.baud, args.parity, args.stop, slave=args.slave, fc=args.fc, addr=args.addr,
                           count=1, n=50, timeout=1.0, interval=0.1)
            if res["ok"] == 50:
                return True, "50 good requests sent. Look at the WiFi page now: the top bar should be green 'Bus healthy'.", ""
            return False, "%d/50 good replies." % res["ok"], "Run the full quick test (without --traffic) to find the problem."

        r.run("Send healthy traffic for the WiFi page", t_traffic)
        return 0 if summarise(r, args) else 1

    if use_adapter:
        simslave = SimSlave(st["adapter"])
        try:
            run_adapter_tests(r, D, simslave, args)
        finally:
            simslave.stop()
    else:
        run_real_device_tests(r, D, args)

    if args.web:
        run_web_tests(r, D, simslave if use_adapter else None, args)

    ok = summarise(r, args)
    return 0 if ok else 1


WIRING_TODO = ("Check the wiring: A to A, B to B, GND to GND between the dongle and the adapter. "
               "If it still fails, swap A and B at one end. Make sure no other program uses either port.")


def run_adapter_tests(r, D, simslave, args):
    # 3 - basic read
    def t_basic():
        simslave.start(9600, "N", 1)
        res = poll(D, 9600, "N", 1, slave=1, n=10)
        if res["ok"] == 10 and res["values"] == [0, 1]:
            return True, "10/10 replies, values [0, 1], about %.0f ms each" % (sum(res["lat"]) / len(res["lat"])), ""
        if res["ok"] + res["exc"] == 0 and res["bad"] == 0:
            return False, "No reply at all (10 requests at 9600 8N1).", WIRING_TODO
        return False, "%d/10 good replies (%d corrupted, %d missing)." % (res["ok"], res["bad"], res["none"]), \
            "Check the cable and connectors, and that the 120 ohm termination is only at the two ends."

    basic_ok = r.run("Read a register through the dongle (9600 8N1)", t_basic)

    # 4 - exception passes through
    def t_exc():
        res = poll(D, 9600, "N", 1, slave=1, addr=9999, count=1, n=3)
        if res["exc"] == 3 and res["exc_code"] == 2:
            return True, "Device error reply 'exception 02' passed through 3/3", ""
        return False, "Expected 3 exception replies, got %d (ok %d, missing %d)." % (res["exc"], res["ok"], res["none"]), \
            "Run the test again. If it repeats, send the report to the engineer."

    # 5 - missing device stays silent
    def t_missing():
        res = poll(D, 9600, "N", 1, slave=77, n=3, timeout=0.3)
        if res["none"] == 3:
            return True, "No reply for slave ID 77, as expected", ""
        return False, "Something answered slave ID 77.", \
            "Another device on the bus uses ID 77, or another master is talking. Disconnect other devices for this test."

    if not basic_ok:
        why = "not run - fix test 3 first (the dongle and the adapter must talk before anything else can be tested)"
        for title in ("Device error reply passes through", "No reply from a device that doesn't exist",
                      "Dongle follows the PC's port settings", "Wrong parity is rejected", "Endurance (many requests in a row)"):
            skip(r, title, why)
        return
    r.run("Device error reply passes through", t_exc)
    r.run("No reply from a device that doesn't exist", t_missing)

    # 6 - port settings follow the PC
    def t_settings():
        settings = [(9600, "E", 1), (9600, "O", 1), (9600, "N", 2), (19200, "E", 1), (38400, "N", 1), (115200, "N", 1)]
        bad = []
        for baud, par, stop in settings:
            simslave.start(baud, par, stop)
            res = poll(D, baud, par, stop, slave=1, n=10)
            if res["ok"] != 10:
                bad.append("%s (%d/10)" % (fmt_setting(baud, par, stop), res["ok"]))
        if not bad:
            return True, "6 settings x 10 requests all OK: " + ", ".join(fmt_setting(*s) for s in settings), ""
        return False, "Failed at: " + ", ".join(bad), \
            "If only the high baud rates fail: check the cable length and the 120 ohm termination at the far end. " \
            "If a whole parity fails: make sure the dongle has firmware v1.2 or newer."

    r.run("Dongle follows the PC's port settings", t_settings)

    # 7 - wrong parity must fail
    def t_parity():
        simslave.start(9600, "E", 1)
        res = poll(D, 9600, "N", 1, slave=1, n=5, timeout=0.3)
        if res["ok"] == 0:
            return True, "No valid reply when the PC and the device use different parity (correct)", ""
        return False, "%d/5 valid replies even though the parity was wrong." % res["ok"], \
            "The dongle is not applying the parity. Make sure it runs firmware v1.2 or newer; send the report to the engineer."

    r.run("Wrong parity is rejected", t_parity)

    # 8 - endurance
    def t_endurance():
        n = 5000 if args.long else 1000
        simslave.start(19200, "E", 1)
        print("(%d requests, about %d s) " % (n, n * 25 // 1000), end="", flush=True)
        res = poll(D, 19200, "E", 1, slave=1, count=10, n=n, interval=0.0, give_up_after=20)
        if res.get("gave_up"):
            return False, "Stopped after %d requests with no good reply at 19200 8E1." % res["gave_up"], \
                "Test 6 shows which port settings work. " + WIRING_TODO
        need = n - n // 1000  # 99.9 %
        msg = "%d/%d OK at 19200 8E1 (%d missing, %d corrupted)" % (res["ok"], n, res["none"], res["bad"])
        if res["ok"] >= need:
            return True, msg, ""
        return False, msg, "Check the cable, connectors, and termination (120 ohm at the far end only), then run again. " \
                           "Keep the cable away from motor/VFD wiring."

    r.run("Endurance (many requests in a row)", t_endurance)


def run_real_device_tests(r, D, args):
    def t_basic():
        res = poll(D, args.baud, args.parity, args.stop, slave=args.slave, fc=args.fc, addr=args.addr, count=1, n=10, timeout=1.0)
        s = fmt_setting(args.baud, args.parity, args.stop)
        if res["ok"] == 10:
            return True, "10/10 replies from slave %d at %s, value %s" % (args.slave, s, res["values"]), ""
        if res["exc"]:
            return False, "The device answered with exception %s." % res["exc_code"], \
                "The device is connected and talking. Choose a register it has (--addr) or try --fc 4."
        if res["ok"] + res["bad"] == 0:
            return False, "No reply from slave %d at %s." % (args.slave, s), \
                "Check the slave ID, baud rate and parity of the device (--slave --baud --parity), the wiring " \
                "(A to A, B to B, GND), and that the device is powered. Unknown settings? Use Auto-scan on the WiFi page."
        return False, "%d/10 good, %d corrupted." % (res["ok"], res["bad"]), \
            "Corrupted replies usually mean wrong baud/parity or a wiring/termination problem."

    if r.run("Read a register from the real device", t_basic):
        def t_end():
            n = 5000 if args.long else 1000
            res = poll(D, args.baud, args.parity, args.stop, slave=args.slave, fc=args.fc, addr=args.addr, count=1, n=n,
                       timeout=1.0, interval=0.01, give_up_after=20)
            need = n - n // 1000
            msg = "%d/%d OK (%d missing, %d corrupted)" % (res["ok"], n, res["none"], res["bad"])
            return res["ok"] >= need, msg, "Check the cable, connectors and termination, then run again."

        r.run("Endurance (many requests in a row)", t_end)


def run_web_tests(r, D, simslave, args):
    import dongle_cli  # noqa: E402

    st = {}

    def t_login():
        dg = dongle_cli.Dongle(args.host, args.user, args.password)
        dg.login()
        st["dg"] = dg
        return True, "Logged in to http://%s" % args.host, ""

    if not r.run("WiFi page: log in", t_login):
        r.out("      (Connect this computer to the WiFi 'ModbusDongle', password 12345678, then run again with --web.)")
        return
    dg = st["dg"]

    def t_counts():
        if simslave:
            simslave.start(9600, "N", 1)
        dg.post("/api/resetStats")
        n = 20
        if simslave:
            poll(D, 9600, "N", 1, slave=1, n=n)
        else:
            poll(D, args.baud, args.parity, args.stop, slave=args.slave, fc=args.fc, addr=args.addr, count=1, n=n, timeout=1.0)
        time.sleep(3)  # let the dongle measure the idle line
        d = dg.get("/api/diag")
        t = d["totals"]
        msg = "page shows %d requests, %d OK, health %s, idle level %s" % (t["requests"], t["ok"], d["health"]["level"], d["link"]["idle"])
        if t["requests"] >= n and t["ok"] >= n and d["health"]["level"] == "OK":
            return True, msg, ""
        todo = "Open the Diagnostics page in a browser and follow the remedy shown for each finding."
        return False, msg, todo

    r.run("WiFi page: counts the traffic, bus healthy", t_counts)

    def t_finding():
        dg.post("/api/resetStats")
        if simslave:
            poll(D, 9600, "N", 1, slave=77, n=5, timeout=0.5)
        else:
            poll(D, args.baud, args.parity, args.stop, slave=247, n=5, timeout=0.5)
        d = dg.get("/api/diag")
        sid = 77 if simslave else 247
        titles = [f["title"] for f in d["health"]["findings"]]
        want = "Slave %d never answers" % sid
        dg.post("/api/resetStats")
        if want in titles:
            return True, "page reports '%s' with a remedy" % want, ""
        return False, "findings were: %s" % ("; ".join(titles) or "none"), "Send the report to the engineer."

    r.run("WiFi page: diagnosis finds a missing device", t_finding)

    if simslave:
        def t_scan():
            simslave.start(19200, "E", 1, ids=(1,))
            res = dg.post("/api/scan/start", idFrom=1, idTo=5, fc=3, addr=0, timeout=100,
                          bauds="9600,19200", fmts="N1,E1", stopFirst=1)
            if not res.get("ok"):
                return False, "scan refused: %s" % res.get("error"), "Close any program using the dongle's port and run again."
            s = {}
            for _ in range(60):
                time.sleep(1)
                s = dg.get("/api/scan")
                if s["state"] != "RUNNING":
                    break
            found = ["ID %d at %d %s" % (x["id"], x["baud"], x["fmt"]) for x in s.get("results", [])]
            if "ID 1 at 19200 8E1" in found:
                return True, "Auto-scan found " + ", ".join(found), ""
            return False, "Auto-scan result: %s" % (", ".join(found) or "nothing found"), \
                "Check the wiring, then try Auto-scan from the web page."

        r.run("WiFi page: auto-scan finds the device", t_scan)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(1)
