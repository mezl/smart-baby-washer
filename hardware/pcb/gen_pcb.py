#!/usr/bin/env python3
"""CN2 interceptor carrier board — geometry model, Gerber/Excellon writer, DRC.

Four through-hole components, no discretes:

    J1  JST-XH 4-pin, 2.54 mm      cable to the D8 CONTROLLER's CN2 header
    J2  JST-XH 4-pin, 2.54 mm      cable to the FRONT PANEL's CN2 header
    U1  Seeed XIAO ESP32-C3        2x7, 2.54 mm pitch, 15.24 mm between rows
    U2  4-ch BSS138 level shifter  2x6, 2.54 mm pitch, row gap = LS_ROW below

CN2 pins 3 and 4 (GND, +5V) pass straight through J1 to J2 — the panel is powered
by the controller and breaking that leaves it dead. Pins 1 and 2 are routed
through the level shifter to the ESP32, which sees and can rewrite every byte.

Channel assignment matches docs/wiring.md exactly:

    ch1  CONTROLLER pin 1 -> HV1 . LV1 -> D1 / GPIO3   Serial0 RX
    ch2  CONTROLLER pin 2 <- HV2 . LV2 <- D2 / GPIO4   Serial0 TX
    ch3  PANEL      pin 1 <- HV3 . LV3 <- D3 / GPIO5   Serial1 TX
    ch4  PANEL      pin 2 -> HV4 . LV4 -> D4 / GPIO6   Serial1 RX

Run it:  python3 gen_pcb.py     -> gerbers/ + DRC + connectivity + preview.svg

Millimetres throughout. Gerber is 4.6 format, absolute, metric.
"""
import math, os, sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gerbers")

# ---------------------------------------------------------------------------
# !! MEASURE THIS BEFORE ORDERING !!
# Row spacing on the 4-channel BSS138 modules is not standardised. SparkFun's
# BOB-12009 and most clones use 0.4" (10.16 mm), but 0.5" (12.7 mm) exists.
# Put calipers across your module, centre of one row to centre of the other.
LS_ROW = 10.16
# ---------------------------------------------------------------------------

# Economy 2-layer process (JLCPCB/PCBWay) is 6 mil = 0.152 mm. Everything here is
# drawn well inside that, so the board is cheap and forgiving to hand-solder.
RULE_TRACE_W   = 0.35
RULE_CLEARANCE = 0.20
RULE_ANNULUS   = 0.30
RULE_MIN_DRILL = 0.30
EDGE_KEEPOUT   = 0.50

BOARD_W, BOARD_H = 31.5, 26.0
pads, tracks, texts = [], [], []


class Pad:
    def __init__(self, ref, name, x, y, w, h, drill=0.0, shape="circle"):
        self.ref, self.name, self.x, self.y = ref, name, x, y
        self.w, self.h, self.drill, self.shape = w, h, drill, shape
        self.net = None
    def bbox(self):
        return (self.x-self.w/2, self.y-self.h/2, self.x+self.w/2, self.y+self.h/2)


class Track:
    def __init__(self, net, layer, pts, w=RULE_TRACE_W):
        self.net, self.layer, self.pts, self.w = net, layer, pts, w


class Text:
    def __init__(self, s, x, y, size=1.2):
        self.s, self.x, self.y, self.size = s, x, y, size


def header(ref, x0, y0, n, rows=1, rowgap=0.0, pitch=2.54, drill=1.0, pad=1.8):
    """Vertical 2.54 mm through-hole header. Pin 1 square, rest round."""
    out = []
    for r in range(rows):
        for i in range(n):
            idx = r*n + i + 1
            p = Pad(ref, str(idx), x0 + r*rowgap, y0 + i*pitch, pad, pad, drill,
                    "rect" if idx == 1 else "circle")
            pads.append(p); out.append(p)
    return out


