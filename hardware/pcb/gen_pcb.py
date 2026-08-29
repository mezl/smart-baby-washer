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

BOARD_W, BOARD_H = 18.6, 27.0
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


def header(ref, x0, y0, n, rows=1, rowgap=0.0, pitch=2.54, drill=1.0, pad=1.8,
           horiz=False):
    """2.54 mm through-hole header. Pin 1 square, rest round.

    horiz=True lays the pins out along X instead of Y, which is what the CN2
    connectors need now that they stand on the top and bottom edges."""
    out = []
    for r in range(rows):
        for i in range(n):
            idx = r*n + i + 1
            x = x0 + (i*pitch if horiz else r*rowgap)
            y = y0 + (r*rowgap if horiz else i*pitch)
            p = Pad(ref, str(idx), x, y, pad, pad, drill,
                    "rect" if idx == 1 else "circle")
            pads.append(p); out.append(p)
    return out


# ============================================================================
# placement — connectors on the top and bottom edges
# ============================================================================
# The previous revision stood both CN2 connectors on the LEFT and RIGHT edges,
# and that is what made the board 31.5 mm wide: the XIAO's two pad rows are
# 15.24 mm apart, so anything beside them adds width directly. Moving the
# connectors to the top and bottom edges makes the board only as wide as the
# XIAO plus a single routing lane, at the cost of about 2 mm of height:
#
#     top edge     [ J2 -> FRONT PANEL ]
#                    XIAO ESP32-C3, shifter nested inside its pad rows
#     bottom edge  [ J1 -> MAIN BOARD  ]
#
# A 4-pin 2.54 mm connector spans 7.62 mm, so one fits an 18.6 mm edge with
# room to route past it; two side by side would not, which is why they face
# opposite ways. That also suits the cable run -- the two CN2 stubs leave the
# board in opposite directions, the way they arrive.
#
# The flow-meter relay (J3/J4) of the previous revision is NOT on this board.
# Its six pins could not share an edge with the CN2 connectors, and carrying
# them cost more width than everything else put together.

# The XIAO does NOT sit centred. It is pushed to the left edge so the single
# lane it needs on the right has room; the left side needs nothing outside the
# pad column, so the space would otherwise be wasted.
XL     = 1.35                     # left pad column, 0.55 mm off the edge
XR     = XL + 15.24               # right pad column
XMID   = (XL + XR) / 2.0
CY     = BOARD_H / 2.0
U2LV, U2HV = XMID - LS_ROW/2, XMID + LS_ROW/2   # shifter, nested inside U1
Y_BOT, Y_TOP = 1.60, BOARD_H - 1.60             # the two connector rows

# Channels. XIAO-to-shifter is 0.94 mm each side: exactly one 0.30 mm trace per
# layer, which is what the LV side needs and no more. The middle corridor
# between the shifter's own columns is 8.5 mm of clear copper and carries the
# four HV signals straight down to the connectors.
CHL, CHR   = (XL + U2LV)/2.0, (U2HV + XR)/2.0
COR0, COR1 = U2LV + 1.175, U2HV - 1.175   # usable corridor for a 0.35 trace
LANE_R     = 17.85                        # the only lane outside the XIAO
BAND_B, BAND_T = 3.04, BOARD_H - 3.04     # between a connector and the XIAO

# Pin 1 leftmost. The two signal pins are the left pair and the two power pins
# the right pair, which is both the CN2 cable order and the order the routing
# wants: the signals climb straight out of their own pads into the corridor,
# and the power pins are already nearest the lane they leave by.
J_X0 = round((COR0 + COR1)/2.0 - 3*2.54/2, 2)     # 4 pins, centred in corridor

# The PANEL connector takes the bottom edge and the CONTROLLER the top, which
# is not arbitrary: it is what makes the board agree with the firmware's pin
# map without a single config change.
#
# Routing forces LV1..LV4 to land on D1..D4 in order -- they share a 0.94 mm
# channel and any other order makes their spans overlap four deep. D1..D4 are
# GPIO3, 4, 5 and 6. The firmware's as-built map is rxb=5 txb=6 txp=3 rxp=4,
# i.e. the CONTROLLER pair on GPIO5/6 (= D3/D4 = channels 3 and 4) and the
# PANEL pair on GPIO3/4 (= D1/D2 = channels 1 and 2). Channels 3 and 4 are the
# shifter's UPPER pins, so the controller has to enter from the top edge.
#
# Put them the other way round and the board still works, but only after
# swapping the four PIN_* defines -- and a pin map that disagrees with the
# copper is exactly the failure that costs a day: frames simply stop decoding.
J2 = header("J2", J_X0, Y_BOT, 4, horiz=True)     # -> FRONT PANEL  (bottom)
J1 = header("J1", J_X0, Y_TOP, 4, horiz=True)     # -> MAIN BOARD   (top)
for i, n in enumerate("1234"):
    J1[i].name = n; J2[i].name = n

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
net("GND",  J1[2], J2[2], LS["GND_H"], LS["GND_L"], X["GND"])
net("+3V3", LS["LV"], X["3V3"])

