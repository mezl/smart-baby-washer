#!/usr/bin/env python3
"""Emit a native KiCad schematic (.kicad_sch) from the same model as the board.

pcbnew can build a .kicad_pcb but has no schematic API, so this writes the
S-expression format directly. Nets are joined by **net labels** rather than drawn
wires: every pin gets a short stub and a label, and KiCad connects same-named
labels. That is electrically identical to wiring them and stays readable with 14
nets across 6 parts.

    python3 gen_kicad_sch.py   ->  cn2-interceptor.kicad_sch

Open it with the .kicad_pcb of the same name in a KiCad project to cross-probe.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_pcb as B

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "cn2-interceptor.kicad_sch")

_n = [0]
def uuid():
    _n[0] += 1
    return f"00000000-0000-0000-0000-{_n[0]:012d}"

# Which pads sit on the left of each symbol; the rest go on the right.
LEFT_COUNT = {"J1": 4, "J2": 4, "J3": 3, "J4": 3, "U1": 7, "U2": 6}
VALUE = {
    "J1": "JST-XH 4P  CONTROLLER CN2",
    "J2": "JST-XH 4P  PANEL CN2",
    "J3": "JST-XH 3P  FLOW METER",
    "J4": "JST-XH 3P  FV TO CONTROLLER",
    "U1": "XIAO ESP32-C3",
    "U2": "4CH BSS138 LEVEL SHIFTER",
}
# Where each symbol sits on the sheet, in mm on a 2.54 grid.
# Spacing has to clear a stub plus a net label on both facing sides, which is
# roughly 30 mm per side -- symbols any closer and the labels collide.
PLACE = {"J2": (68.58, 121.92), "U2": (172.72, 121.92), "U1": (292.10, 121.92),
         "J1": (398.78, 121.92), "J3": (203.20, 251.46), "J4": (332.74, 251.46)}

PITCH = 2.54
STUB  = 5.08


def pads_of(ref):
    return [p for p in B.pads if p.ref == ref]


def sym_pin_xy(ref, idx, n_left, n_total, w):
    """Pin connection point in symbol-local coordinates. KiCad's Y is up."""
    if idx < n_left:
        rows = n_left
        y = (rows - 1) * PITCH / 2 - idx * PITCH
        return (-w / 2 - STUB, y, 0)          # points right, into the body
    j = idx - n_left
    rows = n_total - n_left
    y = (rows - 1) * PITCH / 2 - j * PITCH
    return (w / 2 + STUB, y, 180)


def lib_symbol(ref):
    ps = pads_of(ref)
    n_left = LEFT_COUNT[ref]
    n = len(ps)
    rows = max(n_left, n - n_left)
    h = (rows + 1) * PITCH
    w = 30.48 if ref in ("U1", "U2") else 20.32
    o = [f'    (symbol "cn2:{ref}_SYM" (pin_names (offset 1.016)) (in_bom yes) (on_board yes)']
    o.append(f'      (property "Reference" "{ref[0]}" (at 0 {h/2+2.54:.2f} 0)'
             f' (effects (font (size 1.27 1.27))))')
    o.append(f'      (property "Value" "{VALUE[ref]}" (at 0 {-h/2-2.54:.2f} 0)'
             f' (effects (font (size 1.27 1.27))))')
    o.append(f'      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))')
    o.append(f'      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))')
    o.append(f'      (symbol "{ref}_SYM_0_1"')
    o.append(f'        (rectangle (start {-w/2:.2f} {h/2:.2f}) (end {w/2:.2f} {-h/2:.2f})')
    o.append(f'          (stroke (width 0.254) (type default)) (fill (type background)))')
    o.append('      )')
    o.append(f'      (symbol "{ref}_SYM_1_1"')
    for i, p in enumerate(ps):
        x, y, rot = sym_pin_xy(ref, i, n_left, n, w)
        o.append(f'        (pin passive line (at {x:.2f} {y:.2f} {rot}) (length {STUB:.2f})')
        o.append(f'          (name "{p.name}" (effects (font (size 1.27 1.27))))')
        o.append(f'          (number "{p.name}" (effects (font (size 1.27 1.27)))))')
    o.append('      )')
    o.append('    )')
    return o