# ============================================================================
# placement — packed as tightly as the routing allows
# ============================================================================
# Every column position below is derived from a lane count, not chosen to look
# tidy. Left to right the board is:
#
#   ring ring | J2 | ch4 ch3 | XIAO+shifter block | GND ch2 ch1 | J1 | ring ring
#
# The block is fixed at 16.84 mm (15.24 mm row pitch plus a 1.6 mm pad), the
# connectors at 1.8 mm, and each routing lane needs 0.35 mm of trace plus 0.2 mm
# either side. That adds up to 36 mm, and there is nothing left to remove without
# dropping to 4 layers or moving both connectors to one edge.
CY = BOARD_H / 2.0

XL, XR     = 6.20, 21.44      # XIAO pad columns, 15.24 mm apart
U2LV, U2HV = 8.74, 18.90      # shifter, centred in the gap
J2X, J1X   = 2.80, 25.60      # panel left, controller right
J3X = J4X  = 28.40            # flow relay: J3 to the meter, J4 to the controller

# Lanes, each 0.30-0.35 mm of trace with at least 0.30 mm to whatever is beside
# it. Every one of these was pulled in until that margin was reached.
RING_L_GND, RING_R_GND = 0.75, 30.50
LANE_CH4, LANE_CH3 = 4.20, 4.90            # J2 side
LANE_GND, LANE_CH2, LANE_CH1 = 22.80, 23.50, 24.20   # J1 side
LANE_FIN, LANE_FVCC = 27.00, 29.90         # flow relay side
TOP, BOT_5V, BOT_CH, BOT_GND = 2.00, 22.40, 23.50, 24.70

J2 = header("J2", J2X, CY - 3*2.54/2, 4)
J1 = header("J1", J1X, CY - 3*2.54/2, 4)
for i, n in enumerate("1234"):
    J1[i].name = n; J2[i].name = n

# Flow-meter relay, built exactly like the CN2 interception: the FV cable is cut
# and BOTH ends land on the board. J3 takes the meter, J4 goes on to the
# controller, and the ESP32 sits in between -- so it can pass the real pulses
# through, rewrite their rate, or synthesise flow on a dry bench.
#
#   J3  FV METER        Vcc  GND  SIG -> GPIO10
#   J4  FV CONTROLLER   Vcc  GND  SIG <- GPIO20
#
# Vcc passes straight through J3 to J4 the same way CN2 pins 3 and 4 do; only the
# signal line is broken. Both signal GPIOs have to come off the SAME side of the
# XIAO to be routable, and of the three free pins only GPIO10 (D10) and GPIO20
# (D7) are on the right-hand column -- GPIO7 (D5) is on the left. GPIO2/8/9 are
# strapping pins and GPIO21 emits ROM boot chatter.
J3 = header("J3", J3X, 5.38, 3)          # to the meter
J4 = header("J4", J4X, 15.54, 3)         # on to the controller
for i, n in enumerate(["VCC", "GND", "SIG"]):
    J3[i].name = n; J4[i].name = n

U1 = header("U1", XL, CY - 6*2.54/2, 7, rows=2, rowgap=XR-XL, pad=1.6)
for i, n in enumerate(["D0","D1","D2","D3","D4","D5","D6"]):     U1[i].name = n
for i, n in enumerate(["5V","GND","3V3","D10","D9","D8","D7"]):  U1[7+i].name = n
X = {p.name: p for p in U1}

U2 = header("U2", U2LV, CY - 5*2.54/2, 6, rows=2, rowgap=U2HV-U2LV, pad=1.6)
for i, n in enumerate(["LV1","LV2","LV","GND_L","LV3","LV4"]):   U2[i].name = n
for i, n in enumerate(["HV1","HV2","HV","GND_H","HV3","HV4"]):   U2[6+i].name = n
LS = {p.name: p for p in U2}

# ============================================================================
# nets
# ============================================================================
NETS = {}
def net(name, *ps):
    NETS.setdefault(name, [])
    for p in ps:
        p.net = name; NETS[name].append(p)

net("+5V",  J1[3], J2[3], LS["HV"],    X["5V"])
net("GND",  J1[2], J2[2], J3[1], J4[1], LS["GND_H"], LS["GND_L"], X["GND"])
net("+3V3", LS["LV"], X["3V3"])
net("FLOW_IN",  J3[2], X["D10"])     # meter pulses      -> GPIO10
net("FLOW_OUT", J4[2], X["D7"])      # what the board sees <- GPIO20
net("FV_VCC",   J3[0], J4[0])        # meter supply, straight through

