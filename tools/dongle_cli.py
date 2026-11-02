#!/usr/bin/env python3
"""Command-line access to the dongle's diagnostics, auto-scan and sniffer.

For headless machines such as a Raspberry Pi: talks to the dongle's web API
over WiFi (the dongle's Access Point, http://192.168.4.1, or its site-WiFi
IP). Uses only the Python standard library.

    python3 dongle_cli.py diag                       # health, findings + remedies, slave table
    python3 dongle_cli.py diag --watch 5             # refresh every 5 s
    python3 dongle_cli.py reset                      # reset statistics
    python3 dongle_cli.py scan --from 1 --to 32 --bauds 9600,19200 --fmts N1,E1
    python3 dongle_cli.py sniff-start --auto         # or: --baud 19200 --fmt E1
    python3 dongle_cli.py sniff-show                 # stream decoded frames (Ctrl+C to stop)
    python3 dongle_cli.py sniff-stop

Common options: --host 192.168.4.1 --user admin --password admin
(or set DONGLE_HOST / DONGLE_USER / DONGLE_PASSWORD).
"""
import argparse
import http.cookiejar
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class Dongle:
    def __init__(self, host, user, password):
        self.base = "http://" + host
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))
        self.user, self.password = user, password

    def login(self):
        data = urllib.parse.urlencode({"user": self.user, "pass": self.password}).encode()
        try:
            r = self.opener.open(self.base + "/login", data=data, timeout=10)
        except urllib.error.URLError as e:
            sys.exit("Cannot reach %s (%s). Is this machine connected to the dongle's WiFi "
                     "(ModbusDongle) or the same network?" % (self.base, e.reason))
        final = r.geturl()
        if "locked" in final:
            sys.exit("Login locked for 60 s after too many failed attempts.")
        if "/login" in final:
            sys.exit("Login failed: wrong user name or password (--user / --password).")

    def _req(self, path, data=None, retry=True):
        try:
            body = None if data is None else urllib.parse.urlencode(data).encode()
            r = self.opener.open(self.base + path, data=body, timeout=10)
            return json.loads(r.read().decode())
        except urllib.error.HTTPError as e:
            if e.code == 401 and retry:
                self.login()
                return self._req(path, data, retry=False)
            raise

    def get(self, path):
        return self._req(path)

    def post(self, path, **fields):
        return self._req(path, data=fields)


SEV_MARK = {"FAULT": "[FAULT]", "WARN": "[WARN] ", "INFO": "[INFO] ", "OK": "[OK]   "}


def print_diag(d):
    L, T = d["link"], d["totals"]
    print("=" * 78)
    print("HEALTH: %s    bus %s  mode %s  idle level %s" % (d["health"]["level"], L["line"], L["mode"], L["idle"]))
    print("Requests %d  OK %d  timeouts %d  CRC %d  exceptions %d  wrong ID %d  late %d  foreign %d"
          % (T["requests"], T["ok"], T["timeouts"], T["crc"], T["exceptions"], T["wrongSlave"], T["late"], T["foreign"]))
    print("Recommended master response timeout: %d ms" % d["recTimeoutMs"])
    print("-" * 78)
    for f in d["health"]["findings"]:
        print("%s %s" % (SEV_MARK.get(f["sev"], f["sev"]), f["title"]))
        print("        Likely cause: %s" % f["cause"])
        steps = [s for s in f["remedy"].split("\n") if s]
        label = "Notes" if f["sev"] == "OK" else "Remedy"
        for i, s in enumerate(steps, 1):
            print("        %s %d. %s" % (label if i == 1 else " " * len(label), i, s))
        print()
    if d["slaves"]:
        print("%4s %8s %8s %8s %8s %6s %10s %22s  %s" % ("ID", "Req", "OK", "Success", "Timeout", "CRC", "Exceptions", "Latency min/avg/max ms", "Status"))
        for s in d["slaves"]:
            lat = "%s / %s / %s" % (s["latMin"], s["latAvg"], s["latMax"]) if s["latN"] else "-"
            exc = str(s["exc"]) + (" (" + s["excList"] + ")" if s["excList"] else "")
            print("%4d %8d %8d %7s%% %8d %6d %10s %22s  %s" % (s["id"], s["req"], s["ok"], s["pct"], s["to"], s["crc"], exc, lat, s["status"]))


def cmd_diag(dg, a):
    while True:
        print_diag(dg.get("/api/diag"))
        if not a.watch:
            return
        time.sleep(a.watch)


def cmd_reset(dg, a):
    dg.post("/api/resetStats")
    print("Statistics reset.")


