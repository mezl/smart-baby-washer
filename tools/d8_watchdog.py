#!/usr/bin/env python3
"""Autonomous D8 lockout watchdog. Runs on the Home Assistant host.

The controller asserts status bit 6 spontaneously (the touch-panel unit does
this at idle, with a byte-perfect link) and then ignores every load command,
including the panel's end-of-cycle release. The only thing that has ever
cleared it is a mains cut, and the required duration is nondeterministic:
60 s has failed four times, 120 s has both failed and worked, 180 s has
worked. So this does not encode a rule -- it escalates and VERIFIES, exactly
like tools/unlock2.py, but with no human and no dev machine in the loop.

Design constraints that shaped it:
  - The ESP32 is powered from CN2 pin 4, i.e. by the plug being switched, so
    it cannot cut its own power and come back. Something external must drive
    the plug. HA is the always-on box that reaches both devices.
  - HA's shell_command kills children at 60 s, so the entry point detaches
    and the real work runs in a daemonised child under an flock.
  - A previous incident stranded the machine OFF when an unlock was
    interrupted. Every cut here is wrapped so the restore is unconditional,
    and a separate recovery path turns the plug back on if a prior run died
    mid-cut (plug off + board silent = stranded, not locked).

Verification is the flow probe, not bit 6: command the intake motor, watch
the flow meter -- the only load with a sensor on it. Bit 6 is treated as an
indicator only. Probing costs a few seconds of water; the tank has an
auto-top-off so this is always available.

State machine per run (single shot; the automation provides the period):
  board up, bit 6 clear      -> reset streak, exit
  board up, bit 6 set, idle  -> streak++; at IDLE_STREAK, unlock
  board up, bit 6 set, loads -> streak++; at LOAD_STREAK, unlock anyway --
                                a locked controller has latched whatever was
                                energised (an 88-minute blower run proved
                                it), so waiting longer only wastes power.
                                The cut IS the way those loads stop.
  board down, plug off       -> restore power (stranded recovery)
  board down, plug on        -> log, exit; cutting blind is never safe
"""
import fcntl, io, json, os, socket, struct, sys, time, urllib.request

ESP   = os.environ.get("D8_ESP", "192.168.14.13")
PLUG  = os.environ.get("D8_PLUG", "192.168.14.123")
DIR   = os.environ.get("D8_DIR", "/config/d8")
STATE = DIR + "/watchdog_state.json"
LOG   = DIR + "/watchdog.log"
CSV   = DIR + "/unlock-attempts.csv"
LOCK  = DIR + "/watchdog.lock"

IDLE_STREAK = 2     # ~2 poll periods locked while idle before acting
LOAD_STREAK = 3     # a little more patience if loads are commanded
CUTS = [120, 180, 300, 600]   # 60 s omitted: 0 for 4 in the field log

def log(msg):
    line = time.strftime("[%Y-%m-%d %H:%M:%S] ") + msg
    with io.open(LOG, "a") as f:
        f.write(line + "\n")

# ---- Kasa TP-Link smart plug, autokey XOR on TCP 9999. No dependencies. ----
def _crypt(data, decrypt=False):
    out, key = bytearray(), 0xAB
    for c in data:
        n = c ^ key
        key = c if decrypt else n
        out.append(n)
    return bytes(out)

def kasa(cmd, timeout=6):
    payload = json.dumps(cmd).encode()
    with socket.create_connection((PLUG, 9999), timeout=timeout) as s:
        s.sendall(struct.pack(">I", len(payload)) + _crypt(payload))
        hdr = s.recv(4)
        n = struct.unpack(">I", hdr)[0]
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                break
            buf += chunk
    return json.loads(_crypt(buf, decrypt=True))

def relay(state):
    kasa({"system": {"set_relay_state": {"state": state}}})

def plug_on():
    try:
        return kasa({"system": {"get_sysinfo": {}}})["system"]["get_sysinfo"]["relay_state"] == 1
    except Exception:
        return None

# ---- ESP32 HTTP ----
def g():
    for _ in range(4):
        try:
            with urllib.request.urlopen(f"http://{ESP}/api/status", timeout=6) as f:
                return json.load(f)
        except Exception:
            time.sleep(0.5)
    return None

def p(path):
    for _ in range(4):
        try:
            r = urllib.request.Request(f"http://{ESP}{path}", method="POST")
            with urllib.request.urlopen(r, timeout=6):
                return True
        except Exception:
            time.sleep(0.5)
    return False