net("CH1_HV", J1[0], LS["HV1"]);  net("CH1_LV", LS["LV1"], X["D1"])
net("CH2_HV", J1[1], LS["HV2"]);  net("CH2_LV", LS["LV2"], X["D2"])
net("CH3_HV", J2[0], LS["HV3"]);  net("CH3_LV", LS["LV3"], X["D3"])
net("CH4_HV", J2[1], LS["HV4"]);  net("CH4_LV", LS["LV4"], X["D4"])

NC = [X["D0"], X["D5"], X["D6"], X["D9"], X["D8"]]
for p in NC: p.net = "NC"

# ============================================================================
# routing
# ============================================================================
def trk(n, layer, *pts, w=RULE_TRACE_W):
    tracks.append(Track(n, layer, list(pts), w))

CHL = (XL + U2LV) / 2.0       # 0.94 mm channel, one 0.30 mm trace per layer
CHR = (U2HV + XR) / 2.0

# LV side: jog in the left channel to reach each D pin. Three fit on top because
# their y spans do not overlap; the fourth takes the bottom layer.
for nm, src, dst, layer in (("CH1_LV", LS["LV1"], X["D1"], "top"),
                            ("CH2_LV", LS["LV2"], X["D2"], "top"),
                            ("CH3_LV", LS["LV3"], X["D3"], "top"),
                            ("CH4_LV", LS["LV4"], X["D4"], "bottom")):
    trk(nm, layer, (src.x, src.y), (CHL, src.y), (CHL, dst.y), (dst.x, dst.y), w=0.30)

# HV side. HV1 and HV4 sit at the ends of their column so they leave vertically;
# HV2 uses the right channel and HV3 steps out into the wide middle gap. All four
# then run round the ends of the XIAO.
trk("CH1_HV", "top", (LS["HV1"].x, LS["HV1"].y), (LS["HV1"].x, TOP),
    (LANE_CH1, TOP), (LANE_CH1, J1[0].y), (J1[0].x, J1[0].y))
trk("CH2_HV", "top", (LS["HV2"].x, LS["HV2"].y), (CHR, LS["HV2"].y), (CHR, 3.4),
    (LANE_CH2, 3.4), (LANE_CH2, J1[1].y), (J1[1].x, J1[1].y), w=0.30)
trk("CH3_HV", "top", (LS["HV3"].x, LS["HV3"].y), (15.0, LS["HV3"].y), (15.0, 22.0),
    (LANE_CH3, 22.0), (LANE_CH3, J2[0].y), (J2[0].x, J2[0].y))
trk("CH4_HV", "top", (LS["HV4"].x, LS["HV4"].y), (LS["HV4"].x, BOT_CH),
    (LANE_CH4, BOT_CH), (LANE_CH4, J2[1].y), (J2[1].x, J2[1].y))

# 3V3 on top: it must cross the +5V climb somewhere and separating them by layer
# is cheaper than routing round. It slips through the HV column at the ESP32's
# own 3V3 row, which is the clear window between HV2 and the HV rail.
trk("+3V3", "top", (LS["LV"].x, LS["LV"].y), (10.20, LS["LV"].y),
    (10.20, X["3V3"].y), (X["3V3"].x, X["3V3"].y), w=0.30)

# +5V, bottom. Both connectors' pin 4 is the LOWEST pin, so the ring comes up its
# own column from underneath -- no side lane needed at either end, which is what
# lets the connectors sit this close to the block.
trk("+5V", "bottom", (LS["HV"].x, LS["HV"].y), (14.0, LS["HV"].y), (14.0, TOP),
    (X["5V"].x, TOP), (X["5V"].x, X["5V"].y))
trk("+5V", "bottom", (LS["HV"].x, LS["HV"].y), (16.0, LS["HV"].y), (16.0, BOT_5V),
    (J1[3].x, BOT_5V), (J1[3].x, J1[3].y))
trk("+5V", "bottom", (16.0, BOT_5V), (J2[3].x, BOT_5V), (J2[3].x, J2[3].y))

