#!/usr/bin/env python3
"""Wait for the controller to cool enough to unlock, then clear it.

The lockout (status bit 6) is TEMPERATURE-gated, not time-gated. Measured:
60 s of mains-off cleared it at 33-35 C; 60, 90 and 150 s all failed at 45 C.
So there is no cut long enough to fix a hot machine -- it has to cool first.

Once clear, the fastest way to cool it further is the machine's own plumbing:
drain, draw cold water, repeat. That needs a working controller, which is why
it cannot be the first step.
"""
import json, sys, time, urllib.request
sys.path.insert(0, "/tmp/claude-1000/-home-kai/75cadcaa-ea43-4cc3-b05f-8a5941ae3849/scratchpad")
from kasa import call

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.14.13"
PLUG = sys.argv[2] if len(sys.argv) > 2 else "192.168.14.123"
# Refined by measurement, not guessed: 60 s cleared it at 33-35 C; 60/90 s
# failed at 39 C and 60/90/150 s failed at 45 C. So the gate is temperature and
# it sits below ~36 C -- no cut is long enough above that.
TRY_BELOW = 36
OFF_S     = 60

def st():
    try:
        with urllib.request.urlopen(f"http://{HOST}/api/status", timeout=5) as f:
            return json.load(f)
    except Exception:
        return None
def relay(s): call(PLUG, {"system": {"set_relay_state": {"state": s}}})
def now(): return time.strftime("%H:%M:%S")

print(f"[{now()}] waiting for the machine to cool below {TRY_BELOW} C, then unlocking", flush=True)
last_try = 0
while True:
    d = st()
    if d is None:
        time.sleep(15); continue
    locked = bool(d["st_real"] & 0x40)
    if not locked:
        print(f"[{now()}] CLEAR at {d['temp_real']} C — machine will respond now", flush=True)
        break
    if d["pb1"]:
        print(f"[{now()}] loads commanded (0x{d['pb1']:02X}) — standing off", flush=True)
        time.sleep(30); continue
    if d["temp_real"] >= TRY_BELOW:
        print(f"[{now()}] {d['temp_real']} C — too warm to unlock, waiting", flush=True)
        time.sleep(120); continue
    if time.time() - last_try < 300:
        time.sleep(30); continue
    print(f"[{now()}] {d['temp_real']} C — attempting a {OFF_S}s cut", flush=True)
    last_try = time.time()
    # ALWAYS restore, even if this process is killed mid-cut. Without the
    # finally, killing the watcher during the sleep leaves the appliance dead
    # with no one watching -- which is exactly what happened once.
    try:
        relay(0); time.sleep(OFF_S)
    finally:
        relay(1)
    t0 = time.time()
    while time.time() - t0 < 90 and st() is None:
        time.sleep(3)
    for _ in range(16):
        q = st()
        if q and (q["st_real"] & 0x40): break
        time.sleep(5)
    q = st()
    if q:
        print(f"[{now()}]   -> byte3=0x{q['st_real']:02X} at {q['temp_real']} C", flush=True)