def main():
    errs, _ = B.drc()
    conn = B.connectivity()
    if errs or conn:
        print("refusing to emit: the board model does not pass its own checks")
        for e in (errs + conn)[:20]: print("   ", e)
        return 1

    o = ['(kicad_sch (version 20230121) (generator gen_kicad_sch)',
         f'  (uuid {uuid()})', '  (paper "A2")',
         '  (title_block (title "D8 CN2 interceptor") (rev "A")',
         '    (comment 1 "Generated from hardware/pcb/gen_pcb.py -- do not hand-edit"))',
         '  (lib_symbols']
    for ref in PLACE:
        o += lib_symbol(ref)
    o.append('  )')

    # --- component instances, plus a stub and a net label on every pin --------
    for ref, (sx, sy) in PLACE.items():
        ps = pads_of(ref)
        n_left = LEFT_COUNT[ref]
        n = len(ps)
        rows = max(n_left, n - n_left)
        h = (rows + 1) * PITCH
        w = 30.48 if ref in ("U1", "U2") else 20.32
        su = uuid()
        o.append(f'  (symbol (lib_id "cn2:{ref}_SYM") (at {sx:.2f} {sy:.2f} 0) (unit 1)')
        o.append(f'    (in_bom yes) (on_board yes) (dnp no)')
        o.append(f'    (uuid {su})')
        o.append(f'    (property "Reference" "{ref}" (at {sx:.2f} {sy-h/2-2.54:.2f} 0)'
                 f' (effects (font (size 1.27 1.27))))')
        o.append(f'    (property "Value" "{VALUE[ref]}" (at {sx:.2f} {sy+h/2+2.54:.2f} 0)'
                 f' (effects (font (size 1.27 1.27))))')
        o.append(f'    (property "Footprint" "" (at {sx:.2f} {sy:.2f} 0)'
                 f' (effects (font (size 1.27 1.27)) hide))')
        o.append(f'    (property "Datasheet" "" (at {sx:.2f} {sy:.2f} 0)'
                 f' (effects (font (size 1.27 1.27)) hide))')
        for p in ps:
            o.append(f'    (pin "{p.name}" (uuid {uuid()}))')
        o.append(f'    (instances (project "cn2-interceptor"')
        o.append(f'      (path "/{su}" (reference "{ref}") (unit 1))))')
        o.append('  )')

        for i, p in enumerate(ps):
            lx, ly, rot = sym_pin_xy(ref, i, n_left, n, w)
            # sheet coords: symbol Y is inverted relative to the sheet
            px, py = sx + lx, sy - ly
            ex = px - STUB if rot == 0 else px + STUB
            o.append(f'  (wire (pts (xy {px:.2f} {py:.2f}) (xy {ex:.2f} {py:.2f}))')
            o.append(f'    (stroke (width 0) (type default)) (uuid {uuid()}))')
            net = p.net if p.net and p.net != "NC" else None
            if net:
                just = "right" if rot == 0 else "left"
                o.append(f'  (label "{net}" (at {ex:.2f} {py:.2f} 0)'
                         f' (effects (font (size 1.27 1.27)) (justify {just} bottom))'
                         f' (uuid {uuid()}))')
            else:
                o.append(f'  (no_connect (at {ex:.2f} {py:.2f}) (uuid {uuid()}))')

    o.append('  (sheet_instances (path "/" (page "1")))')
    o.append(')')
    open(OUT, "w").write("\n".join(o) + "\n")

    nets = sorted(n for n in B.NETS if n != "NC")
    nc = sum(1 for p in B.pads if p.net == "NC")
    print(f"wrote {os.path.basename(OUT)}")
    print(f"  {len(PLACE)} symbols, {len(B.pads)} pins, {len(nets)} nets, {nc} no-connects")
    print(f"  nets: {', '.join(nets)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