# Flow relay, both on top. Each slips through a window between J1's pads -- the
# 2.54 mm pitch leaves 0.74 mm between adjacent 1.8 mm pads, enough for a 0.3 mm
# trace with 0.22 mm either side.
trk("FLOW_IN", "top", (J3[2].x, J3[2].y), (LANE_FIN, J3[2].y),
    (LANE_FIN, X["D10"].y), (X["D10"].x, X["D10"].y), w=0.30)
# J4.SIG and D7 happen to share a row, so this one is a straight run below J1.
trk("FLOW_OUT", "top", (J4[2].x, J4[2].y), (X["D7"].x, X["D7"].y), w=0.30)
trk("FV_VCC", "top", (J3[0].x, J3[0].y), (LANE_FVCC, J3[0].y),
    (LANE_FVCC, J4[0].y), (J4[0].x, J4[0].y), w=0.30)

# GND: the two shifter grounds join across the middle gap on top; the run out to
# J2 slips through the clear window between D3 and D4.
trk("GND", "top", (LS["GND_H"].x, LS["GND_H"].y), (LS["GND_L"].x, LS["GND_L"].y))
trk("GND", "bottom", (LS["GND_L"].x, LS["GND_L"].y), (J2[2].x, J2[2].y), w=0.30)
trk("GND", "bottom", (X["GND"].x, X["GND"].y), (LANE_GND, X["GND"].y),
    (LANE_GND, J1[2].y), (J1[2].x, J1[2].y))
# Step down to y=13.0 before running out to the ring: at J1.3's own row this
# passes 0.195 mm under J4's VCC pad, which is 5 um inside the clearance rule.
# Step down to y=13.0 before running right: at J1.3's own row this passes
# 0.195 mm under J4's VCC pad, 5 um inside the rule. LANE_FIN is free on the
# bottom layer -- FLOW_IN uses it on top.
trk("GND", "bottom", (J1[2].x, J1[2].y), (LANE_FIN, J1[2].y), (LANE_FIN, 13.0),
    (RING_R_GND, 13.0), (RING_R_GND, J3[1].y), (J3[1].x, J3[1].y))
trk("GND", "bottom", (RING_R_GND, 13.0), (RING_R_GND, J4[1].y), (J4[1].x, J4[1].y))
trk("GND", "bottom", (RING_R_GND, J4[1].y), (RING_R_GND, BOT_GND),
    (RING_L_GND, BOT_GND), (RING_L_GND, J2[2].y), (J2[2].x, J2[2].y))

# ============================================================================
# silkscreen
# ============================================================================
texts += [
    Text("D8 CN2 INTERCEPT", 2.0, 0.35, 0.85),
    Text("PNL", 0.8, 19.6, 0.8),
    Text("CTL", 24.4, 19.6, 0.8),
    Text("FV", 27.3, 12.4, 0.8),
]

# ============================================================================
# DRC
# ============================================================================
def seg_pt(p, a, b):
    dx, dy = b[0]-a[0], b[1]-a[1]
    L = dx*dx + dy*dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((p[0]-a[0])*dx + (p[1]-a[1])*dy)/L))
    return math.hypot(p[0]-(a[0]+t*dx), p[1]-(a[1]+t*dy))

def seg_seg(a1, a2, b1, b2):
    def cr(o, a, b): return (a[0]-o[0])*(b[1]-o[1]) - (a[1]-o[1])*(b[0]-o[0])
    d1, d2, d3, d4 = cr(b1,b2,a1), cr(b1,b2,a2), cr(a1,a2,b1), cr(a1,a2,b2)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)): return 0.0
    return min(seg_pt(a1,b1,b2), seg_pt(a2,b1,b2), seg_pt(b1,a1,a2), seg_pt(b2,a1,a2))

def rect_seg(r, a, b):
    x0,y0,x1,y1 = r
    if (x0 <= a[0] <= x1 and y0 <= a[1] <= y1) or (x0 <= b[0] <= x1 and y0 <= b[1] <= y1):
        return 0.0
    E = [((x0,y0),(x1,y0)),((x1,y0),(x1,y1)),((x1,y1),(x0,y1)),((x0,y1),(x0,y0))]
    return min(seg_seg(a,b,e[0],e[1]) for e in E)

