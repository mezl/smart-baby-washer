#!/usr/bin/env python3
"""Log a full D8 cycle to disk. Nothing is stored on the ESP32.

The firmware keeps a 4096-event byte ring, which is about 63 seconds of traffic,
and a 24-entry run-length history per direction. A wash cycle runs 20-40 minutes,
so the ring wraps roughly 38 times and a reboot loses everything. If you want a
record of a cycle, it has to be captured from the host while the cycle runs.

    python3 cycle_log.py                        # logs until Ctrl-C
    python3 cycle_log.py --out mycycle.jsonl
    python3 cycle_log.py --host d8-sniffer.local --hz 5

Writes two files side by side:

    <name>.jsonl   one object per distinct STATE
    <name>.log     the same thing as readable lines, for scrolling through live

The log is run-length compressed: identical consecutive samples do not each get a
line. One line is written when a state first appears and then rewritten in place
as long as that state holds, so it carries `t` (first seen), `t_end` (last seen),
`dur` and `n` (samples). A machine sitting still for twenty minutes is one line
that keeps growing, not six thousand identical ones.

The in-place rewrite always targets the last line of the file, so it is a seek,
truncate and write -- and because it is refreshed at most once a second, a crash
or a power cut costs at most one second of `t_end` on the final run. Every
completed run is already durable.

Network drops are recorded rather than skipped: a gap in a cycle record is itself
evidence, so it is written down instead of silently waited out.

    python3 cycle_log.py --compact old.jsonl    # compress an existing log
"""
import argparse, json, os, sys, time
import urllib.request, urllib.error

B1 = {0: "washpump", 1: "drain", 2: "waterheat", 3: "airheat",
      4: "dry(blower+heat)", 5: "intake", 6: "b6?", 7: "b7?"}


def bits(v, names):
    return [names[i] for i in range(8) if v & (1 << i)]


def decode(d):
    st, pb1 = d.get("st_real", 0), d.get("pb1", 0)
    return {
        "ctrl_raw":  d.get("fb", ""),
        "panel_raw": d.get("fp", ""),
        "run":   bool(st & 0x01),
        "lidoff": bool(st & 0x02),
        "e5":    bool(st & 0x40),
        "st":    st,
        "loads": bits(pb1, B1),
        "pb1": pb1, "mode": [d.get("pb2", 0), d.get("pb3", 0)],
        # The candidate temperature bytes are bytes 1 and 5 of the controller
        # frame, so they are already in ctrl_raw -- no separate field needed.
        "flow_hz": d.get("flow_hz", 0),
        "flow_pulses": d.get("flow_pulses", 0),
        "ok_c": d.get("ok_c"), "bad_c": d.get("bad_c"),
        "ok_p": d.get("ok_p"), "bad_p": d.get("bad_p"),
        "uptime_s": d.get("uptime_s"),
    }


def key(s):
    """What counts as a change worth a line of its own."""
    return (s["ctrl_raw"], s["panel_raw"], s["bad_c"], s["bad_p"])


def fmt_line(s):
    flags = []
    if s["run"]:    flags.append("RUN")
    if s["lidoff"]: flags.append("LID-OFF")
    if s["e5"]:     flags.append("E5")
    if s["bad_c"] or s["bad_p"]:
        flags.append(f"BAD c={s['bad_c']} p={s['bad_p']}")
    return "%8.1f +%-7.1f %-24s %-15s %s %s" % (
        s["t"], s.get("dur", 0.0), s["ctrl_raw"], s["panel_raw"],
        ",".join(flags) or "-", " ".join(s["loads"]))


