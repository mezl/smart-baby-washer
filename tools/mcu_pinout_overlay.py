#!/usr/bin/env python3
"""Overlay the BF7612DM16-SJLX pinout on the macro of the fitted chip.

Pin positions are measured off the photo (gull-wing shoulders), not fitted
to an idealised package -- the macro has enough perspective that the two
rows differ in both pitch and centre. Pin 1 is set by the moulded dimple
at the top-right of the package; numbering then runs counter-clockwise.
"""
import math
from PIL import Image, ImageDraw, ImageFont

S = "/tmp/claude-1000/-home-kai-software-momcozy-d8/75cadcaa-ea43-4cc3-b05f-8a5941ae3849/scratchpad"
SRC = f"{S}/chip.png"
OUT = f"{S}/mcu_pinout_on_board.jpg"

PAD_T, PAD_B, PAD_L, PAD_R = 700, 780, 120, 300
DIMPLE = (1495, 505)

# measured pad centres, chip.png coords
TOPX = [1580, 1390, 1200, 1020, 830, 635, 445, 250]   # pins 1..8, right -> left
BOTX = [130, 320, 520, 720, 920, 1130, 1350, 1560]    # pins 9..16, left -> right


def ytop(x):
    return 501 - 0.0852 * (x - 39) - 95


def ybot(x):
    return 1079 - 0.0238 * (x - 36) + 165


PINS = {
    1:  ("PD6", "RXD0_E · INT1 · ADC22",       "uart"),
    2:  ("PD3", "XTAL_IN · ADC19",             "xtal"),
    3:  ("PD2", "XTAL_OUT · ADC18",            "xtal"),
    4:  ("PD1", "PWM2_A · ADC17",              "io"),
    5:  ("PB6", "RXD1_B · ADC06",              "io"),
    6:  ("PB5", "SCL0_B · PWM0_A1 · ADC05",    "io"),
    7:  ("PB4", "TXD0_B · ADC04",              "uart"),
    8:  ("PB3", "RXD0_B · PWM0_D · ADC03",     "uart"),
    9:  ("PB2", "TXD1_A · PWM0_C · ADC02",     "io"),
    10: ("PB1", "RXD1_A · PWM0_B · ADC01",     "io"),
    11: ("PB0", "PWM0_A · ADC00 · COM0",       "io"),
    12: ("VSS", "ground",                       "gnd"),
    13: ("VCC", "supply",                       "vcc"),
    14: ("PA1", "TXD0_A/_E · SDA0_A · SWE0",   "uart"),
    15: ("PA0", "RXD0_A · TXD0_F · SCL0_A",    "uart"),
    16: ("PD7", "RXD0_F · INT2 · PGC2",        "uart"),
}
COL = {"uart": (95, 205, 255), "xtal": (198, 158, 255), "gnd": (238, 238, 238),
       "vcc": (255, 88, 88), "io": (255, 212, 90)}

FDIR = "/usr/share/fonts/truetype/dejavu"


def main():
    base = Image.open(SRC).convert("RGB")
    W, H = base.size
    cv = Image.new("RGB", (W + PAD_L + PAD_R, H + PAD_T + PAD_B), (13, 15, 19))
    cv.paste(base, (PAD_L, PAD_T))
    d = ImageDraw.Draw(cv)

    f_port = ImageFont.truetype(f"{FDIR}/DejaVuSans-Bold.ttf", 31)
    f_alt = ImageFont.truetype(f"{FDIR}/DejaVuSansCondensed.ttf", 24)
    f_num = ImageFont.truetype(f"{FDIR}/DejaVuSans-Bold.ttf", 40)
    f_ttl = ImageFont.truetype(f"{FDIR}/DejaVuSans-Bold.ttf", 44)
    f_sub = ImageFont.truetype(f"{FDIR}/DejaVuSansCondensed.ttf", 28)

    for n in range(1, 17):
        top = n <= 8
        x = (TOPX[n - 1] if top else BOTX[n - 9]) + PAD_L
        y = (ytop(TOPX[n - 1]) if top else ybot(BOTX[n - 9])) + PAD_T
        port, alt, cls = PINS[n]
        c = COL[cls]

        ly = PAD_T - 30 if top else PAD_T + H + 30
        d.line([(x, y), (x, ly)], fill=c, width=3)
        d.ellipse([x - 27, y - 27, x + 27, y + 27], outline=c, width=7)

        strip = Image.new("RGBA", (600, 92), (0, 0, 0, 0))
        sd = ImageDraw.Draw(strip)
        sd.text((0, 0), f"{n}  {port}", font=f_port, fill=c)
        sd.text((0, 42), alt, font=f_alt, fill=(198, 205, 214))
        strip = strip.rotate(90, expand=True)
        sw, sh = strip.size
        cv.paste(strip, (int(x - 48), int(ly - sh) if top else int(ly)), strip)

        by = y - 64 if top else y + 64
        d.text((x, by), str(n), font=f_num, fill=c, anchor="mm",
               stroke_width=5, stroke_fill=(8, 10, 14))

    # pin-1 dimple
    dx, dy = DIMPLE[0] + PAD_L, DIMPLE[1] + PAD_T
    d.ellipse([dx - 66, dy - 66, dx + 66, dy + 66], outline=(255, 55, 55), width=8)
    d.line([(dx + 50, dy + 52), (dx + 210, dy + 190)], fill=(255, 55, 55), width=6)
    d.text((dx + 150, dy + 210), "moulded dimple", font=f_port, fill=(255, 95, 95),
           stroke_width=5, stroke_fill=(8, 10, 14))
    d.text((dx + 150, dy + 248), "= pin 1", font=f_port, fill=(255, 95, 95),
           stroke_width=5, stroke_fill=(8, 10, 14))

    d.text((PAD_L, 28), "FMD BF7612DM16-SJLX (SOP-16)", font=f_ttl, fill=(246, 248, 251))
    d.text((PAD_L, 84), "Momcozy D8 controller BBW04001-UL-P · copper side, viewed from above · "
                        "pins 1-8 top row right-to-left, 9-16 bottom row left-to-right",
           font=f_sub, fill=(150, 158, 170))

    y0 = H + PAD_T + PAD_B - 128
    x = PAD_L
    for c, lab in [((95, 205, 255), "UART0-capable — CN2 lands on one of these"),
                   ((255, 88, 88), "VCC"), ((238, 238, 238), "VSS"),
                   ((198, 158, 255), "XTAL (no crystal fitted)"),
                   ((255, 212, 90), "general I/O")]:
        d.rectangle([x, y0 + 6, x + 28, y0 + 34], fill=c)
        d.text((x + 40, y0 + 3), lab, font=f_sub, fill=(212, 218, 226))
        x += 52 + int(d.textlength(lab, font=f_sub))

    d.text((PAD_L, y0 + 56),
           "Pin 1 is from the package dimple and is corroborated by C9, the decoupling cap, "
           "sitting under pins 12/13.",
           font=f_sub, fill=(190, 197, 206))
    d.text((PAD_L, y0 + 92),
           "NOT ONE PIN HERE HAS BEEN TRACED TO A NET. Ring it out before you probe or inject.",
           font=f_sub, fill=(255, 168, 84))

    cv.convert("RGB").save(OUT, quality=93)
    print("wrote", OUT, cv.size)


if __name__ == "__main__":
    main()