def rect_rect(r1, r2):
    dx = max(r2[0]-r1[2], r1[0]-r2[2], 0.0)
    dy = max(r2[1]-r1[3], r1[1]-r2[3], 0.0)
    return math.hypot(dx, dy)

def drc():
    errs, n = [], 0
    # Through-hole pads exist on both layers, so every track is checked against
    # every pad regardless of which layer the track is on.
    for layer in ("top", "bottom"):
        lt = [t for t in tracks if t.layer == layer]
        for t in lt:
            for k in range(len(t.pts)-1):
                a, b = t.pts[k], t.pts[k+1]
                for p in pads:
                    if p.net == t.net: continue
                    d = rect_seg(p.bbox(), a, b) - t.w/2; n += 1
                    if d < RULE_CLEARANCE:
                        errs.append(f"{layer}: track {t.net} vs pad {p.ref}.{p.name} = {d:.3f}")
        for i in range(len(lt)):
            for j in range(i+1, len(lt)):
                if lt[i].net == lt[j].net: continue
                for a in range(len(lt[i].pts)-1):
                    for b in range(len(lt[j].pts)-1):
                        d = seg_seg(lt[i].pts[a], lt[i].pts[a+1],
                                    lt[j].pts[b], lt[j].pts[b+1]) - (lt[i].w+lt[j].w)/2
                        n += 1
                        if d < RULE_CLEARANCE:
                            errs.append(f"{layer}: track {lt[i].net} vs {lt[j].net} = {d:.3f}")
    for i in range(len(pads)):
        for j in range(i+1, len(pads)):
            a, b = pads[i], pads[j]
            if a.net and a.net != "NC" and a.net == b.net: continue
            d = rect_rect(a.bbox(), b.bbox()); n += 1
            if d < RULE_CLEARANCE:
                errs.append(f"pad {a.ref}.{a.name} vs {b.ref}.{b.name} = {d:.3f}")
    for p in pads:
        if p.drill:
            ann = (min(p.w, p.h) - p.drill)/2
            if ann < RULE_ANNULUS: errs.append(f"annulus {p.ref}.{p.name} = {ann:.3f}")
            if p.drill < RULE_MIN_DRILL: errs.append(f"drill {p.ref}.{p.name}")
        x0,y0,x1,y1 = p.bbox()
        if x0 < EDGE_KEEPOUT or y0 < EDGE_KEEPOUT or x1 > BOARD_W-EDGE_KEEPOUT \
           or y1 > BOARD_H-EDGE_KEEPOUT:
            errs.append(f"edge keepout {p.ref}.{p.name}")
    for t in tracks:
        for q in t.pts:
            if q[0] < EDGE_KEEPOUT or q[1] < EDGE_KEEPOUT \
               or q[0] > BOARD_W-EDGE_KEEPOUT or q[1] > BOARD_H-EDGE_KEEPOUT:
                errs.append(f"edge keepout: track {t.net} at {q}")
    return errs, n

def connectivity():
    """Every pad on a net, every net >= 2 pads, every pad physically touched."""
    bad = []
    for name, ps in NETS.items():
        if name == "NC": continue
        if len(ps) < 2: bad.append(f"net {name} has {len(ps)} pad(s)")
    for p in pads:
        if p.net == "NC": continue
        if p.net is None:
            bad.append(f"{p.ref}.{p.name} on no net"); continue
        if not any(t.net == p.net and any(abs(q[0]-p.x) < 0.01 and abs(q[1]-p.y) < 0.01
                                          for q in t.pts) for t in tracks):
            bad.append(f"{p.ref}.{p.name} ({p.net}) has no track landing on it")
    # every net must be one connected component
    for name, ps in NETS.items():
        segs = [t for t in tracks if t.net == name]
        if not segs: continue
        nodes = {(round(p.x,3), round(p.y,3)) for p in ps}
        seen, stack = set(), [next(iter(nodes))]
        adj = {}
        for t in segs:
            for k in range(len(t.pts)-1):
                a = (round(t.pts[k][0],3), round(t.pts[k][1],3))
                b = (round(t.pts[k+1][0],3), round(t.pts[k+1][1],3))
                adj.setdefault(a, set()).add(b); adj.setdefault(b, set()).add(a)
        while stack:
            cur = stack.pop()
            if cur in seen: continue
            seen.add(cur)
            stack.extend(adj.get(cur, ()))
        missing = nodes - seen
        if missing: bad.append(f"net {name} is not one island; unreached: {sorted(missing)}")
    return bad