def compact(path):
    """Run-length compress an existing .jsonl written by an older version."""
    runs, cur = [], None
    for ln in open(path):
        ln = ln.strip()
        if not ln: continue
        r = json.loads(ln)
        if "error" in r:
            runs.append(r); cur = None; continue
        k = key(r)
        if cur is not None and key(cur) == k:
            cur["t_end"] = r["t"]
            cur["dur"] = round(r["t"] - cur["t"], 2)
            cur["n"] = cur.get("n", 1) + 1
        else:
            r.setdefault("t_end", r["t"]); r.setdefault("dur", 0.0); r.setdefault("n", 1)
            runs.append(r); cur = r
    out = path.replace(".jsonl", "") + ".compact.jsonl"
    with open(out, "w") as f:
        for r in runs: f.write(json.dumps(r) + "\n")
    print(f"{path}: {sum(1 for _ in open(path))} lines -> {len(runs)} runs  ->  {out}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="d8-sniffer.local")
    ap.add_argument("--hz", type=float, default=5.0)
    ap.add_argument("--out", default=None)
    ap.add_argument("--refresh", type=float, default=1.0,
                    help="how often to rewrite the in-progress run's line")
    ap.add_argument("--compact", metavar="FILE", help="compress an existing .jsonl and exit")
    a = ap.parse_args()

    if a.compact:
        return compact(a.compact)

    name = a.out or time.strftime("d8-cycle-%Y%m%d-%H%M%S")
    if name.endswith(".jsonl"):
        name = name[:-6]
    fj = open(name + ".jsonl", "a", buffering=1)
    ft = open(name + ".log", "a", buffering=1)
    url = f"http://{a.host}/api/status"

    t0 = time.time()
    cur = None            # the run in progress
    off_j = off_t = 0     # where its line starts in each file
    last_refresh, runs, errs = 0.0, 0, 0
    hdr = (f"# d8 cycle log  host={a.host}  started={time.strftime('%Y-%m-%d %H:%M:%S')}\n"
           f"# run-length compressed: one line per distinct state, rewritten while it holds\n"
           f"# start    held     ctrl frame               panel frame     state\n")
    ft.write(hdr)
    print(hdr + f"# writing {name}.jsonl and {name}.log", flush=True)

    def emit(rewrite):
        """Write the current run, either as a new line or over the last one."""
        nonlocal off_j, off_t
        if rewrite:
            fj.seek(off_j); fj.truncate()
            ft.seek(off_t); ft.truncate()
        else:
            off_j, off_t = fj.tell(), ft.tell()
        fj.write(json.dumps(cur) + "\n")
        ft.write(fmt_line(cur) + "\n")

    try:
        while True:
            now = time.time()
            try:
                with urllib.request.urlopen(url, timeout=3) as r:
                    d = json.loads(r.read().decode())
                s = decode(d)
                el = round(now - t0, 2)
                if cur is not None and key(cur) == key(s):
                    # same state: extend the run in place, do not add a line
                    cur["t_end"], cur["n"] = el, cur["n"] + 1
                    cur["dur"] = round(el - cur["t"], 2)
                    if (now - last_refresh) >= a.refresh:
                        emit(rewrite=True); last_refresh = now
                else:
                    if cur is not None:
                        emit(rewrite=True)        # finalise the run that just ended
                    cur = s
                    cur["t"] = cur["t_end"] = el
                    cur["dur"], cur["n"] = 0.0, 1
                    cur["iso"] = time.strftime("%H:%M:%S", time.localtime(now))
                    emit(rewrite=False)
                    last_refresh, runs = now, runs + 1
                    print(fmt_line(cur), flush=True)
            except Exception as e:
                errs += 1
                if cur is not None:
                    emit(rewrite=True); cur = None      # close the run before the gap
                line = {"t": round(now - t0, 2), "error": str(e)[:120]}
                off_j, off_t = fj.tell(), ft.tell()
                fj.write(json.dumps(line) + "\n")
                ft.write("%8.1f  !! %s\n" % (now - t0, str(e)[:120]))
            time.sleep(max(0.02, 1.0 / a.hz))
    except KeyboardInterrupt:
        pass
    finally:
        if cur is not None:
            emit(rewrite=True)
        msg = (f"\n# stopped after {time.time()-t0:.0f}s, "
               f"{runs} runs, {errs} poll errors\n# files: {name}.jsonl {name}.log\n")
        ft.write(msg); print(msg, flush=True)
        fj.close(); ft.close()


if __name__ == "__main__":
    sys.exit(main())