def wait_up(maxw=150):
    # st_real is 0 until the first controller frame decodes; ok_c > 20 is the
    # guard that stopped a rebooting board from reading as CLEAR.
    t0 = time.time()
    while time.time() - t0 < maxw:
        d = g()
        if d and d.get("ok_c", 0) > 20:
            return d
        time.sleep(3)
    return None

def probe():
    """Definitive: intake ON, watch the flow meter. None = could not probe."""
    d = g()
    if d is None or d.get("pb1"):
        return None
    base = d["flow_real"]; peak = base
    try:
        p("/api/mode_ovr?b2=2a&b3=07")
        p("/api/panel_ovr?clr=FF&set=20")
        t0 = time.time()
        while time.time() - t0 < 20:
            q = g()
            if q:
                peak = max(peak, q["flow_real"])
                if peak > base + 3:
                    break
            time.sleep(1)
    finally:
        p("/api/panel_ovr?clr=0&set=0")
        p("/api/mode_ovr?b2=&b3=")
    return peak > base + 3

def rec(temp, cut, bit6, prob):
    new = not os.path.exists(CSV)
    with io.open(CSV, "a") as f:
        if new:
            f.write("time,temp_c,cut_s,bit6_after,probe_after\n")
        f.write("%s,%s,%s,%s,%s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"),
                                      temp, cut, bit6, prob))

def load_state():
    try:
        with io.open(STATE) as f:
            return json.load(f)
    except Exception:
        return {"streak": 0}

def save_state(st):
    with io.open(STATE, "w") as f:
        json.dump(st, f)

def unlock():
    """Escalating cuts, each restore unconditional, each result verified."""
    for cut in CUTS:
        d = g()
        temp = d.get("temp_real", "?") if d else "?"
        log("cutting mains for %ds (temp %s C)" % (cut, temp))
        try:
            relay(0)
            time.sleep(cut)
        finally:
            # An exception between relay(0) and here must NEVER strand the
            # machine off. Retry the restore until the plug confirms it.
            for _ in range(20):
                try:
                    relay(1)
                    if plug_on():
                        break
                except Exception:
                    pass
                time.sleep(5)
        d = wait_up()
        if d is None:
            log("board did not come back after %ds cut -- stopping" % cut)
            rec(temp, cut, "?", "board-down")
            return False
        bit6 = 1 if (d["st_real"] & 0x40) else 0
        prob = probe() if not bit6 else None
        rec(d.get("temp_real", "?"), cut, bit6,
            {True: 1, False: 0, None: ""}[prob])
        if not bit6 and prob:
            log("UNLOCKED by a %ds cut, verified by flow probe" % cut)
            return True
        log("%ds cut: bit6=%d probe=%s -- escalating" % (cut, bit6, prob))
        time.sleep(60)
    log("all cuts exhausted; still locked")
    return False

def run():
    st = load_state()
    d = g()
    if d is None:
        on = plug_on()
        if on is False:
            # A prior run (or a manual CUT MAINS from the app) left the plug
            # off with nothing scheduled to restore it. That is a stranding,
            # and fixing it is this branch's whole purpose.
            log("board down and plug OFF -- restoring power (stranded)")
            relay(1)
        elif on:
            log("board unreachable but plug is ON -- nothing safe to do")
        else:
            log("board and plug both unreachable")
        return
    locked = bool(d["st_real"] & 0x40)
    if not locked:
        if st.get("streak"):
            save_state({"streak": 0})
        return
    st["streak"] = st.get("streak", 0) + 1
    save_state(st)
    idle = (d.get("pb1", 0) == 0) and (d.get("state", 0) == 0)
    need = IDLE_STREAK if idle else LOAD_STREAK
    log("locked (byte3=0x%02X, %s C, %s) streak %d/%d"
        % (d["st_real"], d.get("temp_real", "?"),
           "idle" if idle else "loads 0x%02X" % d.get("pb1", 0),
           st["streak"], need))
    if st["streak"] < need:
        return
    ok = unlock()
    save_state({"streak": 0})
    log("unlock %s" % ("succeeded" if ok else "FAILED -- will retry next poll"))

if __name__ == "__main__":
    os.makedirs(DIR, exist_ok=True)
    if "--child" not in sys.argv:
        # Detach: HA's shell_command kills its child at 60 s, and a full
        # escalation takes up to ~25 min.
        import subprocess
        subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), "--child"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        sys.exit(0)
    lk = io.open(LOCK, "w")
    try:
        fcntl.flock(lk, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit(0)          # an unlock is already in progress
    try:
        run()
    except Exception as e:
        log("ERROR: %r" % e)
        # Belt and braces: whatever happened, do not leave the plug off.
        try:
            if plug_on() is False:
                relay(1)
                log("restored plug in exception handler")
        except Exception:
            pass