# ============================================================================
# Gerber / Excellon / preview
# ============================================================================
def g(v): return str(int(round(v*1e6)))

def hdr(f, fn):
    f.write("%FSLAX46Y46*%\n%MOMM*%\n")
    f.write(f"%TF.FileFunction,{fn}*%\n%TF.Part,Single*%\n%LPD*%\n")

def apertures(items):
    ap, order = {}, []
    for k in items:
        if k not in ap:
            ap[k] = 10 + len(ap); order.append(k)
    return ap, order

def defn(k):
    return f"C,{k[1]:.4f}" if k[0] == "C" else f"R,{k[1]:.4f}X{k[2]:.4f}"

def write_copper(path, layer, fn):
    keys = []
    for p in pads:
        keys.append(("C", p.w) if p.shape == "circle" else ("R", p.w, p.h))
    for t in tracks:
        if t.layer == layer: keys.append(("C", t.w))
    ap, order = apertures(keys)
    with open(path, "w") as f:
        hdr(f, fn)
        for k in order: f.write(f"%ADD{ap[k]}{defn(k)}*%\n")
        cur = None
        for p in pads:
            k = ("C", p.w) if p.shape == "circle" else ("R", p.w, p.h)
            if ap[k] != cur: f.write(f"D{ap[k]}*\n"); cur = ap[k]
            f.write(f"X{g(p.x)}Y{g(p.y)}D03*\n")
        for t in tracks:
            if t.layer != layer: continue
            k = ("C", t.w)
            if ap[k] != cur: f.write(f"D{ap[k]}*\n"); cur = ap[k]
            f.write(f"X{g(t.pts[0][0])}Y{g(t.pts[0][1])}D02*\n")
            for q in t.pts[1:]: f.write(f"X{g(q[0])}Y{g(q[1])}D01*\n")
        f.write("M02*\n")

def write_mask(path, fn):
    keys = [("C", p.w+0.1) if p.shape == "circle" else ("R", p.w+0.1, p.h+0.1) for p in pads]
    ap, order = apertures(keys)
    with open(path, "w") as f:
        hdr(f, fn)
        for k in order: f.write(f"%ADD{ap[k]}{defn(k)}*%\n")
        cur = None
        for p in pads:
            k = ("C", p.w+0.1) if p.shape == "circle" else ("R", p.w+0.1, p.h+0.1)
            if ap[k] != cur: f.write(f"D{ap[k]}*\n"); cur = ap[k]
            f.write(f"X{g(p.x)}Y{g(p.y)}D03*\n")
        f.write("M02*\n")

