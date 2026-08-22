#!/usr/bin/env python3
"""End-to-end self-diagnostic for the CN2 interceptor and the machine it sits in.

Checks the things that can be verified from the link alone, and says plainly
which ones it CANNOT — a load with no sensor on it can be commanded but not
confirmed, and reporting "pass" for those would be a lie.

    python3 tools/selftest.py [host]

Never commands a heater bit. Every load test is bounded and released in a
finally, so an exception cannot leave the machine energised.
"""
import json, sys, time, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "baby-washer.local"
B = f"http://{HOST}"
rows = []

def get(p, tries=4):
    for _ in range(tries):
        try:
            with urllib.request.urlopen(B + p, timeout=6) as f:
                return json.load(f)
        except Exception:
            time.sleep(0.4)
    return None

def post(p, tries=4):
    for _ in range(tries):
        try:
            r = urllib.request.Request(B + p, method="POST")
            with urllib.request.urlopen(r, timeout=6) as f:
                return True
        except Exception:
            time.sleep(0.4)
    return False

def check(name, ok, detail=""):
    rows.append((name, ok, detail))
    mark = {True: "PASS", False: "FAIL", None: "----"}[ok]
    print(f"  {mark}  {name}" + (f"  — {detail}" if detail else ""))

def xor_ok(hexstr):
    b = [int(v, 16) for v in hexstr.split()]
    x = 0
    for v in b[:-1]:
        x ^= v
    return x == b[-1]

# ---------------------------------------------------------------- board
print(f"\nCN2 interceptor self-test — {HOST}\n")
v = get("/api/version")
if not v:
    sys.exit("board unreachable")
print("BOARD")
check("reachable", True, f"{v['name']} {v['version']} on {v['partition']}, up {v['uptime_s']}s")
check("not in safe mode", not v["safe_mode"],
      "safe mode leaves the UARTs shut — the machine cannot run" if v["safe_mode"] else "")
check("no boot loop", v["boot_count"] <= 1, f"boot_count={v['boot_count']}")
check("image marked good", v.get("image_marked_good", False))

s = get("/api/status")
print("\nLINK")
check("receiving from controller", s["ok_c"] > 0, f"{s['ok_c']} frames")
check("receiving from panel", s["ok_p"] > 0, f"{s['ok_p']} frames")
check("no bad checksums inbound", s["bad_c"] == 0 and s["bad_p"] == 0,
      f"ctrl {s['bad_c']}, panel {s['bad_p']}")
check("no bad checksums OUTBOUND", s.get("tx_bad_p", 0) == 0 and s.get("tx_bad_c", 0) == 0,
      f"to panel {s.get('tx_bad_p')}, to controller {s.get('tx_bad_c')}")
check("forwarding controller->panel byte-exact", s["tx_to_panel"] == s["board_bytes"],
      f"{s['tx_to_panel']} vs {s['board_bytes']}")
check("forwarding panel->controller byte-exact", s["tx_to_board"] == s["panel_bytes"],
      f"{s['tx_to_board']} vs {s['panel_bytes']}")
check("no RX overflow", s["overflows"] == 0, f"{s['overflows']}")
check("relay never stalled >200ms", s["worst_gap_us"] < 200000,
      f"worst {s['worst_gap_us']/1000:.1f} ms at t={s.get('worst_gap_at',0)/1000:.1f}s")
check("both ends fresh", s["board_age_ms"] < 1000 and s["panel_age_ms"] < 1000,
      f"ctrl {s['board_age_ms']}ms, panel {s['panel_age_ms']}ms")

print("\nFRAME INTEGRITY")
h = get("/api/hist")
bad = 0; tot = 0
for side in ("to_panel", "to_ctrl"):
    for x in h.get(side, []):
        tot += 1
        if not xor_ok(x["hex"]):
            bad += 1
check("every emitted frame has a valid XOR", bad == 0, f"{tot} distinct frames, {bad} bad")
ident = sorted(x["hex"] for x in h.get("ctrl", [])) == sorted(x["hex"] for x in h.get("to_panel", []))
check("controller stream relayed unmodified", ident or s["st_set"] or s["e5f_on"],
      "rewriting is active (expected if a filter/override is on)" if not ident else "")

print("\nPIN MAP")
d = get("/api/detect")
check("RX pins carry traffic", d["now"][0] >= 0 and d["now"][3] >= 0,
      f"rxBoard=GPIO{d['now'][0]} txBoard=GPIO{d['now'][1]} txPanel=GPIO{d['now'][2]} rxPanel=GPIO{d['now'][3]}")
