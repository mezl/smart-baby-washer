#!/usr/bin/env python3
"""Panelise the CN2 interceptor for a JLCPCB minimum order.

JLCPCB's minimum order is 5 pieces and its base tier covers any single design
up to 100 x 100 mm, so a 93 x 81 mm panel of 15 boards costs what one 18.6 x 27
board costs. Five panels is 75 boards.

V-SCORING, not mouse bites. The boards abut with no gap, which is what makes 15
fit; scoring needs a straight line running the full width or height of the
panel, and a grid of identical rectangles is exactly that. It also costs no
board area, where tab-routing would need a ~2 mm slot between every pair and
drop the count.

The copper is safe for it without changing anything: gen_pcb enforces a 0.5 mm
edge keepout on every pad and track, and V-scoring wants 0.4 mm.

    python3 gen_panel.py            # 5 x 3, the largest that fits
    python3 gen_panel.py 3 2        # or pick your own

Everything is emitted from gen_pcb's model, so the panel cannot drift from the
board it repeats.
"""
import os, sys
import gen_pcb as B

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "gerbers-panel")

# JLCPCB's base tier. Beyond this the order moves up a price bracket, which is
# the entire reason to panelise in the first place.
MAX_MM = 100.0


def offsets(cols, rows):
    return [(c*B.BOARD_W, r*B.BOARD_H) for r in range(rows) for c in range(cols)]


def write_copper(path, layer, fn, offs):
    keys = [("C", p.w) if p.shape == "circle" else ("R", p.w, p.h) for p in B.pads]
    keys += [("C", t.w) for t in B.tracks if t.layer == layer]
    ap, order = B.apertures(keys)
    with open(path, "w") as f:
        B.hdr(f, fn)
        for k in order: f.write(f"%ADD{ap[k]}{B.defn(k)}*%\n")
        cur = None
        for dx, dy in offs:
            for p in B.pads:
                k = ("C", p.w) if p.shape == "circle" else ("R", p.w, p.h)
                if ap[k] != cur: f.write(f"D{ap[k]}*\n"); cur = ap[k]
                f.write(f"X{B.g(p.x+dx)}Y{B.g(p.y+dy)}D03*\n")
            for t in B.tracks:
                if t.layer != layer: continue
                k = ("C", t.w)
                if ap[k] != cur: f.write(f"D{ap[k]}*\n"); cur = ap[k]
                f.write(f"X{B.g(t.pts[0][0]+dx)}Y{B.g(t.pts[0][1]+dy)}D02*\n")
                for q in t.pts[1:]:
                    f.write(f"X{B.g(q[0]+dx)}Y{B.g(q[1]+dy)}D01*\n")
        f.write("M02*\n")


def write_mask(path, fn, offs):
    keys = [("C", p.w+0.1) if p.shape == "circle" else ("R", p.w+0.1, p.h+0.1)
            for p in B.pads]
    ap, order = B.apertures(keys)
    with open(path, "w") as f:
        B.hdr(f, fn)
        for k in order: f.write(f"%ADD{ap[k]}{B.defn(k)}*%\n")
        cur = None
        for dx, dy in offs:
            for p in B.pads:
                k = ("C", p.w+0.1) if p.shape == "circle" else ("R", p.w+0.1, p.h+0.1)
                if ap[k] != cur: f.write(f"D{ap[k]}*\n"); cur = ap[k]
                f.write(f"X{B.g(p.x+dx)}Y{B.g(p.y+dy)}D03*\n")
        f.write("M02*\n")


def write_silk(path, fn, offs):
    segs = B.silk_segments()
    with open(path, "w") as f:
        B.hdr(f, fn)
        f.write("%ADD10C,0.1500*%\nD10*\n")
        for dx, dy in offs:
            for (x1, y1, x2, y2) in segs:
                f.write(f"X{B.g(x1+dx)}Y{B.g(y1+dy)}D02*"
                        f"X{B.g(x2+dx)}Y{B.g(y2+dy)}D01*\n")
        f.write("M02*\n")


def write_outline(path, pw, ph):
    """The panel border ONLY.

    The internal score lines deliberately do NOT go in here. Anything on the
    outline layer is read as a route, and a router following those lines would
    cut the panel into 15 loose boards at the fab instead of scoring it."""
    with open(path, "w") as f:
        B.hdr(f, "Profile,NP")
        f.write("%ADD10C,0.1000*%\nD10*\n")
        pts = [(0,0),(pw,0),(pw,ph),(0,ph),(0,0)]
        f.write(f"X{B.g(pts[0][0])}Y{B.g(pts[0][1])}D02*\n")
        for p in pts[1:]: f.write(f"X{B.g(p[0])}Y{B.g(p[1])}D01*\n")
        f.write("M02*\n")


def write_vcut(path, cols, rows, pw, ph):
    """Score lines, on their own layer for the fab to read as V-cuts."""
    with open(path, "w") as f:
        B.hdr(f, "Other,Vcut")
        f.write("%ADD10C,0.1000*%\nD10*\n")
        for c in range(1, cols):
            x = c*B.BOARD_W
            f.write(f"X{B.g(x)}Y{B.g(0)}D02*X{B.g(x)}Y{B.g(ph)}D01*\n")
        for r in range(1, rows):
            y = r*B.BOARD_H
            f.write(f"X{B.g(0)}Y{B.g(y)}D02*X{B.g(pw)}Y{B.g(y)}D01*\n")
        f.write("M02*\n")