# ch1/ch2 = PANEL pair on D1/D2 (GPIO3/4);  ch3/ch4 = CONTROLLER pair on
# D3/D4 (GPIO5/6). Matches firmware config.h exactly: rxb=5 txb=6 txp=3 rxp=4.
net("CH1_HV", J2[0], LS["HV1"]);  net("CH1_LV", LS["LV1"], X["D1"])  # panel RX  <- GPIO3
net("CH2_HV", J2[1], LS["HV2"]);  net("CH2_LV", LS["LV2"], X["D2"])  # panel TX  -> GPIO4
net("CH3_HV", J1[0], LS["HV3"]);  net("CH3_LV", LS["LV3"], X["D3"])  # ctrl  TX  -> GPIO5
net("CH4_HV", J1[1], LS["HV4"]);  net("CH4_LV", LS["LV4"], X["D4"])  # ctrl  RX  <- GPIO6

NC = [X["D0"], X["D5"], X["D6"], X["D10"], X["D9"], X["D8"], X["D7"]]
for p in NC: p.net = "NC"

# ============================================================================
# mechanical — the module bodies, which the copper does not show
# ============================================================================
# The XIAO's PADS span 15.24 mm but its BODY is 21.0 mm long, so it overhangs
# its own pad rows by 2.88 mm at each end -- straight over whatever is beyond
# them. That is why the board is 27 mm tall and not 24: at 24 mm the XIAO sits
# physically on top of both JST housings. Nothing in the Gerbers would have
# shown it, and the boards would have arrived unbuildable.
XIAO_BODY = (17.5, 21.0)      # datasheet outline, W x H as placed here
LS_BODY   = (15.7, 13.2)

def bodies():
    """(name, x0, y0, x1, y1) plan-view outline of each module."""
    out = []
    for name, ps, (bw, bh) in (("U1", U1, XIAO_BODY), ("U2", U2, LS_BODY)):
        cx = (min(p.x for p in ps) + max(p.x for p in ps)) / 2.0
        cy = (min(p.y for p in ps) + max(p.y for p in ps)) / 2.0
        out.append((name, cx-bw/2, cy-bh/2, cx+bw/2, cy+bh/2))
    return out

def mechanical():
    """Module bodies must clear the connectors and stay on the board."""
    bad = []
    for name, x0, y0, x1, y1 in bodies():
        if x0 < 0 or y0 < 0 or x1 > BOARD_W or y1 > BOARD_H:
            bad.append(f"{name} body overhangs the board edge "
                       f"({x0:.2f},{y0:.2f})-({x1:.2f},{y1:.2f})")
        for p in pads:
            if p.ref in ("U1", "U2"): continue
            d = rect_rect((x0,y0,x1,y1), p.bbox())
            if d <= 0:
                bad.append(f"{name} body sits on {p.ref}.{p.name}")
    return bad

# ============================================================================
# the board's pin map, checked against the firmware
# ============================================================================
# The netlist above decides which GPIO each CN2 wire lands on, and the firmware
# has to agree or the link simply does not decode -- no error, no bad checksum,
# just zero frames. That failure has already cost a day on this project once,
# so it is asserted here rather than left to a comment: config.h is the file
# that ships, and this is the board it must describe.
XIAO_GPIO = {"D0":2, "D1":3, "D2":4, "D3":5, "D4":6, "D5":7, "D6":21,
             "D7":20, "D8":8, "D9":9, "D10":10}

def _gpio_of(chan):
    """The XIAO GPIO carrying a channel's LV side."""
    pad = [p for p in NETS[chan + "_LV"] if p.ref == "U1"][0]
    return XIAO_GPIO[pad.name]

# ch3 = controller TX -> we receive;  ch4 = controller RX -> we drive
# ch1 = panel RX      -> we drive;    ch2 = panel TX      -> we receive
BOARD_PINMAP = {"PIN_RX_BOARD": _gpio_of("CH3"), "PIN_TX_BOARD": _gpio_of("CH4"),
                "PIN_TX_PANEL": _gpio_of("CH1"), "PIN_RX_PANEL": _gpio_of("CH2")}

def check_firmware_pinmap():
    """Compare against firmware/include/config.h. Returns a list of mismatches."""
    import re
    cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "..", "firmware", "include", "config.h")
    if not os.path.exists(cfg):
        return ["config.h not found — pin map NOT verified"]
    txt = open(cfg).read()
    out = []
    for k, v in BOARD_PINMAP.items():
        m = re.search(rf"^#define\s+{k}\s+(\d+)", txt, re.M)
        if not m:
            out.append(f"{k} missing from config.h")
        elif int(m.group(1)) != v:
            out.append(f"{k}: board GPIO{v}, firmware GPIO{m.group(1)}")
    return out

