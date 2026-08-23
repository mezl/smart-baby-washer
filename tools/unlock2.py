#!/usr/bin/env python3
"""Clear the controller lockout, verify it properly, and RECORD what worked.

Every previous attempt at this guessed a rule and then had it falsified:
"60 s clears it", "it is temperature-gated below 36 C". Three 60 s cuts at
33-34 C failed the day after an identical cut at 33 C succeeded. So this does
not encode a rule. It escalates, verifies with the definitive probe rather
than with status bit 6, and writes every attempt to a CSV so the rule can be
derived from data instead of asserted.

Verification matters: bit 6 is an INDICATOR. The proof that the controller is
accepting commands is to command the intake motor and watch the flow meter --
the only load on this machine with a sensor on it.

    python3 tools/unlock2.py [host] [plug]
"""
import io, json, os, sys, time, urllib.request
sys.path.insert(0, "/tmp/claude-1000/-home-kai/75cadcaa-ea43-4cc3-b05f-8a5941ae3849/scratchpad")
from kasa import call

HOST = sys.argv[1] if len(sys.argv) > 1 else "baby-washer.local"
PLUG = sys.argv[2] if len(sys.argv) > 2 else "192.168.14.123"
B = f"http://{HOST}"
CSV = "/home/kai/software/d8-smart/captures/unlock-attempts.csv"
CUTS = [60, 120, 180, 300, 600]      # escalating
SETTLE = 240                          # wait between attempts

def g():
    for _ in range(4):
        try:
            with urllib.request.urlopen(B + "/api/status", timeout=6) as f:
                return json.load(f)
        except Exception:
            time.sleep(0.5)
    return None

def p(path):
    for _ in range(4):
        try:
            r = urllib.request.Request(B + path, method="POST")
            with urllib.request.urlopen(r, timeout=6) as f:
                return True
        except Exception:
            time.sleep(0.5)
    return False

def relay(s): call(PLUG, {"system": {"set_relay_state": {"state": s}}})
def stamp():  return time.strftime("%H:%M:%S")

def wait_up(maxw=120):
    t0 = time.time()
    while time.time() - t0 < maxw:
        d = g()
        # st_real is 0 until the first controller frame lands; a board that has
        # only just booted reports "no fault" before it knows anything.
        if d and d.get("ok_c", 0) > 20:
            return d
        time.sleep(3)
    return None

def probe():
    """Definitive: does the controller act on a load command? Costs a little water."""
    d = g()
    if d is None or d["pb1"]:
        return None
    base = d["flow_real"]; peak = base
    try:
        p("/api/mode_ovr?b2=2a&b3=07")
        p("/api/panel_ovr?clr=FF&set=20")
        t0 = time.time()
        while time.time() - t0 < 20:
            q = g()
            if q: peak = max(peak, q["flow_real"])
            if peak > base + 3: break
            time.sleep(1)
    finally:
        p("/api/panel_ovr?clr=0&set=0"); p("/api/mode_ovr?b2=&b3=")
    return peak > base + 3

def rec(row):
    new = not os.path.exists(CSV)
    with io.open(CSV, "a") as f:
        if new: f.write("time,temp_c,cut_s,bit6_after,probe_after,uptime_s\n")
        f.write(",".join(str(x) for x in row) + "\n")

print(f"[{stamp()}] unlock: escalating cuts, verified by flow probe", flush=True)
attempt = 0
while True:
    d = wait_up()
    if d is None:
        print(f"[{stamp()}] board down — restoring power"); relay(1); time.sleep(20); continue
    if d["pb1"]:
        print(f"[{stamp()}] loads commanded (0x{d['pb1']:02X}) — standing off", flush=True)
        time.sleep(60); continue

    bit6 = bool(d["st_real"] & 0x40)
    if not bit6:
        ok = probe()
        print(f"[{stamp()}] bit6 clear at {d['temp_real']}C — probe says "
              f"{'ACCEPTING COMMANDS' if ok else 'still not responding'}", flush=True)
        rec([stamp(), d["temp_real"], 0, 0, 1 if ok else 0, d["uptime_s"]])
        if ok:
            print(f"[{stamp()}] UNLOCKED — verified by flow", flush=True)
            break
    cut = CUTS[min(attempt, len(CUTS) - 1)]
    attempt += 1
    print(f"[{stamp()}] attempt {attempt}: {d['temp_real']}C, cutting {cut}s", flush=True)
    try:
        relay(0); time.sleep(cut)
    finally:
        relay(1)                       # always restore, even if killed
    d = wait_up()
    if d is None:
        print(f"[{stamp()}]   board did not return"); continue
    time.sleep(20)                      # bit 6 has appeared up to 45 s after boot
    d = g() or d
    bit6 = bool(d["st_real"] & 0x40)
    ok = None if bit6 else probe()
    print(f"[{stamp()}]   -> byte3=0x{d['st_real']:02X} at {d['temp_real']}C"
          f"  probe={'n/a' if ok is None else ('PASS' if ok else 'FAIL')}", flush=True)
    rec([stamp(), d["temp_real"], cut, 1 if bit6 else 0,
         "" if ok is None else (1 if ok else 0), d["uptime_s"]])
    if ok:
        print(f"[{stamp()}] UNLOCKED after a {cut}s cut at {d['temp_real']}C", flush=True)
        break
    time.sleep(SETTLE)
