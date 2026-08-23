#!/usr/bin/env python3
"""Heat-rate vs water-volume test. Bounded, monitored, releases in finally."""
import json,sys,time,urllib.request
sys.path.insert(0,"/tmp/claude-1000/-home-kai/75cadcaa-ea43-4cc3-b05f-8a5941ae3849/scratchpad")
from kasa import call
B="http://192.168.14.13"
MAXT=72          # hard stop: never heat past this
HEAT_MAX=300     # hard stop: never heat longer than this

def g(p="/api/status"):
    for _ in range(3):
        try:
            with urllib.request.urlopen(B+p,timeout=5) as f: return json.load(f)
        except Exception: time.sleep(0.3)
    return None
def p(path):
    for _ in range(3):
        try:
            r=urllib.request.Request(B+path,method="POST")
            with urllib.request.urlopen(r,timeout=5) as f: return True
        except Exception: time.sleep(0.3)
    return False
def release():
    p("/api/panel_ovr?clr=0&set=0"); p("/api/mode_ovr?b2=&b3=")
def killpower(why):
    print("  !! %s — CUTTING MAINS"%why,flush=True)
    release()
    call("192.168.14.123",{"system":{"set_relay_state":{"state":0}}})

def clear_ok():
    d=g()
    return d and not (d['st_real'] & 0x40)

def drive(mask,secs,label,target=None):
    """Hold a load mask for secs, sampling temp. Returns (samples, locked)."""
    out=[]; locked=False
    try:
        if target: p("/api/mode_ovr?b2=%02x&b3=%02x"%target)
        p("/api/panel_ovr?clr=FF&set=%02X"%mask)
        t0=time.time()
        while time.time()-t0<secs:
            d=g()
            if d is None: time.sleep(1); continue
            out.append((time.time()-t0, d['temp_real'], d['flow_real']))
            if d['st_real'] & 0x40:
                locked=True
                print("  %-10s t+%5.1fs temp=%d  <<< LOCKOUT"%(label,time.time()-t0,d['temp_real']),flush=True)
                break
            # Ceiling only gates HEAT stages. Applying it to drain and fill
            # meant a hot machine aborted every stage instantly.
            if (mask & 0x0C) and d['temp_real'] >= MAXT:
                print("  %-10s t+%5.1fs temp=%d  ceiling reached, stopping"%(label,time.time()-t0,d['temp_real']),flush=True)
                break
            if len(out)%5==0:
                print("  %-10s t+%5.1fs temp=%d flow=%d"%(label,time.time()-t0,d['temp_real'],d['flow_real']),flush=True)
            time.sleep(2)
    finally:
        release()
    return out,locked

def rate(s):
    if len(s)<4: return 0.0
    dt=s[-1][0]-s[0][0]; dT=s[-1][1]-s[0][1]
    return dT/dt if dt>0 else 0.0

results=[]
TESTS=[("wash+heat 80ct",0x05,(0xAB,0x1C),80),
       ("steam 20ct",    0x04,(0x2A,0x07),20)]

for name,mask,tgt,counts in TESTS:
    print("\n=== %s ==="%name,flush=True)
    if not clear_ok():
        print("  controller LOCKED — skipping (needs a 60 s power cut)"); results.append((name,None,True)); continue
    drive(0x02,25,"drain")                                     # empty first
    fl,_=drive(0x20,120,"fill",tgt)                            # fill to target
    got=max(x[2] for x in fl) if fl else 0
    print("  filled to %d counts (wanted %d)"%(got,counts),flush=True)
    if got < counts*0.6:
        print("  fill short — skipping heat stage"); results.append((name,None,False)); continue
    s,locked=drive(mask,HEAT_MAX,"heat")
    r=rate(s)
    print("  RATE %.4f C/s   %d -> %d C over %.0fs   locked=%s"%(
          r,s[0][1] if s else 0,s[-1][1] if s else 0,s[-1][0] if s else 0,locked),flush=True)
    results.append((name,r,locked))
    drive(0x02,30,"drain")
    if locked:
        print("  lockout hit — stopping the sweep here",flush=True); break

print("\n%s"%("="*60))
print("%-18s %-12s %s"%("test","heat rate","lockout"))
for n,r,l in results:
    print("%-18s %-12s %s"%(n, "%.4f C/s"%r if r else "n/a", "YES" if l else "no"))
d=g()
if d: print("\nfinal: pb1=0x%02X fwd=0x%02X byte3=0x%02X temp=%d"%(d['pb1'],d['pb1_fwd'],d['st_real'],d['temp_real']))
