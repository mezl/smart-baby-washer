#!/usr/bin/env python3
"""Does the lockout depend on WHICH loads run, at a fixed temperature?

Heats the machine above 40 C, then holds three load configurations in turn --
wash+heat, steam, dry -- for equal time. Temperature is held roughly constant
across all three, so the load configuration is the only variable.

On lockout: records the exact frame, releases every load, cuts mains, and
reports. Never commands a heater above CEIL.
"""
import json, sys, time, urllib.request
sys.path.insert(0, "/tmp/claude-1000/-home-kai/75cadcaa-ea43-4cc3-b05f-8a5941ae3849/scratchpad")
from kasa import call

HOST="192.168.14.13"; PLUG="192.168.14.123"
CEIL=68          # never heat past this
HOLD=180         # seconds per configuration
B=f"http://{HOST}"
log=[]

def g():
    for _ in range(4):
        try:
            with urllib.request.urlopen(B+"/api/status",timeout=6) as f: return json.load(f)
        except Exception: time.sleep(0.5)
    return None
def p(path):
    for _ in range(4):
        try:
            r=urllib.request.Request(B+path,method="POST")
            with urllib.request.urlopen(r,timeout=6) as f: return True
        except Exception: time.sleep(0.5)
    return False
def release(): p("/api/panel_ovr?clr=0&set=0"); p("/api/mode_ovr?b2=&b3=")
def stamp(): return time.strftime("%H:%M:%S")

def wait_clear(maxwait=5400):
    t0=time.time()
    while time.time()-t0<maxwait:
        d=g()
        # st_real is 0 until the first controller frame is decoded, so a
        # freshly rebooted board reports "no fault" before it knows anything.
        # Polling in that window reads as CLEAR and starts driving loads into a
        # machine that is still locked -- which is what happened after an OTA
        # landed mid-wait. Require real frames, and require it to persist.
        if d and d.get('ok_c',0) > 20 and not (d['st_real'] & 0x40):
            print(f"[{stamp()}] CLEAR at {d['temp_real']} C "
                  f"({d['ok_c']} frames seen)",flush=True); return True
        if d: print(f"[{stamp()}] locked, {d['temp_real']} C — waiting",flush=True)
        time.sleep(120)
    return False

def hold(mask,label,secs,tgt=None):
    """Hold a load mask. Returns 'locked' | 'ceiling' | 'ok'."""
    print(f"\n[{stamp()}] === {label}  (byte1=0x{mask:02X}) for {secs}s ===",flush=True)
    out='ok'
    try:
        if tgt: p("/api/mode_ovr?b2=%02x&b3=%02x"%tgt)
        p("/api/panel_ovr?clr=FF&set=%02X"%mask)
        t0=time.time(); k=0
        while time.time()-t0<secs:
            d=g()
            if d is None: time.sleep(1); continue
            log.append((label,time.time()-t0,d['temp_real'],d['flow_real'],d['st_real'],d['pb1_fwd']))
            if d['st_real'] & 0x40:
                print(f"  t+{time.time()-t0:5.1f}s temp={d['temp_real']} flow={d['flow_real']} "
                      f"fwd=0x{d['pb1_fwd']:02X}  <<< LOCKOUT  frame={d['fb']}",flush=True)
                out='locked'; break
            if (mask & 0x0C) and d['temp_real']>=CEIL:
                print(f"  t+{time.time()-t0:5.1f}s temp={d['temp_real']}  ceiling",flush=True)
                out='ceiling'; break
            k+=1
            if k%6==0: print(f"  t+{time.time()-t0:5.1f}s temp={d['temp_real']} flow={d['flow_real']} fwd=0x{d['pb1_fwd']:02X}",flush=True)
            time.sleep(2)
    finally:
        release()
    return out

if not wait_clear(): sys.exit("never unlocked")

results={}
# --- prepare: empty, small charge, heat above 40 -------------------------
hold(0x02,"prep drain",30)
hold(0x20,"prep fill 20ct",120,(0x2A,0x07))
r=hold(0x05,"prep heat to >45",240)
d=g()
print(f"\n[{stamp()}] prepared at {d['temp_real']} C, byte3=0x{d['st_real']:02X}",flush=True)
if d['st_real'] & 0x40:
    results['prep']='locked'
else:
    for mask,label in ((0x05,"WASH+HEAT"),(0x04,"STEAM"),(0x18,"DRY")):
        d=g()
        if d['st_real'] & 0x40: results[label]='locked (already)'; break
        if d['temp_real']<40:
            print(f"[{stamp()}] {d['temp_real']} C — reheating to >40 first",flush=True)
            hold(0x05,"reheat",180)
        results[label]=hold(mask,label,HOLD)
        if results[label]=='locked': break

print("\n"+"="*58)
for k,v in results.items(): print(f"  {k:<12} {v}")
d=g()
if d and (d['st_real'] & 0x40):
    print(f"\n[{stamp()}] LOCKED — cutting mains 60 s",flush=True)
    release()
    call(PLUG,{"system":{"set_relay_state":{"state":0}}})
    time.sleep(60)
    call(PLUG,{"system":{"set_relay_state":{"state":1}}})
    t0=time.time()
    while time.time()-t0<90 and g() is None: time.sleep(3)
    q=g()
    if q: print(f"[{stamp()}] back: byte3=0x{q['st_real']:02X} temp={q['temp_real']}",flush=True)
else:
    print(f"\n[{stamp()}] finished CLEAR, temp={d['temp_real'] if d else '?'}",flush=True)
import io
io.open("/home/kai/software/d8-smart/captures/lockout-matrix-20260823.log","w").write(
    "\n".join("%s,%.1f,%d,%d,0x%02X,0x%02X"%x for x in log))
print("samples saved to captures/lockout-matrix-20260823.log")