# ============================================================================
# routing
# ============================================================================
def trk(n, layer, *pts, w=RULE_TRACE_W):
    tracks.append(Track(n, layer, list(pts), w))

# LV side: jog in the left channel to reach each D pin. Three fit on top
# because their y spans do not overlap; the fourth takes the bottom layer.
for nm, src_, dst, layer in (("CH1_LV", LS["LV1"], X["D1"], "top"),
                             ("CH2_LV", LS["LV2"], X["D2"], "top"),
                             ("CH3_LV", LS["LV3"], X["D3"], "top"),
                             ("CH4_LV", LS["LV4"], X["D4"], "bottom")):
    trk(nm, layer, (src_.x, src_.y), (CHL, src_.y), (CHL, dst.y), (dst.x, dst.y),
        w=0.30)

# HV side. Each signal pin sits directly under the corridor, so it climbs out
# of its own pad and turns once, into its shifter pin. The inner pin's climb
# crosses the outer pin's turn, which is the only reason two of these four are
# on the bottom layer.
trk("CH1_HV", "top",    (J2[0].x, J2[0].y), (J2[0].x, LS["HV1"].y), (LS["HV1"].x, LS["HV1"].y))
trk("CH2_HV", "bottom", (J2[1].x, J2[1].y), (J2[1].x, LS["HV2"].y), (LS["HV2"].x, LS["HV2"].y))
trk("CH3_HV", "top",    (J1[0].x, J1[0].y), (J1[0].x, LS["HV3"].y), (LS["HV3"].x, LS["HV3"].y))
trk("CH4_HV", "bottom", (J1[1].x, J1[1].y), (J1[1].x, LS["HV4"].y), (LS["HV4"].x, LS["HV4"].y))

# +5V and GND both have to reach the XIAO's RIGHT pad column and both
# connectors, so both use the outer lane -- one per layer, which is the whole
# reason the lane has to exist and the board cannot be 17.8 mm wide.
trk("+5V", "top", (J2[3].x, J2[3].y), (J2[3].x, BAND_B), (LANE_R, BAND_B),
    (LANE_R, X["5V"].y), (X["5V"].x, X["5V"].y))
trk("+5V", "top", (LANE_R, X["5V"].y), (LANE_R, LS["HV"].y), (LS["HV"].x, LS["HV"].y))
trk("+5V", "top", (J1[3].x, J1[3].y), (J1[3].x, BAND_T), (LANE_R, BAND_T),
    (LANE_R, LS["HV"].y))

trk("GND", "bottom", (J2[2].x, J2[2].y), (J2[2].x, BAND_B), (LANE_R, BAND_B),
    (LANE_R, X["GND"].y), (X["GND"].x, X["GND"].y))
trk("GND", "bottom", (LANE_R, X["GND"].y), (LANE_R, LS["GND_H"].y),
    (LS["GND_H"].x, LS["GND_H"].y))
trk("GND", "bottom", (J1[2].x, J1[2].y), (J1[2].x, BAND_T), (LANE_R, BAND_T),
    (LANE_R, LS["GND_H"].y))
# The shifter's two grounds join across the middle corridor on the other layer.
trk("GND", "top", (LS["GND_H"].x, LS["GND_H"].y), (LS["GND_L"].x, LS["GND_L"].y))

# 3V3 leaves the shifter into the corridor, steps down a row, and crosses the
# HV column in the clear window between HV2 and the HV rail -- 1.27 mm of pitch
# either side, which leaves 0.295 mm of copper-to-copper.
trk("+3V3", "top", (LS["LV"].x, LS["LV"].y), (COR1, LS["LV"].y),
    (COR1, X["3V3"].y), (X["3V3"].x, X["3V3"].y), w=0.30)

# ============================================================================
# silkscreen
# ============================================================================
texts = [
    Text("PNL", 0.9, 0.50, 0.8),
    Text("CTL", 0.9, BOARD_H - 1.30, 0.8),
    Text("D8 CN2", COR0 + 0.2, CY - 0.50, 0.8),
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
    mech = mechanical()
    pm = check_firmware_pinmap()
    print("pin map        " + "  ".join(f"{k.split('_',1)[1].lower()}={v}"
                                        for k, v in BOARD_PINMAP.items()))
    if pm:
        print("PIN MAP        MISMATCH vs firmware/include/config.h")
        for e in pm: print("   ", e)
    else:
        print("PIN MAP        matches firmware/include/config.h")
    for name, x0, y0, x1, y1 in bodies():
        print(f"body {name:<9} {x1-x0:.1f} x {y1-y0:.1f} mm   "
              f"x {x0:.2f}..{x1:.2f}   y {y0:.2f}..{y1:.2f}")
    if mech:
        print("MECHANICAL     FAIL")
        for m in mech: print("   ", m)
    else:
        print("MECHANICAL     PASS — module bodies clear the connectors")
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
    return 1 if (errs or conn or pm or mech) else 0

if __name__ == "__main__":
    sys.exit(main())