FONT = {
 'A':[(0,0,0,4),(0,4,3,4),(3,4,3,0),(0,2,3,2)],'B':[(0,0,0,4),(0,4,3,4),(3,4,3,2),(0,2,3,2),(3,2,3,0),(0,0,3,0)],
 'C':[(3,4,0,4),(0,4,0,0),(0,0,3,0)],'D':[(0,0,0,4),(0,4,2,4),(2,4,3,3),(3,3,3,1),(3,1,2,0),(2,0,0,0)],
 'E':[(3,4,0,4),(0,4,0,0),(0,0,3,0),(0,2,2,2)],'F':[(3,4,0,4),(0,4,0,0),(0,2,2,2)],
 'G':[(3,4,0,4),(0,4,0,0),(0,0,3,0),(3,0,3,2),(3,2,2,2)],'H':[(0,0,0,4),(3,0,3,4),(0,2,3,2)],
 'I':[(1.5,0,1.5,4),(0,4,3,4),(0,0,3,0)],'J':[(3,4,3,1),(3,1,2,0),(2,0,0,0),(0,0,0,1)],
 'K':[(0,0,0,4),(0,2,3,4),(0,2,3,0)],'L':[(0,4,0,0),(0,0,3,0)],
 'M':[(0,0,0,4),(0,4,1.5,2),(1.5,2,3,4),(3,4,3,0)],'N':[(0,0,0,4),(0,4,3,0),(3,0,3,4)],
 'O':[(0,0,0,4),(0,4,3,4),(3,4,3,0),(3,0,0,0)],'P':[(0,0,0,4),(0,4,3,4),(3,4,3,2),(3,2,0,2)],
 'R':[(0,0,0,4),(0,4,3,4),(3,4,3,2),(3,2,0,2),(1.5,2,3,0)],'S':[(3,4,0,4),(0,4,0,2),(0,2,3,2),(3,2,3,0),(3,0,0,0)],
 'T':[(0,4,3,4),(1.5,4,1.5,0)],'U':[(0,4,0,0),(0,0,3,0),(3,0,3,4)],
 'V':[(0,4,1.5,0),(1.5,0,3,4)],'W':[(0,4,0.7,0),(0.7,0,1.5,3),(1.5,3,2.3,0),(2.3,0,3,4)],
 'X':[(0,4,3,0),(0,0,3,4)],'Y':[(0,4,1.5,2),(3,4,1.5,2),(1.5,2,1.5,0)],'Z':[(0,4,3,4),(3,4,0,0),(0,0,3,0)],
 '0':[(0,0,0,4),(0,4,3,4),(3,4,3,0),(3,0,0,0)],'1':[(1,3,1.5,4),(1.5,4,1.5,0),(0.5,0,2.5,0)],
 '2':[(0,4,3,4),(3,4,3,2),(3,2,0,2),(0,2,0,0),(0,0,3,0)],'3':[(0,4,3,4),(3,4,3,0),(3,0,0,0),(0,2,3,2)],
 '4':[(0,4,0,2),(0,2,3,2),(3,4,3,0)],'5':[(3,4,0,4),(0,4,0,2),(0,2,3,2),(3,2,3,0),(3,0,0,0)],
 '6':[(3,4,0,4),(0,4,0,0),(0,0,3,0),(3,0,3,2),(3,2,0,2)],'7':[(0,4,3,4),(3,4,1,0)],
 '8':[(0,0,0,4),(0,4,3,4),(3,4,3,0),(3,0,0,0),(0,2,3,2)],'9':[(3,0,3,4),(3,4,0,4),(0,4,0,2),(0,2,3,2)],
 ' ':[], '-':[(0,2,3,2)], '.':[(1.3,0,1.7,0)],
}

def silk_segments():
    segs = []
    for t in texts:
        sc, cx = t.size/4.0, t.x
        for ch in t.s.upper():
            for (x1,y1,x2,y2) in FONT.get(ch, []):
                segs.append((cx+x1*sc, t.y+(4-y1)*sc, cx+x2*sc, t.y+(4-y2)*sc))
            cx += 4.2*sc
    return segs

def write_silk(path, fn):
    with open(path, "w") as f:
        hdr(f, fn)
        f.write("%ADD10C,0.1500*%\nD10*\n")
        for (x1,y1,x2,y2) in silk_segments():
            f.write(f"X{g(x1)}Y{g(y1)}D02*X{g(x2)}Y{g(y2)}D01*\n")
        f.write("M02*\n")

def write_outline(path):
    with open(path, "w") as f:
        hdr(f, "Profile,NP")
        f.write("%ADD10C,0.1000*%\nD10*\n")
        pts = [(0,0),(BOARD_W,0),(BOARD_W,BOARD_H),(0,BOARD_H),(0,0)]
        f.write(f"X{g(pts[0][0])}Y{g(pts[0][1])}D02*\n")
        for p in pts[1:]: f.write(f"X{g(p[0])}Y{g(p[1])}D01*\n")
        f.write("M02*\n")

