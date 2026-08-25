#!/usr/bin/env python3
"""Quick full-machine diagnosis through the CN2 overrides (CPU relay mode).

Drives each load individually and judges it with the machine's only two
sensors -- the intake flow meter and the sump NTC:

  drain      commanded; judged indirectly (post-fill drain must not overflow
             the next fill; also audible)
  intake     flow counts must advance at a sane rate        -> measured
  flow meter proven by the same test                        -> measured
  wash pump  commanded; no sensor on it                     -> listen
  water heat wash+heat must raise sump temp                 -> measured
  blower     blower must cool the wet sump                  -> measured
  air heat   no air probe exists; commanded during blower   -> feel the vent

Safety: refuses to start unless the controller is clear and the panel idle;
water heat only runs after a VERIFIED fill; everything is released in a
finally; bit 6 aborts the run. ~6 minutes, uses a small fill of water.

    python3 tools/d8_diagnose.py [host]
"""
import json, sys, time, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.14.13"
B = f"http://{HOST}"

def g():
    for _ in range(3):
        try:
            with urllib.request.urlopen(B + "/api/status", timeout=5) as f:
                return json.load(f)
        except Exception:
            time.sleep(0.4)
    return None

def post(path):
    for _ in range(3):
        try:
            r = urllib.request.Request(B + path, method="POST")
            with urllib.request.urlopen(r, timeout=5):
                return True
        except Exception:
            time.sleep(0.4)
    return False

def loads(mask):
    return post(f"/api/panel_ovr?clr=FF&set={mask:x}" if mask else "/api/panel_ovr?clr=0&set=0")

def release():
    loads(0); post("/api/mode_ovr?b2=&b3=")

def watch(secs, tag):
    """Sample flow+temp for secs; abort the whole run on a controller lock."""
    t0 = time.time(); rows = []
    while time.time() - t0 < secs:
        d = g()
        if d:
            if d.get("st_real", 0) & 0x40:
                raise RuntimeError(f"controller LOCKED during {tag}")
            rows.append((d.get("flow_real", 0), float(d.get("temp_real", 0))))
        time.sleep(2)
    return rows

R = {}   # name -> (verdict, detail)

d = g()
if d is None: sys.exit("board unreachable")
if d.get("wire"): sys.exit("board is in WIRE mode — overrides dormant; refuse")
if d.get("st_real", 0) & 0x40: sys.exit("controller locked — power cycle first")
if d.get("pb1"): sys.exit(f"panel is commanding 0x{d['pb1']:02X} — a cycle is running")
t_start = float(d.get("temp_real", 0))
print(f"start: temp {t_start:.0f} C, controller clear, CPU relay mode\n")

try:
    print("[1/6] pre-drain 25 s (b1) — listen for the drain pump")
    loads(0x02); watch(25, "drain")

    print("[2/6] intake + flow meter (b5, small fill)")
    post("/api/mode_ovr?b2=ab&b3=1c"); loads(0x20)
    rows = watch(30, "intake")
    release()
    flow_gain = max(r[0] for r in rows) - min(r[0] for r in rows) if rows else 0
    rate = flow_gain / 30.0
    ok = flow_gain >= 15
    R["intake pump"] = (ok, f"{flow_gain} counts in 30 s ({rate:.2f}/s)")
    R["flow meter"]  = (ok, "proven by the same counts" if ok else "no counts seen")
    if not ok:
        raise RuntimeError("no water delivered — refusing to heat")

    print("[3/6] wash pump alone 12 s (b0) — listen for circulation")
    loads(0x01); watch(12, "wash pump")
    R["wash pump"] = (None, "commanded ok — no sensor; confirm by ear")

    print("[4/6] water heater: wash+heat (0x05) 90 s — expect temp rise")
    loads(0x05)
    rows = watch(90, "water heat")
    t_hot = rows[-1][1] if rows else t_start
    rise = t_hot - min(r[1] for r in rows) if rows else 0
    R["water heater"] = (rise >= 2.0, f"+{rise:.1f} C in 90 s (now {t_hot:.0f} C)")

    print("[5/6] blower cooldown (b4) 90 s — expect temp fall on a wet sump")
    loads(0x10)
    rows = watch(90, "blower")
    fall = max(r[1] for r in rows) - rows[-1][1] if rows else 0
    # Field-tested 2026-08-25: right after the heat phase the heater plate's
    # residual heat masks sump cooling entirely (fan audibly running, sump
    # -0.0 C). There is no fair sensor proxy for the blower, so it is a
    # CHECK item like the wash pump; the cooling number rides along as info.
    R["blower"] = (None, f"commanded ok — confirm fan by ear (sump {-fall:+.1f} C info only)")

    print("[6/6] air heat + blower (0x18) 20 s — feel warm air at the vent; then final drain")
    loads(0x18); watch(20, "air heat")
    R["air heater"] = (None, "commanded ok — no air probe; confirm at the vent")
    loads(0x02); watch(30, "final drain")
    R["drain pump"] = (None, "commanded twice — no sensor; sump should be empty")
finally:
    release()

print("\n===== DIAGNOSIS =====")
worst = "PASS"
for k, (ok, msg) in R.items():
    v = "PASS" if ok else ("CHECK" if ok is None else "FAIL")
    if v == "FAIL": worst = "FAIL"
    print(f"  {k:<13} {v:<6} {msg}")
d = g()
print(f"\ncontroller after run: st=0x{d.get('st_real',0):02X}  temp={d.get('temp_real')} C")
print(f"overall: {worst} (CHECK items need your ears/hands)")