def cmd_scan(dg, a):
    r = dg.post("/api/scan/start", idFrom=a.id_from, idTo=a.id_to, fc=a.fc, addr=a.addr, timeout=a.timeout,
                bauds=a.bauds, fmts=a.fmts, stopFirst=0 if a.full else 1)
    if not r.get("ok"):
        sys.exit("Scan refused: " + r.get("error", "?"))
    print("Scan started (stop the Modbus master on the PC first). Ctrl+C stops the scan.")
    try:
        while True:
            time.sleep(1)
            s = dg.get("/api/scan")
            print("\r%-8s %5d/%-5d probes  ID %-3d at %-14s %4ds   " % (s["state"], s["done"], s["total"], s["id"], s["combo"], s["elapsedS"]), end="", flush=True)
            if s["state"] not in ("RUNNING",):
                break
    except KeyboardInterrupt:
        dg.post("/api/scan/stop")
        time.sleep(1.5)
        s = dg.get("/api/scan")
    print()
    if s["results"]:
        print("Devices found:")
        for r in s["results"]:
            print("  ID %3d  %6d %s  %s  latency %s ms" % (r["id"], r["baud"], r["fmt"],
                  "exception %d (device present)" % r["exc"] if r["exc"] else "data", r["lat"]))
        print("Use these port settings in your master: " + ", ".join(sorted({"%d %s" % (r["baud"], r["fmt"]) for r in s["results"]})))
    else:
        print("No device answered. Check power, A/B wiring, ID range, and add more baud rates/formats.")
    if s["garbled"]:
        print("Garbled replies at: " + ", ".join(s["garbled"]) + " (a device may be at a nearby setting)")


def cmd_sniff_start(dg, a):
    fields = {"fwd": 0 if a.no_copy else 1, "reset": 0 if a.keep_stats else 1}
    if a.auto or not a.baud:
        fields["auto"] = 1
    else:
        fields.update(baud=a.baud, fmt=a.fmt)
    r = dg.post("/api/sniff/start", **fields)
    if not r.get("ok"):
        sys.exit("Sniffer refused: " + r.get("error", "?"))
    print("Sniffer started (listen-only). Use 'sniff-show' to watch frames, 'sniff-stop' to end.")


def cmd_sniff_stop(dg, a):
    dg.post("/api/sniff/stop")
    print("Sniffer stopped; the dongle is back to USB bridging.")


KIND = ["MASTER", "SLAVE ", "EXCEPT", "?     ", "GARBAGE"]


def cmd_sniff_show(dg, a):
    since, last_state = 0, None
    first = dg.get("/api/sniff?since=0")
    since = first["seq"]  # only show new frames
    print("Watching new frames - Ctrl+C to stop watching (the sniffer keeps running).")
    prev_t = None
    try:
        while True:
            d = dg.get("/api/sniff?since=%d" % since)
            state = d["state"] + " " + d["combo"]
            if state != last_state:
                print("--- sniffer %s%s" % (state, " (auto-detected)" if d["auto"] else ""))
                last_state = state
            for f in d["list"]:
                b = f["b"]
                gap = "" if prev_t is None else "%8.1f" % ((f["t"] - prev_t) / 1000.0)
                prev_t = f["t"]
                head = "ID %3d FC %2d" % (b[0], b[1] & 0x7F) if f["k"] != 4 and len(b) >= 2 else " " * 12
                print("%10.4f %8s  %s  %s  %s%s" % (f["t"] / 1e6, gap, KIND[f["k"]], head,
                      " ".join("%02X" % x for x in b), " ..." if f["n"] > len(b) else ""))
                since = f["s"]
            if not d["list"]:
                since = max(since, d["seq"])
            time.sleep(0.05 if d["more"] else 0.5)
    except KeyboardInterrupt:
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("DONGLE_HOST", "192.168.4.1"))
    ap.add_argument("--user", default=os.environ.get("DONGLE_USER", "admin"))
    ap.add_argument("--password", default=os.environ.get("DONGLE_PASSWORD", "admin"))
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("diag", help="show health, findings with remedies, slave table")
    p.add_argument("--watch", type=float, default=0, help="refresh every N seconds")
    p.set_defaults(fn=cmd_diag)
    sub.add_parser("reset", help="reset statistics").set_defaults(fn=cmd_reset)

    p = sub.add_parser("scan", help="run auto-scan and print the devices found")
    p.add_argument("--from", dest="id_from", type=int, default=1)
    p.add_argument("--to", dest="id_to", type=int, default=247)
    p.add_argument("--fc", type=int, choices=(3, 4), default=3)
    p.add_argument("--addr", type=int, default=0)
    p.add_argument("--timeout", type=int, default=100, help="reply timeout per probe, ms")
    p.add_argument("--bauds", default="9600,19200,38400,115200")
    p.add_argument("--fmts", default="N1,E1", help="any of N1,E1,O1,N2")
    p.add_argument("--full", action="store_true", help="don't stop after the first setting that finds devices")
    p.set_defaults(fn=cmd_scan)

    p = sub.add_parser("sniff-start", help="start the listen-only sniffer")
    p.add_argument("--auto", action="store_true", help="auto-detect bus settings (default if --baud not given)")
    p.add_argument("--baud", type=int)
    p.add_argument("--fmt", default="N1", choices=("N1", "E1", "O1", "N2"))
    p.add_argument("--no-copy", action="store_true", help="don't copy bus bytes to the dongle's COM port")
    p.add_argument("--keep-stats", action="store_true", help="don't reset statistics at start")
    p.set_defaults(fn=cmd_sniff_start)
    sub.add_parser("sniff-stop", help="stop the sniffer").set_defaults(fn=cmd_sniff_stop)
    sub.add_parser("sniff-show", help="stream decoded frames").set_defaults(fn=cmd_sniff_show)

    a = ap.parse_args()
    dg = Dongle(a.host, a.user, a.password)
    dg.login()
    a.fn(dg, a)


if __name__ == "__main__":
    main()
