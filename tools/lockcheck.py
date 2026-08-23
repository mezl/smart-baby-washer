#!/usr/bin/env python3
"""Definitively test whether the controller is accepting load commands.

Status bit 6 is an INDICATOR -- it has tracked "loads ignored" in every
observation, but it is still an inference from a bit whose meaning nobody has
documented. This is the proof: command the intake motor and watch the flow
meter, the only load on this machine with a sensor on it.

    python3 tools/lockcheck.py [host]

Uses a few seconds of water. Intake only -- no heater bit is ever commanded --
and the override is released in a finally, so an exception cannot leave the
pump running.
"""
import json, sys, time, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "baby-washer.local"
B = f"http://{HOST}"
SECS = 20

def g():
    for _ in range(3):
        try:
            with urllib.request.urlopen(B + "/api/status", timeout=5) as f:
                return json.load(f)
        except Exception:
            time.sleep(0.3)
    return None

def p(path):
    for _ in range(3):
        try:
            r = urllib.request.Request(B + path, method="POST")
            with urllib.request.urlopen(r, timeout=5) as f:
                return True
        except Exception:
            time.sleep(0.3)
    return False

d = g()
if d is None:
    sys.exit("board unreachable")
if d["pb1"]:
    sys.exit(f"REFUSED: the panel is commanding 0x{d['pb1']:02X} — a cycle is running")

bit6 = bool(d["st_real"] & 0x40)
print(f"bit 6 says: {'LOCKED' if bit6 else 'clear'}   (byte3=0x{d['st_real']:02X}, {d['temp_real']} C)")
print(f"probing with a {SECS}s intake command...")

base = d["flow_real"]; peak = base
try:
    p("/api/mode_ovr?b2=2a&b3=07")        # smallest target the machine uses
    p("/api/panel_ovr?clr=FF&set=20")     # intake ONLY
    t0 = time.time()
    while time.time() - t0 < SECS:
        q = g()
        if q:
            peak = max(peak, q["flow_real"])
            if peak > base + 3:
                break
        time.sleep(1)
finally:
    p("/api/panel_ovr?clr=0&set=0")
    p("/api/mode_ovr?b2=&b3=")

responds = peak > base + 3
print(f"flow {base} -> {peak}")
print()
if responds:
    print("CONTROLLER IS ACCEPTING COMMANDS — safe to start a cycle")
else:
    print("CONTROLLER IS NOT RESPONDING — a cycle will do nothing")
print(f"bit 6 {'agreed' if bit6 != responds else 'DISAGREED'} with the probe")
sys.exit(0 if responds else 1)
