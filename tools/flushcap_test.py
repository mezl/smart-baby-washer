#!/usr/bin/env python3
"""Exercise the untargeted-flush cap without moving a drop of water.

SPOOF feeds the controller a permanently idle panel frame, so nothing the
overrides say can reach it and no load can be energised. The masks are still
applied to the real panel stream on the way past, and that rewritten value is
what the cap watches -- so the whole decision path runs while the machine sits
inert. That is the only reason this test is safe to run on a plumbed-in machine.

    python3 tools/flushcap_test.py baby-washer.local

Checks, in order:
  1. a TARGETED fill is never capped, however long it runs   (negative control)
  2. an untargeted 0xFF flush trips the cap at the set time
  3. the forwarded byte 1 loses INTAKE and keeps DRAIN
  4. releasing the flush clears the hold and re-arms it

Exits non-zero on the first failure, and always puts the link back the way it
found it.
"""
import json, sys, time, urllib.parse, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "baby-washer.local"
BASE = f"http://{HOST}"
CAP_MS = 5000
DRAIN, INTAKE = 0x02, 0x20
fails = []


def req(path, method="GET"):
    r = urllib.request.Request(BASE + path, method=method)
    with urllib.request.urlopen(r, timeout=8) as f:
        body = f.read().decode()
    return json.loads(body) if body.startswith("{") else body


def st():
    return req("/api/status")


def check(ok, what, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {what}{'  -- ' + detail if detail else ''}")
    if not ok:
        fails.append(what)
    return ok


def clear():
    req("/api/panel_ovr?clr=0&set=0", "POST")
    req("/api/mode_ovr?b2=&b3=", "POST")
    req("/api/spoof?v=off", "POST")


def hold_flush(b2, b3, secs, label):
    """Command byte1 = drain+intake with the given target pair, and watch.

    byte 2 is never independent of byte 3 on this machine, so each case sends
    the real observed pair rather than an invented one.
    """
    req(f"/api/mode_ovr?b2={b2:02x}&b3={b3:02x}", "POST")
    req(f"/api/panel_ovr?clr=0&set={DRAIN | INTAKE:02x}", "POST")
    t0 = time.time()
    seen_on = seen_cap = False
    fwd_at_cap = None
    while time.time() - t0 < secs:
        s = st()
        if s["flush_on"]:
            seen_on = True
        if s["flush_cap"] and not seen_cap:
            seen_cap = True
            fwd_at_cap = s["pb1_fwd"]
            print(f"    {label}: capped at {s['flush_ms']} ms, "
                  f"forwarded byte1 = 0x{fwd_at_cap:02X}")
        time.sleep(0.4)
    return seen_on, seen_cap, fwd_at_cap


print(f"flush-cap test against {HOST}")
base = st()
if base["pb1"] or base.get("cyc_state") == 1:
    sys.exit(f"ABORT: the machine is not idle (pb1=0x{base['pb1']:02X}). "
             "Run this with nothing going on.")
orig_cap = base["fcap_ms"]
print(f"  machine idle, cap currently {orig_cap} ms\n")

try:
    # SPOOF FIRST. Nothing below may run until the controller is deafened.
    req("/api/spoof?v=on", "POST")
    s = st()
    if not s["spoof"]:
        sys.exit("ABORT: spoof did not engage — refusing to command any load.")
    print(f"  spoof engaged, controller is being fed {s['spoof_frame']}\n")

    req(f"/api/flushcap?ms={CAP_MS}", "POST")
    n0 = st()["flush_n"]

    print(f"1. targeted fill (byte3 0x20) held {CAP_MS // 1000 * 2} s "
          "— must NOT cap")
    on, cap, _ = hold_flush(0x40, 0x20, CAP_MS / 1000 * 2, "targeted")
    check(not on, "a targeted fill is not seen as a flush")
    check(not cap, "a targeted fill is never capped")
    req("/api/panel_ovr?clr=0&set=0", "POST")
    time.sleep(0.8)

    print(f"\n2. untargeted flush (byte3 0xFF) — must cap at {CAP_MS} ms")
    on, cap, fwd = hold_flush(0xFF, 0xFF, CAP_MS / 1000 * 2, "untargeted")
    check(on, "the flush was detected")
    check(cap, "the cap fired")
    if fwd is not None:
        check(not (fwd & INTAKE), "INTAKE stripped from the forwarded byte 1",
              f"0x{fwd:02X}")
        check(bool(fwd & DRAIN), "DRAIN left set — this is what ends the stage",
              f"0x{fwd:02X}")
    n1 = st()["flush_n"]
    check(n1 == n0 + 1, "fired exactly once", f"{n0} -> {n1}")

    print("\n3. release")
    req("/api/panel_ovr?clr=0&set=0", "POST")
    req("/api/mode_ovr?b2=&b3=", "POST")
    time.sleep(1.2)
    s = st()
    check(not s["flush_on"], "flush cleared")
    check(not s["flush_cap"], "hold released")
finally:
    clear()
    req(f"/api/flushcap?ms={orig_cap}", "POST")
    s = st()
    print(f"\nrestored: spoof={s['spoof']} pb1_fwd=0x{s['pb1_fwd']:02X} "
          f"cap={s['fcap_ms']} ms")

print("\n" + ("FAILED: " + ", ".join(fails) if fails else "all checks passed"))
sys.exit(1 if fails else 0)
