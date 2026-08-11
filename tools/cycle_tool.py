#!/usr/bin/env python3
"""Load and save the D8's user programs as a JSON file.

    python3 cycle_tool.py dump                 # device -> JSON on stdout
    python3 cycle_tool.py dump > cycles.json
    python3 cycle_tool.py load cycles.json     # JSON -> device
    python3 cycle_tool.py load cycles.json --replace   # also clear slots not listed
    python3 cycle_tool.py check cycles.json    # parse and validate locally, no device
    python3 cycle_tool.py pack  "drain 20s" "fill 90" "wash+heat 5m"   # -> canonical

A program is a name and a list of stages. Each stage is a load set, an optional
amount, and an optional duration:

    drain 20s | fill 90 | wash+heat 5m | steam 7m | flush 70s | dry 10m

The device stores a fixed-width canonical form, 7 characters per stage:

    DRAN20S FILL90P WASH05M     ->  DRAN20S,FILL90P,WASH05M
    CODE nnU = 4-letter code, 2 digits, unit S/M/H or P for a fill percentage

The device does the real validation and its answer is authoritative -- `check`
runs the same rules here so a mistake is caught before anything is written, not
because the two are expected to disagree.
"""
import argparse, json, sys, urllib.parse, urllib.request

DEF_HOST = "baby-washer.local"
SLOTS = 6

# Keep in step with KW[] in firmware/src/cn2.cpp.
KW = {"wash": 0x01, "pump": 0x01, "drain": 0x02, "heat": 0x04, "waterheat": 0x04,
      "airheat": 0x08, "dry": 0x18, "blower": 0x10, "intake": 0x20,
      "steam": 0x04, "wait": 0x00}


# Canonical: 7 characters per stage, CODEnnU. Keep in step with MN[] in
# firmware/src/cn2.cpp.
MN = {"DRAN": 0x02, "FILL": 0x20, "PUMP": 0x01, "WASH": 0x05, "STEM": 0x04,
      "AIRH": 0x08, "BLOW": 0x10, "DRYR": 0x18, "FLSH": 0x22, "WAIT": 0x00}
WORD = {"DRAN": "drain", "PUMP": "wash", "WASH": "wash+heat", "STEM": "steam",
        "AIRH": "airheat", "BLOW": "blower", "DRYR": "dry", "WAIT": "wait"}
# A byte covers a range of counts, so the inverse is ambiguous. The four the
# machine actually uses map back to the percentages that produce them.
INV = {0x07: 20, 0x1C: 80, 0x20: 90, 0x23: 99}


def code_of(L, T):
    if T == 0xFF:            return "FLSH"
    if L & 0x20:             return "FILL"
    if (L & 0x18) == 0x18:   return "DRYR"
    if L & 0x10:             return "BLOW"
    if L & 0x08:             return "AIRH"
    if L & 0x05:             return "WASH" if (L & 0x05) == 0x05 else ("PUMP" if L & 0x01 else "STEM")
    if L & 0x02:             return "DRAN"
    return "WAIT"


