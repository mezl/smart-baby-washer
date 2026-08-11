#!/usr/bin/env python3
"""Build the board natively in KiCad from the same model, then export with KiCad.

gen_pcb.py owns the geometry. This turns it into a real .kicad_pcb via the
pcbnew Python API, so the file can be opened, DRC'd and re-exported by KiCad
itself rather than trusting my own Gerber writer.

    python3 gen_kicad.py
    kicad-cli pcb export gerbers --output gerbers-kicad cn2-interceptor.kicad_pcb
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_pcb as B
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "cn2-interceptor.kicad_pcb")

def mm(v):  return pcbnew.FromMM(float(v))
def pt(x, y): return pcbnew.VECTOR2I(mm(x), mm(y))

# KiCad's Y axis points down in file coordinates, same as gen_pcb's model, so the
# geometry carries over unchanged.

def main():
    errs, _ = B.drc()
    conn = B.connectivity()
    if errs or conn:
        print("refusing to build: the model does not pass its own checks")
        for e in (errs + conn)[:20]: print("   ", e)
        return 1

    board = pcbnew.BOARD()

    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)

    nets = {}
    for name in B.NETS:
        if name == "NC": continue
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        nets[name] = ni

    # --- footprints, one per reference designator ---------------------------
    by_ref = {}
    for p in B.pads:
        by_ref.setdefault(p.ref, []).append(p)

    # Derived, not hand-listed: adding a connector to gen_pcb.py used to crash
    # here with a KeyError until this dict was updated by hand, twice.
    def fpname(ref, ps):
        known = {"U1": "XIAO_ESP32C3_2X7", "U2": "LEVELSHIFT_2X6"}
        if ref in known: return known[ref]
        return f"JST_XH_{len(ps)}" if ref.startswith("J") else f"GENERIC_{len(ps)}P"
    VALUES = {"J1": "JST-XH 4P (CONTROLLER)", "J2": "JST-XH 4P (PANEL)",
              "J3": "JST-XH 3P (FLOW METER)", "J4": "JST-XH 3P (FV TO CONTROLLER)",
              "U1": "XIAO ESP32-C3", "U2": "4CH BSS138 LEVEL SHIFTER"}

    for ref, ps in by_ref.items():
        fp = pcbnew.FOOTPRINT(board)
        # Give each footprint a library id, or DRC reports lib_footprint_issues
        # for every one of them.
        fp.SetFPID(pcbnew.LIB_ID("cn2interceptor", fpname(ref, ps)))
        fp.SetReference(ref)
        fp.SetValue(VALUES.get(ref, ""))
        fp.SetPosition(pt(ps[0].x, ps[0].y))
        # The auto-placed reference and value land on top of each other at the
        # footprint origin. Keep them in the file but off the silkscreen; the
        # board-level text below carries the legend.
        fp.Reference().SetLayer(pcbnew.F_Fab)
        fp.Value().SetLayer(pcbnew.F_Fab)
        board.Add(fp)
        for p in ps:
            pad = pcbnew.PAD(fp)
            pad.SetNumber(p.name)
            pad.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
            pad.SetShape(pcbnew.PAD_SHAPE_RECT if p.shape == "rect"
                         else pcbnew.PAD_SHAPE_CIRCLE)
            pad.SetSize(pcbnew.VECTOR2I(mm(p.w), mm(p.h)))
            pad.SetDrillSize(pcbnew.VECTOR2I(mm(p.drill), mm(p.drill)))
            pad.SetLayerSet(pad.PTHMask())
            # Pos0 is the pad's offset from the footprint origin. Setting only
            # SetPosition() leaves Pos0 at zero, and KiCad recomputes absolute
            # position from Pos0 on save -- every pad collapses onto the origin.
            pad.SetPos0(pcbnew.VECTOR2I(mm(p.x - ps[0].x), mm(p.y - ps[0].y)))
            pad.SetPosition(pt(p.x, p.y))
            if p.net and p.net != "NC":
                pad.SetNet(nets[p.net])
            fp.Add(pad)
        fp.SetReference(ref)

    # --- tracks --------------------------------------------------------------
    for t in B.tracks:
        layer = pcbnew.F_Cu if t.layer == "top" else pcbnew.B_Cu
        for i in range(len(t.pts) - 1):
            tr = pcbnew.PCB_TRACK(board)
            tr.SetStart(pt(*t.pts[i]))
            tr.SetEnd(pt(*t.pts[i+1]))
            tr.SetWidth(mm(t.w))
            tr.SetLayer(layer)
            tr.SetNet(nets[t.net])
            board.Add(tr)

    # --- board outline -------------------------------------------------------
    W, H = B.BOARD_W, B.BOARD_H
    for (x1, y1, x2, y2) in ((0,0,W,0), (W,0,W,H), (W,H,0,H), (0,H,0,0)):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(pt(x1, y1)); seg.SetEnd(pt(x2, y2))
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(mm(0.1))
        board.Add(seg)

    # --- silkscreen ----------------------------------------------------------
    for t in B.texts:
        tx = pcbnew.PCB_TEXT(board)
        tx.SetText(t.s)
        tx.SetPosition(pt(t.x, t.y + t.size/2))
        tx.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
        tx.SetLayer(pcbnew.F_SilkS)
        tx.SetTextSize(pcbnew.VECTOR2I(mm(t.size), mm(t.size)))
        tx.SetTextThickness(mm(0.15))
        board.Add(tx)

    pcbnew.SaveBoard(OUT, board)

    # KiCad's own DRC, not mine. This is the check that actually counts.
    rpt = os.path.join(HERE, "drc-report.txt")
    if os.path.exists(rpt): os.remove(rpt)
    try:
        pcbnew.WriteDRCReport(board, rpt, pcbnew.EDA_UNITS_MILLIMETRES, True)
        txt = open(rpt).read()
        import re
        viol = re.search(r"\*\* Found (\d+) DRC violations \*\*", txt)
        unc  = re.search(r"\*\* Found (\d+) unconnected pads \*\*", txt)
        foot = re.search(r"\*\* Found (\d+) Footprint errors \*\*", txt)
        print("KiCad DRC:")
        print(f"  violations       {viol.group(1) if viol else '?'}")
        print(f"  unconnected pads {unc.group(1) if unc else '?'}")
        print(f"  footprint errors {foot.group(1) if foot else '?'}")
        bad = [l for l in txt.splitlines() if l.strip().startswith("[")]
        for l in bad[:15]: print("   ", l.strip())
    except Exception as e:
        print(f"KiCad DRC unavailable: {e}")

    print(f"wrote {os.path.basename(OUT)}")
    print(f"  {len(by_ref)} footprints, {len(B.pads)} pads, "
          f"{sum(len(t.pts)-1 for t in B.tracks)} track segments, {len(nets)} nets")
    return 0

if __name__ == "__main__":
    sys.exit(main())
