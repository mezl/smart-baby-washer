#!/usr/bin/env python3
"""Emit Eagle .sch and .brd from the same model gen_pcb.py uses for the Gerbers.

Both files are generated from gen_pcb's placement, nets and tracks, so the
schematic, the board and the Gerbers cannot drift apart -- there is one source of
truth and three renderings of it.

    python3 gen_eagle.py    ->  cn2-interceptor.sch, cn2-interceptor.brd

Eagle 6+ stores .sch/.brd as XML with coordinates in millimetres. These import
into Eagle, Fusion 360 Electronics, and KiCad (File > Import > Non-KiCad Board).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_pcb as B

HERE = os.path.dirname(os.path.abspath(__file__))
EAGLE_VER = "9.6.2"

# Eagle layer numbers used here.
L_TOP, L_BOT, L_PAD, L_DIM, L_TPLACE, L_TNAME = 1, 16, 17, 20, 21, 25
L_NET, L_SYM, L_SNAME, L_SVAL = 91, 94, 95, 96

LAYERS = [
    (1,"Top","4","1","yes"), (16,"Bottom","1","1","yes"), (17,"Pads","2","1","yes"),
    (18,"Vias","2","1","yes"), (19,"Unrouted","6","1","yes"), (20,"Dimension","15","1","yes"),
    (21,"tPlace","7","1","yes"), (22,"bPlace","7","1","yes"), (23,"tOrigins","15","1","yes"),
    (24,"bOrigins","15","1","yes"), (25,"tNames","7","1","yes"), (26,"bNames","7","1","yes"),
    (27,"tValues","7","1","yes"), (28,"bValues","7","1","yes"), (29,"tStop","7","3","no"),
    (30,"bStop","7","6","no"), (44,"Drills","7","1","no"), (45,"Holes","7","1","no"),
    (91,"Nets","2","1","yes"), (92,"Busses","1","1","yes"), (93,"Pins","2","1","no"),
    (94,"Symbols","4","1","yes"), (95,"Names","7","1","yes"), (96,"Values","7","1","yes"),
    (97,"Info","7","1","yes"), (98,"Guide","6","1","yes"),
]

def hdr():
    o = ['<?xml version="1.0" encoding="utf-8"?>',
         '<!DOCTYPE eagle SYSTEM "eagle.dtd">',
         f'<eagle version="{EAGLE_VER}">', '<drawing>',
         '<settings><setting alwaysvectorfont="no"/>'
         '<setting verticaltext="up"/></settings>',
         '<grid distance="2.54" unitdist="mm" unit="mm" style="lines" multiple="1" '
         'display="no" altdistance="0.635" altunitdist="mm" altunit="mm"/>', '<layers>']
    for n, nm, col, fil, act in LAYERS:
        o.append(f'<layer number="{n}" name="{nm}" color="{col}" fill="{fil}" '
                 f'visible="yes" active="{act}"/>')
    o.append('</layers>')
    return o

# ---------------------------------------------------------------------------
# component definitions, taken straight from gen_pcb's placement
# ---------------------------------------------------------------------------
PARTS = [
    # ref, package, deviceset, pin names in pad order, value
    ("J1", "JST_XH_4",  "CONN_4", ["1","2","3","4"], "JST-XH 4P"),
    ("J2", "JST_XH_4",  "CONN_4", ["1","2","3","4"], "JST-XH 4P"),
    ("U2", "LS_2X6",    "LEVELSHIFT",
     ["HV1","HV2","HV","GND_H","HV3","HV4","LV1","LV2","LV","GND_L","LV3","LV4"],
     "4CH BSS138"),
    ("U1", "XIAO_2X7",  "XIAO_ESP32C3",
     ["D0","D1","D2","D3","D4","D5","D6","5V","GND","3V3","D10","D9","D8","D7"],
     "XIAO ESP32-C3"),
]

def pads_of(ref):
    return [p for p in B.pads if p.ref == ref]

def package_xml(name, ps, ref):
    """Footprint, drawn relative to the part origin (its first pad)."""
    ox, oy = ps[0].x, ps[0].y
    o = [f'<package name="{name}">']
    for p in ps:
        shape = "square" if p.shape == "rect" else "round"
        o.append(f'<pad name="{p.name}" x="{p.x-ox:.4f}" y="{-(p.y-oy):.4f}" '
                 f'drill="{p.drill:.2f}" diameter="{p.w:.2f}" shape="{shape}"/>')
    xs = [p.x-ox for p in ps]; ys = [-(p.y-oy) for p in ps]
    x0, x1 = min(xs)-1.3, max(xs)+1.3
    y0, y1 = min(ys)-1.3, max(ys)+1.3
    for (a,b,c,d) in ((x0,y0,x1,y0),(x1,y0,x1,y1),(x1,y1,x0,y1),(x0,y1,x0,y0)):
        o.append(f'<wire x1="{a:.3f}" y1="{b:.3f}" x2="{c:.3f}" y2="{d:.3f}" '
                 f'width="0.127" layer="{L_TPLACE}"/>')
    o.append(f'<text x="{x0:.3f}" y="{y1+0.4:.3f}" size="1.016" layer="{L_TNAME}">&gt;NAME</text>')
    o.append('</package>')
    return o

def symbol_xml(name, pins, left_n):
    """Simple box symbol: `left_n` pins down the left side, the rest down the right."""
    o = [f'<symbol name="{name}">']
    right = pins[left_n:]
    h = max(len(pins[:left_n]), len(right))
    W = 20.32
    top = 0.0
    o.append(f'<wire x1="0" y1="{top:.2f}" x2="{W:.2f}" y2="{top:.2f}" width="0.254" layer="{L_SYM}"/>')
    bot = top - (h+1)*2.54
    o.append(f'<wire x1="0" y1="{bot:.2f}" x2="{W:.2f}" y2="{bot:.2f}" width="0.254" layer="{L_SYM}"/>')
    o.append(f'<wire x1="0" y1="{top:.2f}" x2="0" y2="{bot:.2f}" width="0.254" layer="{L_SYM}"/>')
    o.append(f'<wire x1="{W:.2f}" y1="{top:.2f}" x2="{W:.2f}" y2="{bot:.2f}" width="0.254" layer="{L_SYM}"/>')
    for i, nm in enumerate(pins[:left_n]):
        y = top - (i+1)*2.54
        o.append(f'<pin name="{nm}" x="-5.08" y="{y:.2f}" visible="pin" length="middle" '
                 f'direction="pas"/>')
    for i, nm in enumerate(right):
        y = top - (i+1)*2.54
        o.append(f'<pin name="{nm}" x="{W+5.08:.2f}" y="{y:.2f}" visible="pin" '
                 f'length="middle" direction="pas" rot="R180"/>')
    o.append(f'<text x="0" y="{top+1.0:.2f}" size="1.778" layer="{L_SNAME}">&gt;NAME</text>')
    o.append(f'<text x="0" y="{bot-2.4:.2f}" size="1.778" layer="{L_SVAL}">&gt;VALUE</text>')
    o.append('</symbol>')
    return o

def library_xml():
    o = ['<library name="cn2interceptor">', '<packages>']
    for ref, pkg, ds, pins, val in PARTS:
        if ref == "J2": continue                       # shares J1's package
        o += package_xml(pkg, pads_of(ref), ref)
    o += ['</packages>', '<symbols>']
    for ref, pkg, ds, pins, val in PARTS:
        if ref == "J2": continue
        left = {"CONN_4": 4, "LEVELSHIFT": 6, "XIAO_ESP32C3": 7}[ds]
        o += symbol_xml(ds, pins, left)
    o += ['</symbols>', '<devicesets>']
    for ref, pkg, ds, pins, val in PARTS:
        if ref == "J2": continue
        prefix = "J" if ds == "CONN_4" else "U"
        o.append(f'<deviceset name="{ds}" prefix="{prefix}">')
        o.append(f'<gates><gate name="G$1" symbol="{ds}" x="0" y="0"/></gates>')
        o.append(f'<devices><device name="" package="{pkg}"><connects>')
        for nm in pins:
            o.append(f'<connect gate="G$1" pin="{nm}" pad="{nm}"/>')
        o.append('</connects><technologies><technology name=""/></technologies>'
                 '</device></devices></deviceset>')
    o += ['</devicesets>', '</library>']
    return o

CLASSES = ['<classes><class number="0" name="default" width="0" drill="0"/></classes>']

DESIGNRULES = f'''<designrules name="cn2">
<description language="en">Economy 2-layer, 6/6 mil capable process</description>
<param name="mdWireWire" value="{B.RULE_CLEARANCE}mm"/>
<param name="mdWirePad" value="{B.RULE_CLEARANCE}mm"/>
<param name="mdPadPad" value="{B.RULE_CLEARANCE}mm"/>
<param name="msWidth" value="0.2mm"/>
<param name="msDrill" value="0.3mm"/>
<param name="rlMinPadTop" value="{B.RULE_ANNULUS}mm"/>
</designrules>'''

# ---------------------------------------------------------------------------
def write_brd(path):
    o = hdr()
    o.append('<board>')
    o.append('<plain>')
    for (a,b,c,d) in ((0,0,B.BOARD_W,0), (B.BOARD_W,0,B.BOARD_W,B.BOARD_H),
                      (B.BOARD_W,B.BOARD_H,0,B.BOARD_H), (0,B.BOARD_H,0,0)):
        o.append(f'<wire x1="{a}" y1="{B.BOARD_H-b}" x2="{c}" y2="{B.BOARD_H-d}" '
                 f'width="0.1" layer="{L_DIM}"/>')
    for t in B.texts:
        o.append(f'<text x="{t.x:.3f}" y="{B.BOARD_H-t.y-t.size:.3f}" size="{t.size:.3f}" '
                 f'layer="{L_TPLACE}">{t.s}</text>')
    o.append('</plain>')
    o.append('<libraries>')
    o += library_xml()
    o.append('</libraries>')
    o.append('<attributes/><variantdefs/>')
    o += CLASSES
    o.append(DESIGNRULES)
    o.append('<autorouter/>')
    o.append('<elements>')
    for ref, pkg, ds, pins, val in PARTS:
        ps = pads_of(ref)
        o.append(f'<element name="{ref}" library="cn2interceptor" package="{pkg}" '
                 f'value="{val}" x="{ps[0].x:.4f}" y="{B.BOARD_H-ps[0].y:.4f}"/>')
    o.append('</elements>')
    o.append('<signals>')
    for name, ps in B.NETS.items():
        if name == "NC": continue
        o.append(f'<signal name="{name}">')
        for p in ps:
            o.append(f'<contactref element="{p.ref}" pad="{p.name}"/>')
        for t in B.tracks:
            if t.net != name: continue
            layer = L_TOP if t.layer == "top" else L_BOT
            for i in range(len(t.pts)-1):
                x1, y1 = t.pts[i]; x2, y2 = t.pts[i+1]
                o.append(f'<wire x1="{x1:.4f}" y1="{B.BOARD_H-y1:.4f}" '
                         f'x2="{x2:.4f}" y2="{B.BOARD_H-y2:.4f}" '
                         f'width="{t.w}" layer="{layer}"/>')
        o.append('</signal>')
    o.append('</signals>')
    o.append('</board></drawing></eagle>')
    open(path, "w").write("\n".join(o) + "\n")

def write_sch(path):
    # Schematic placement: connectors left, shifter middle, ESP32 right.
    place = {"J1": (20.32, 127.0), "J2": (20.32, 88.9),
             "U2": (76.2, 116.84), "U1": (137.16, 116.84)}
    o = hdr()
    o.append('<schematic xreflabel="%F%N/%S.%C%R" xrefpart="/%S.%C%R">')
    o.append('<libraries>')
    o += library_xml()
    o.append('</libraries>')
    o.append('<attributes/><variantdefs/>')
    o += CLASSES
    o.append('<modules/>')
    o.append('<parts>')
    for ref, pkg, ds, pins, val in PARTS:
        o.append(f'<part name="{ref}" library="cn2interceptor" deviceset="{ds}" '
                 f'device="" value="{val}"/>')
    o.append('</parts>')
    o.append('<sheets><sheet><plain>')
    o.append('<text x="12.7" y="160" size="3.5" layer="97">D8 CN2 INTERCEPTOR</text>')
    o.append('<text x="12.7" y="153" size="2.0" layer="97">'
             'CN2 pins 3/4 pass through J1-J2. Pins 1/2 go via U2 to the ESP32.</text>')
    o.append('</plain><instances>')
    for ref, pkg, ds, pins, val in PARTS:
        x, y = place[ref]
        o.append(f'<instance part="{ref}" gate="G$1" x="{x}" y="{y}"/>')
    o.append('</instances><busses/><nets>')

    # Pin coordinates in sheet space, so the net wires land on real pins.
    pinpos = {}
    for ref, pkg, ds, pins, val in PARTS:
        left = {"CONN_4": 4, "LEVELSHIFT": 6, "XIAO_ESP32C3": 7}[ds]
        px, py = place[ref]
        for i, nm in enumerate(pins[:left]):
            pinpos[(ref, nm)] = (px - 5.08, py - (i+1)*2.54)
        for i, nm in enumerate(pins[left:]):
            pinpos[(ref, nm)] = (px + 20.32 + 5.08, py - (i+1)*2.54)

    for name, ps in B.NETS.items():
        if name == "NC": continue
        o.append(f'<net name="{name}" class="0">')
        o.append('<segment>')
        for p in ps:
            o.append(f'<pinref part="{p.ref}" gate="G$1" pin="{p.name}"/>')
        # a spine at the mean y, with a stub from every pin, so the net is
        # visually connected as well as electrically declared
        pts = [pinpos[(p.ref, p.name)] for p in ps]
        spine_y = sum(q[1] for q in pts)/len(pts)
        xs = [q[0] for q in pts]
        o.append(f'<wire x1="{min(xs):.2f}" y1="{spine_y:.2f}" x2="{max(xs):.2f}" '
                 f'y2="{spine_y:.2f}" width="0.1524" layer="{L_NET}"/>')
        for (qx, qy) in pts:
            if abs(qy - spine_y) > 0.01:
                o.append(f'<wire x1="{qx:.2f}" y1="{qy:.2f}" x2="{qx:.2f}" '
                         f'y2="{spine_y:.2f}" width="0.1524" layer="{L_NET}"/>')
        o.append(f'<label x="{min(xs):.2f}" y="{spine_y+0.8:.2f}" size="1.27" '
                 f'layer="{L_NET}"/>')
        o.append('</segment></net>')
    o.append('</nets></sheet></sheets></schematic></drawing></eagle>')
    open(path, "w").write("\n".join(o) + "\n")

def main():
    errs, _ = B.drc()
    conn = B.connectivity()
    if errs or conn:
        print("refusing to emit: the board model does not pass its own checks")
        for e in (errs + conn)[:20]: print("   ", e)
        return 1
    write_brd(os.path.join(HERE, "cn2-interceptor.brd"))
    write_sch(os.path.join(HERE, "cn2-interceptor.sch"))
    import xml.etree.ElementTree as ET
    for f in ("cn2-interceptor.brd", "cn2-interceptor.sch"):
        ET.parse(os.path.join(HERE, f))
        print(f"{f:26s} well-formed XML")
    nets = [n for n in B.NETS if n != "NC"]
    print(f"parts {len(PARTS)}   nets {len(nets)}   "
          f"signals routed {len([t for t in B.tracks])}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