def write_drill(path, offs):
    holes = {}
    for dx, dy in offs:
        for p in B.pads:
            if p.drill:
                holes.setdefault(round(p.drill, 3), []).append((p.x+dx, p.y+dy))
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


def render_svg(path, cols, rows, pw, ph, offs):
    S = 7
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{pw*S:.0f}" '
         f'height="{ph*S:.0f}" viewBox="-2 -2 {pw+4} {ph+4}">',
         f'<rect x="0" y="0" width="{pw}" height="{ph}" fill="#0d5c2a"/>']
    for dx, dy in offs:
        for t in B.tracks:
            col = "#e74c3c" if t.layer == "top" else "#3498db"
            d = " ".join(("M" if i == 0 else "L")+f"{p[0]+dx:.3f},{p[1]+dy:.3f}"
                         for i, p in enumerate(t.pts))
            o.append(f'<path d="{d}" stroke="{col}" stroke-width="{t.w}" '
                     f'fill="none" stroke-linecap="round" opacity="0.9"/>')
        for p in B.pads:
            o.append(f'<circle cx="{p.x+dx:.3f}" cy="{p.y+dy:.3f}" '
                     f'r="{p.w/2:.3f}" fill="#e8c547"/>')
            if p.drill:
                o.append(f'<circle cx="{p.x+dx:.3f}" cy="{p.y+dy:.3f}" '
                         f'r="{p.drill/2:.3f}" fill="#0d5c2a"/>')
        for (x1,y1,x2,y2) in B.silk_segments():
            o.append(f'<line x1="{x1+dx:.3f}" y1="{y1+dy:.3f}" x2="{x2+dx:.3f}" '
                     f'y2="{y2+dy:.3f}" stroke="#fff" stroke-width="0.13"/>')
    for c in range(1, cols):
        o.append(f'<line x1="{c*B.BOARD_W}" y1="0" x2="{c*B.BOARD_W}" y2="{ph}" '
                 f'stroke="#fff" stroke-width="0.25" stroke-dasharray="1.5,1"/>')
    for r in range(1, rows):
        o.append(f'<line x1="0" y1="{r*B.BOARD_H}" x2="{pw}" y2="{r*B.BOARD_H}" '
                 f'stroke="#fff" stroke-width="0.25" stroke-dasharray="1.5,1"/>')
    o.append(f'<rect x="0" y="0" width="{pw}" height="{ph}" fill="none" '
             f'stroke="#eee" stroke-width="0.3"/>')
    o.append('</svg>')
    open(path, "w").write("\n".join(o))


def main():
    cols = int(sys.argv[1]) if len(sys.argv) > 2 else 5
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    pw, ph = cols*B.BOARD_W, rows*B.BOARD_H

    # The board itself must still be good; a panel of a broken board is 15
    # broken boards.
    errs, _ = B.drc()
    conn = B.connectivity()
    mech = B.mechanical()
    pinm = B.check_firmware_pinmap()
    if errs or conn or mech or pinm:
        print("refusing to panelise — run gen_pcb.py, the board does not pass")
        return 1

    os.makedirs(OUT, exist_ok=True)
    offs = offsets(cols, rows)
    P = f"{OUT}/cn2-interceptor-panel"
    write_copper(f"{P}-F_Cu.gbr", "top", "Copper,L1,Top", offs)
    write_copper(f"{P}-B_Cu.gbr", "bottom", "Copper,L2,Bot", offs)
    write_mask(f"{P}-F_Mask.gbr", "Soldermask,Top", offs)
    write_mask(f"{P}-B_Mask.gbr", "Soldermask,Bot", offs)
    write_silk(f"{P}-F_Silkscreen.gbr", "Legend,Top", offs)
    write_outline(f"{P}-Edge_Cuts.gbr", pw, ph)
    write_vcut(f"{P}-V_Cut.gbr", cols, rows, pw, ph)
    holes = write_drill(f"{P}-PTH.drl", offs)
    render_svg(f"{OUT}/preview-panel.svg", cols, rows, pw, ph, offs)

    fits = pw <= MAX_MM and ph <= MAX_MM
    print(f"board          {B.BOARD_W} x {B.BOARD_H} mm")
    print(f"panel          {cols} x {rows} = {cols*rows} boards, "
          f"{pw:.1f} x {ph:.1f} mm")
    print(f"JLCPCB tier    {'within' if fits else 'OVER'} {MAX_MM:.0f} x "
          f"{MAX_MM:.0f} mm"
          + ("" if fits else "  <-- this will cost more, pick a smaller grid"))
    print(f"per order      5 panels minimum = {5*cols*rows} boards")
    print(f"holes          {sum(len(v) for v in holes.values())}")
    print(f"separation     V-score, {cols-1} vertical + {rows-1} horizontal "
          f"lines, on -V_Cut.gbr")
    print(f"copper to score line  {B.EDGE_KEEPOUT} mm (V-cut wants 0.4)")
    return 0 if fits else 1


if __name__ == "__main__":
    sys.exit(main())
