#!/usr/bin/env python3
"""Continuous D8 telemetry logger. Runs on HA, kept alive by the watchdog.

One compact JSON line every 2 s while the board answers: temperature, flow,
panel command, fill target, status byte, lock duration, cycle state. Daily
files, 14-day retention. ~4 MB/day worst case. Exists because a full cycle
completed with nothing recording -- never again.
"""
import io, json, os, time, urllib.request

ESP  = os.environ.get("D8_ESP", "192.168.14.13")
DIR  = "/config/d8/logs"
KEEP = 14

os.makedirs(DIR, exist_ok=True)

def prune():
    files = sorted(f for f in os.listdir(DIR) if f.startswith("status-"))
    for f in files[:-KEEP]:
        try: os.remove(os.path.join(DIR, f))
        except OSError: pass

last_day = ""
while True:
    t0 = time.time()
    try:
        with urllib.request.urlopen(f"http://{ESP}/api/status", timeout=4) as r:
            d = json.load(r)
        day = time.strftime("%Y%m%d")
        if day != last_day:
            last_day = day; prune()
        row = {"t": round(t0, 1)}
        for k in ("temp_real","flow_real","pb1","pb3","st_real","locked_ms",
                  "state","stage","wire","uptime_s","bad_c","bad_p"):
            if k in d: row[k] = d[k]
        with io.open(f"{DIR}/status-{day}.jsonl", "a") as f:
            f.write(json.dumps(row, separators=(",",":")) + "\n")
    except Exception:
        pass                      # board down: log nothing, keep trying
    time.sleep(max(0.5, 2 - (time.time() - t0)))
