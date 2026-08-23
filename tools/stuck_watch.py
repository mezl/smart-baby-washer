#!/usr/bin/env python3
"""Host-side stuck-load watchdog: power-cycle the D8 when a load is stranded.

The controller locks out on status bit 6 and then ignores the panel's
end-of-cycle release, leaving the blower and air heater energised. Nothing on
the CN2 link can stop that -- the panel is already commanding nothing.

The ESP32 has its own version of this, but it can only cut mains and stay off:
it is powered by what it switches, so it cannot turn the plug back on. This
runs on a machine that is not, so it can do a real cycle. It deliberately acts
sooner than the on-board one, leaving that as the backstop if this host is down.

    python3 tools/stuck_watch.py [host] [plug_ip]
"""
import json, sys, time, urllib.request
sys.path.insert(0, "/tmp/claude-1000/-home-kai/75cadcaa-ea43-4cc3-b05f-8a5941ae3849/scratchpad")
from kasa import call

HOST  = sys.argv[1] if len(sys.argv) > 1 else "baby-washer.local"
PLUG  = sys.argv[2] if len(sys.argv) > 2 else "192.168.14.123"
DWELL = 120      # seconds all conditions must hold
HOT_C = 40       # below this, not worth cutting mains
OFF_S = 15       # how long to hold it off
MIN_GAP = 600    # never cycle more often than this

def st():
    try:
        with urllib.request.urlopen(f"http://{HOST}/api/status", timeout=5) as f:
            return json.load(f)
    except Exception:
        return None

def relay(state):
    try:
        call(PLUG, {"system": {"set_relay_state": {"state": state}}})
        return True
    except Exception as e:
        print(f"  plug error: {e}", flush=True)
        return False

def stamp():
    return time.strftime("%H:%M:%S")

print(f"[{stamp()}] watching {HOST}, plug {PLUG}", flush=True)
print(f"  trigger: panel idle + bit6 + >={HOT_C}C + not cooling, for {DWELL}s", flush=True)

armed_at = None
peak = 0
last_cycle = 0
while True:
    d = st()
    if d is None:
        armed_at = None
        time.sleep(10); continue

    idle   = d["pb1"] == 0
    locked = bool(d["st_real"] & 0x40)
    temp   = d["temp_real"]
    hot    = temp >= HOT_C

    if not (idle and locked and hot):
        if armed_at: print(f"[{stamp()}] disarmed (pb1=0x{d['pb1']:02X} bit6={locked} temp={temp})", flush=True)
        armed_at = None
        time.sleep(10); continue

    if armed_at is None:
        armed_at = time.time(); peak = temp
        print(f"[{stamp()}] ARMED — panel idle, bit6 set, {temp}C", flush=True)
        time.sleep(10); continue

    peak = max(peak, temp)
    if temp + 2 <= peak:                       # genuinely cooling: not stuck
        print(f"[{stamp()}] cooling ({peak}->{temp}C) — disarmed", flush=True)
        armed_at = None
        time.sleep(10); continue

    held = time.time() - armed_at
    if held >= DWELL and time.time() - last_cycle > MIN_GAP:
        print(f"[{stamp()}] STUCK LOAD — {temp}C held {held:.0f}s with the panel idle. "
              f"power-cycling.", flush=True)
        relay(0)
        time.sleep(OFF_S)
        relay(1)
        last_cycle = time.time()
        armed_at = None
        for i in range(40):
            if st(): print(f"[{stamp()}] machine back up", flush=True); break
            time.sleep(3)
    time.sleep(10)