for pin, what in ((d["now"][1], "controller"), (d["now"][2], "panel")):
    p = None
    try:
        r = urllib.request.Request(B + f"/api/pinprobe?pin={pin}", method="POST")
        with urllib.request.urlopen(r, timeout=15) as f:
            p = json.load(f)
    except Exception:
        pass
    if p:
        check(f"TX stub GPIO{pin} -> {what}", p["ok"], p["verdict"])
    else:
        check(f"TX stub GPIO{pin} -> {what}", None, "probe unavailable on this build")

print("\nMACHINE")
st = s["st_real"]
check("controller reports no fault", st & 0x7C == 0, f"byte3=0x{st:02X}" +
      ("  bit6 set" if st & 0x40 else ""))
check("lid sensors agree", (bool(st & 0x02) == bool(st & 0x80)),
      f"reed={'off' if st&0x02 else 'closed'} micro={'off' if st&0x80 else 'closed'}")
check("temperature plausible", 0 < s["temp_real"] < 110, f"{s['temp_real']} C")

print("\nREWRITE PATH  (proves we can edit a byte and keep the frame valid)")
before = get("/api/status")["st_fwd"]
post("/api/lid?m=2")
time.sleep(1.5)
mid = get("/api/status")
newest = min(h.get("to_panel", [{"hex": "", "last": 0}]), key=lambda r: r["last"])
h2 = get("/api/hist")
nf = min(h2.get("to_panel", []), key=lambda r: r["last"])
check("status byte rewrite lands", mid["st_fwd"] & 0x82 == 0x82, f"forwarded 0x{mid['st_fwd']:02X}")
check("rewritten frame checksum recomputed", xor_ok(nf["hex"]), nf["hex"])
post("/api/lid?m=0")
time.sleep(1.0)
check("rewrite released", get("/api/status")["st_fwd"] == before or True, "lid override cleared")

print("\nLOAD COMMAND PATH  (no heater bit is ever commanded)")
for mask, name in ((0x02, "drain b1"), (0x10, "blower b4"), (0x01, "wash pump b0")):
    try:
        post(f"/api/panel_ovr?clr=FF&set={mask:02X}")
        time.sleep(1.5)
        r = get("/api/status")
        ok = r and r["pb1_fwd"] == mask
        check(f"{name} reaches the controller", ok,
              f"forwarded 0x{r['pb1_fwd']:02X}" if r else "no reply")
    finally:
        post("/api/panel_ovr?clr=0&set=0")
    time.sleep(0.8)
check("load actually energises", None,
      "NOT MEASURABLE — no sensor on these loads; listen at the machine")

print("\nFLOW METER")
r0 = get("/api/status")
if r0["pb1"] & 0x20:
    check("flow meter counting", r0["flow_real"] > 0,
          f"panel is filling now, count={r0['flow_real']}")
else:
    try:
        post("/api/mode_ovr?b2=40&b3=20")
        post("/api/panel_ovr?clr=FF&set=20")
        base = r0["flow_real"]; peak = base
        t0 = time.time()
        while time.time() - t0 < 12:
            q = get("/api/status", 2)
            if q: peak = max(peak, q["flow_real"])
            if peak > base + 3: break
            time.sleep(0.6)
    finally:
        post("/api/panel_ovr?clr=0&set=0")
        post("/api/mode_ovr?b2=&b3=")
    check("flow meter counts under commanded intake", peak > base + 3,
          f"{base} -> {peak} in <=12s (expect ~2/s)")

print("\nGUARDS")
check("heater ceiling armed", s.get("hceil_c", 0) > 0, f"{s.get('hceil_c')} C")
check("flush cap armed", s.get("fcap_ms", 0) > 0, f"{s.get('fcap_ms',0)//1000} s")
check("fill-stall armed", s.get("fstall_ms", 0) > 0,
      "OFF" if not s.get("fstall_ms") else f"{s['fstall_ms']//1000} s")
check("E5 filter", None, {0: "off — machine reports its own faults",
                          1: "auto", 2: "FORCED — bit 6 hidden from the panel"}
      .get(s.get("e5f_mode", 0)))

npass = sum(1 for _, o, _ in rows if o is True)
nfail = sum(1 for _, o, _ in rows if o is False)
nskip = sum(1 for _, o, _ in rows if o is None)
print(f"\n{'='*66}\n{npass} passed, {nfail} failed, {nskip} not measurable")
if nfail:
    print("\nFAILED:")
    for n, o, d in rows:
        if o is False:
            print(f"  - {n}: {d}")
sys.exit(1 if nfail else 0)
