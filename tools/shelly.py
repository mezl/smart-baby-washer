#!/usr/bin/env python3
"""Shelly Gen2+ plug control — the D8's outlet after the Kasa swap (Aug 26).

    python3 tools/shelly.py [status|on|off|cycle [hold_s]] [ip]

Gen4 Plug US at 192.168.14.95, RPC over plain HTTP. initial_state=on is
configured on the device, so a mains outage restores power by itself --
the HS103's stranding failure mode no longer exists.
"""
import json, sys, time, urllib.request

IP = sys.argv[2] if len(sys.argv) > 2 else "192.168.14.95"

def rpc(method, **params):
    q = "&".join(f"{k}={str(v).lower() if isinstance(v,bool) else v}" for k, v in params.items())
    url = f"http://{IP}/rpc/{method}" + (f"?{q}" if q else "")
    with urllib.request.urlopen(url, timeout=6) as r:
        return json.load(r)

def status():
    s = rpc("Switch.GetStatus", id=0)
    print(f"output={'ON' if s['output'] else 'OFF'}  {s['apower']}W  {s['voltage']}V  {s['current']}A")
    return s

cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
if cmd == "status":
    status()
elif cmd == "on":
    rpc("Switch.Set", id=0, on=True); status()
elif cmd == "off":
    rpc("Switch.Set", id=0, on=False); status()
elif cmd == "cycle":
    hold = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 6
    rpc("Switch.Set", id=0, on=False)
    print(f"off, holding {hold}s..."); time.sleep(hold)
    rpc("Switch.Set", id=0, on=True); status()