def to_compact(stages):
    """Readable stages -> the canonical CODEnnU form."""
    out = []
    for text in stages:
        L, T, S = parse_stage(text)
        c = code_of(L, T)
        if c == "FILL":
            pct = INV.get(T, max(0, (T * 20 - 10 + 6) // 7))
            out.append("%s%02dP" % (c, min(pct, 99)))
            continue
        # Largest unit that keeps the value under 100, so 20s stays 20S and an
        # hour is 01H rather than something that will not fit.
        v, u = S, "S"
        if v >= 6000:   v, u = (v + 1800) // 3600, "H"
        elif v >= 100:  v, u = (v + 30) // 60, "M"
        out.append("%s%02d%s" % (c, min(v, 99), u))
    # Commas on output; the device accepts them either way.
    return ",".join(out)


def from_compact(spec):
    """Canonical form -> readable stages. Commas are optional."""
    spec = spec.replace(",", "")
    if len(spec) % 7:
        raise ValueError("canonical form is 7 chars per stage (CODEnnU)")
    out = []
    for i in range(0, len(spec), 7):
        c, v, u = spec[i:i+4].upper(), int(spec[i+4:i+6]), spec[i+6].upper()
        if c not in MN:
            raise ValueError(f"unknown code {c!r}")
        if u == "P":
            out.append(f"fill {v}")
        else:
            secs = v * (60 if u == "M" else 3600 if u == "H" else 1)
            nm = "flush" if c == "FLSH" else WORD[c]
            out.append(f"{nm} {secs}s" if secs else nm)
    return out


def parse_stage(text):
    """One stage -> (loads, target, seconds). Raises ValueError on a bad word."""
    loads = target = secs = 0
    isfill = False
    for tok in text.lower().split():
        if tok == "fill":
            loads |= 0x20; isfill = True
        elif tok == "flush":
            loads |= 0x22; target = 0xFF
        elif tok[0].isdigit():
            n = int("".join(c for c in tok if c.isdigit()))
            if isfill and not target:
                # Integer, matching the firmware. round(n * 0.35) is wrong in
                # float: 90 * 0.35 is 31.499999999999996, so it gives 31 where
                # the machine's own value for 90 counts is 32. 0.35 is 7/20.
                target = (n * 7 + 10) // 20       # counts -> byte 3
            else:
                secs = n * (60 if tok.endswith("m") else 3600 if tok.endswith("h") else 1)
        else:
            for part in tok.split("+"):
                if part not in KW:
                    raise ValueError(f"unknown word {part!r} in stage {text!r}")
                loads |= KW[part]
    if not loads and not secs:
        raise ValueError(f"stage {text!r} does nothing")
    return loads, target, secs


def validate(stages):
    """The device's rules, run locally. Returns a reason or None."""
    water = False
    for text in stages:
        L, T, S = parse_stage(text)
        if L & 0xC0:
            return "bits 6 and 7 have never been seen set"
        if (L & 0x20) and not T:
            return f"{text!r}: intake with no fill target does nothing"
        if T == 0xFF and not (L & 0x02):
            return f"{text!r}: an untargeted flush needs the drain open"
        if (L & 0x20) and T == 0xFF and not S:
            return f"{text!r}: an untargeted flush needs a duration"
        if not L and not S:
            return f"{text!r}: a stage with no loads needs a duration"
        if (L & 0x04) and not water:
            return f"{text!r}: water heater before any fill — dry fire"
        if L & 0x20:
            water = True
        if L & 0x02:
            water = False
    return None


def post(host, path):
    req = urllib.request.Request(f"http://{host}{path}", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=6) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:            # 400 carries the reason
        return json.loads(e.read().decode())


def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=6) as r:
        return json.loads(r.read().decode())


def cmd_dump(a):
    cur = get(a.host, "/api/custom")
    out = {"version": 1, "programs": []}
    for i, s in enumerate(cur["slots"]):
        if not s["name"]:
            continue
        stages = from_compact(s["spec"])
        out["programs"].append({"slot": i, "name": s["name"], "stages": stages})
    json.dump(out, sys.stdout, indent=2)
    print()
    return 0


def cmd_check(a, quiet=False):
    doc = json.load(open(a.file))
    bad = 0
    for p in doc.get("programs", []):
        why = None
        try:
            why = validate(p["stages"])
        except ValueError as e:
            why = str(e)
        if why:
            print(f"  ✗ slot {p.get('slot','?')} {p.get('name','?')!r}: {why}")
            bad += 1
        elif not quiet:
            print(f"  ✓ slot {p.get('slot','?')} {p.get('name','?')!r}: "
                  f"{len(p['stages'])} stages")
    if bad:
        print(f"{bad} program(s) would be rejected")
    return 1 if bad else 0


def cmd_load(a):
    if cmd_check(a, quiet=True):
        print("nothing written")
        return 1
    doc = json.load(open(a.file))
    used = set()
    for p in doc.get("programs", []):
        slot = int(p["slot"])
        used.add(slot)
        # Send the canonical compact form: fixed width, hex only, nothing that
        # needs URL-encoding, and a length that catches truncation on its own.
        q = urllib.parse.urlencode({"slot": slot, "name": p["name"],
                                    "stages": to_compact(p["stages"])})
        r = post(a.host, "/api/custom?" + q)
        if r.get("ok"):
            print(f"  slot {slot}: {p['name']} — {len(p['stages'])} stages")
        else:
            # The device is the authority; if it disagrees with us, say so loudly.
            print(f"  slot {slot}: REJECTED BY DEVICE — {r.get('err')}")
    if a.replace:
        for slot in range(SLOTS):
            if slot not in used:
                post(a.host, f"/api/cycle?del={slot}")
        print(f"  cleared slots not listed: {sorted(set(range(SLOTS)) - used)}")
    return 0


def cmd_pack(a):
    why = validate(a.stages)
    if why:
        print(f"rejected: {why}")
        return 1
    print(to_compact(a.stages))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEF_HOST)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("dump")
    sp = sub.add_parser("pack", help="readable stages -> the canonical form")
    sp.add_argument("stages", nargs="+")
    for name in ("load", "check"):
        s = sub.add_parser(name)
        s.add_argument("file")
        if name == "load":
            s.add_argument("--replace", action="store_true",
                           help="delete any slot the file does not mention")
    a = ap.parse_args()
    return {"dump": cmd_dump, "load": cmd_load, "check": cmd_check,
            "pack": cmd_pack}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