def write_drill(path):
    holes = {}
    for p in pads:
        if p.drill: holes.setdefault(round(p.drill,3), []).append((p.x, p.y))
    with open(path, "w") as f:
        f.write("M48\nMETRIC,TZ\n")
        tools = {d: i+1 for i, d in enumerate(sorted(holes))}
        for d, i in tools.items(): f.write(f"T{i}C{d:.3f}\n")
        f.write("%\nG90\nG05\n")
        for d in sorted(holes):
            f.write(f"T{tools[d]}\n")
            for (x, y) in holes[d]: f.write(f"X{x:.3f}Y{y:.3f}\n")
        f.write("T0\nM30\n")
    return holes

def render_svg(path):
    S = 14
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{BOARD_W*S:.0f}" '
         f'height="{BOARD_H*S:.0f}" viewBox="-1 -1 {BOARD_W+2} {BOARD_H+2}">',
         f'<rect x="0" y="0" width="{BOARD_W}" height="{BOARD_H}" fill="#0d5c2a" '
         f'stroke="#eee" stroke-width="0.15"/>']
    for t in tracks:
        col = "#e74c3c" if t.layer == "top" else "#3498db"
        d = " ".join(("M" if i == 0 else "L")+f"{p[0]:.3f},{p[1]:.3f}"
                     for i, p in enumerate(t.pts))
        o.append(f'<path d="{d}" stroke="{col}" stroke-width="{t.w}" fill="none" '
                 f'stroke-linecap="round" stroke-linejoin="round" opacity="0.9"/>')
    for p in pads:
        if p.shape == "circle":
            o.append(f'<circle cx="{p.x:.3f}" cy="{p.y:.3f}" r="{p.w/2:.3f}" fill="#e8c547"/>')
        else:
            b = p.bbox()
            o.append(f'<rect x="{b[0]:.3f}" y="{b[1]:.3f}" width="{p.w:.3f}" '
                     f'height="{p.h:.3f}" fill="#e8c547"/>')
        if p.drill:
            o.append(f'<circle cx="{p.x:.3f}" cy="{p.y:.3f}" r="{p.drill/2:.3f}" fill="#0d5c2a"/>')
    for (x1,y1,x2,y2) in silk_segments():
        o.append(f'<line x1="{x1:.3f}" y1="{y1:.3f}" x2="{x2:.3f}" y2="{y2:.3f}" '
                 f'stroke="#fff" stroke-width="0.13"/>')
    o.append('</svg>')
    open(path, "w").write("\n".join(o))

# ============================================================================
def main():
    os.makedirs(OUT, exist_ok=True)
    errs, checks = drc()
    conn = connectivity()
    B = f"{OUT}/cn2-interceptor"
    write_copper(f"{B}-F_Cu.gbr", "top", "Copper,L1,Top")
    write_copper(f"{B}-B_Cu.gbr", "bottom", "Copper,L2,Bot")
    write_mask(f"{B}-F_Mask.gbr", "Soldermask,Top")
    write_mask(f"{B}-B_Mask.gbr", "Soldermask,Bot")
    write_silk(f"{B}-F_Silkscreen.gbr", "Legend,Top")
    write_outline(f"{B}-Edge_Cuts.gbr")
    holes = write_drill(f"{B}-PTH.drl")
    render_svg(f"{OUT}/preview.svg")

    print(f"board          {BOARD_W} x {BOARD_H} mm, 2 layers")
    print(f"components     J1, J2 (JST-XH 4p) . U1 XIAO ESP32-C3 . U2 4ch level shifter")
    print(f"shifter rows   {LS_ROW} mm  <-- verify with calipers before ordering")
    print(f"pads           {len(pads)}   tracks {len(tracks)}   nets {len(NETS)}")
    print(f"drills         {sorted(holes)} mm ({sum(len(v) for v in holes.values())} holes)")
    print(f"DRC            {checks} checks @ {RULE_CLEARANCE} mm clearance")
    if errs:
        print(f"DRC            FAIL — {len(errs)} violation(s)")
        for e in errs[:30]: print("   ", e)
    else:
        print("DRC            PASS")
    if conn:
        print(f"CONNECTIVITY   FAIL — {len(conn)} problem(s)")
        for c in conn[:30]: print("   ", c)
    else:
        print("CONNECTIVITY   PASS — every net is one island, every pad landed on")
    return 1 if (errs or conn) else 0

if __name__ == "__main__":
    sys.exit(main())
