# CN2 interceptor carrier board

A 18.6 × 27 mm, 2-layer carrier that sits in the CN2 cable between the D8
controller and its front panel. CN2 pins 3 and 4 (GND, +5 V) pass straight
through; pins 1 and 2 are routed through a level shifter to a XIAO ESP32-C3,
which then sees and can rewrite every byte in both directions.

<p align="center">
  <img src="preview-kicad.svg" width="420" alt="Board: red = top copper, blue = bottom copper. J2 (panel) on the bottom edge, J1 (controller) on the top edge, level shifter nested between the XIAO's pin rows.">
</p>

**J2 (panel) on the bottom edge, J1 (controller) on the top**, with the XIAO
between them and U2 nested *inside* the XIAO's 15.24 mm row gap — its own rows
are 10.16 mm apart, so it drops straight into the middle.

Standing the connectors on opposite edges rather than beside the XIAO is what
makes the board narrow: nothing sits left or right of the module except one
routing lane, so the width is the XIAO's row pitch plus that lane and two
edges. It also suits the cable run, since the two CN2 stubs leave the board in
opposite directions, the way they arrive.

### Which connector goes where is not arbitrary

Routing forces `LV1..LV4` onto `D1..D4` in order — they share a 0.94 mm channel
and any other order makes their spans overlap four deep. `D1..D4` are GPIO 3, 4,
5 and 6, and the firmware's as-built map is `rxb=5 txb=6 txp=3 rxp=4`: the
**controller** pair on GPIO5/6 and the **panel** pair on GPIO3/4. GPIO5/6 are
channels 3 and 4, which are the shifter's upper pins — so the controller has to
enter from the top edge.

Swap the connectors and the board still works, but only after swapping the four
`PIN_*` defines. `gen_pcb.py` reads `firmware/include/config.h` on every run and
fails if the two disagree, because a pin map that disagrees with the copper does
not announce itself: frames simply stop decoding.

## ⚠️ Mount U2 on the underside

Packing the shifter inside the XIAO's rows puts the two modules on top of each
other in plan view:

```
XIAO module   17.5 × 21.0 mm   x  0.22 .. 17.72    y  3.00 .. 24.00
shifter       15.7 × 13.2 mm   x  1.12 .. 16.82    y  6.90 .. 20.10
```

The shifter's outline sits entirely inside the XIAO's. On 2.54 mm sockets the
XIAO stands about 8.5 mm off the board and the shifter plus its own headers is
about 12 mm, so **they collide if both are fitted on top.**

**Solder U2 to the reverse side.** The pads are plated through-holes, so it makes
no difference electrically, and the two modules then sit back to back with the
PCB between them. Nothing in the Gerbers changes — it is purely an assembly
choice, but getting it wrong wastes a board.

If you would rather have both parts on top, put the shifter beside the XIAO
instead of between its rows; that costs roughly 14 mm of board length.

## Bill of materials — four parts

| Ref | Part | Notes |
|---|---|---|
| J1 | JST-XH 4-pin, 2.54 mm, vertical | **top** edge — cable to the **controller** CN2 |
| J2 | JST-XH 4-pin, 2.54 mm, vertical | **bottom** edge — cable to the **panel** CN2 |
| U1 | Seeed XIAO ESP32-C3 | 2×7, 2.54 mm pitch, 15.24 mm between rows |
| U2 | 4-channel BSS138 level shifter | 2×6, 2.54 mm pitch |

Plus two 1×7 and two 1×6 header strips if you want the modules socketed rather
than soldered down. Socketing the XIAO is worth it — it keeps `D5`, `D10` and
`D7` reachable for the flow and switch simulation, which is why the board carries
no separate header for them.

## The flow-meter relay is not on this revision

The previous board carried J3/J4, a relay for the flow-meter (FV) cable, so the
ESP32 could pass the real pulses through, rewrite their rate, or synthesise flow
on a dry bench. **It has been removed**, and that removal is most of the size
reduction: six more pins would not share an edge with the CN2 connectors, so
they forced the connectors back onto the left and right sides and the width from
18.6 mm to 31.5 mm — the flow relay cost more board than everything else on it.

If you put it back, the two findings that cost the most to learn are still worth
having:

- **The ESP32-C3 is not 5 V tolerant and there is no channel left to shift the
  flow signal** — all four converter channels are spoken for by CN2.
  `docs/hardware.md` records the meter as having a 10 kΩ internal pull-up and
  warns that running it at 5 V puts 5 V on a non-5V-tolerant GPIO. Measure FV
  Vcc first; if it is 5 V, `GPIO10` needs a divider, or feed the meter from the
  board's 3V3 so the whole signal drops to 3.3 V.
- **Cutting the line removes the meter's pull-up from the controller's input**,
  because that pull-up lives inside the meter and ends up on the far side of the
  break. The simulated output then has to drive the line actively, or the
  controller-side pin needs its own pull-up.

Only `GPIO7` (D5), `GPIO10` (D10) and `GPIO20` (D7) are free and safe on this
part — `GPIO2`, `GPIO8` and `GPIO9` are strapping pins and `GPIO21` emits ROM
boot chatter at every reset. All three stay reachable on the XIAO's own header
if you socket the module, which is the main reason to socket it.

## Connections

| ch | CN2 | | Converter | XIAO | UART |
|---|---|---|---|---|---|
| 1 | J2 PANEL pin 1 — RX, **input** | ← | `HV1` · `LV1` | `D1 / GPIO3` | `Serial1` **TX** |
| 2 | J2 PANEL pin 2 — TX, output | → | `HV2` · `LV2` | `D2 / GPIO4` | `Serial1` **RX** |
| 3 | J1 CONTROLLER pin 1 — TX, output | → | `HV3` · `LV3` | `D3 / GPIO5` | `Serial0` **RX** |
| 4 | J1 CONTROLLER pin 2 — RX, **input** | ← | `HV4` · `LV4` | `D4 / GPIO6` | `Serial0` **TX** |

That is `PIN_RX_BOARD 5`, `PIN_TX_BOARD 6`, `PIN_TX_PANEL 3`, `PIN_RX_PANEL 4` —
the as-built map already in `firmware/include/config.h`, so **a board built from
this needs no firmware change.** `gen_pcb.py` asserts it on every run.

Note this differs from the channel order in
[`docs/wiring.md`](../../docs/wiring.md#as-built--the-authoritative-table), which
describes the hand-wired module: there the controller landed on channels 1 and 2.
The electrical result is identical; only which pair uses which converter channel
moved, so that the connectors could sit on opposite edges.

**The rule that decides every connection is unchanged:** an *output* stub must
land on an ESP32 **RX** pin, an *input* stub must be **driven** by an ESP32 **TX**
pin. Wiring a TX pin to another output is the one way to damage a board.

Power: controller pin 4 = panel pin 4 = converter `HV` = XIAO `5V`, one net.
XIAO `3V3` → converter `LV`. All grounds common.

## ⚠️ Measure the level shifter before ordering

**Row spacing on 4-channel BSS138 modules is not standardised.** SparkFun's
BOB-12009 and most clones use 0.4″ (10.16 mm), but 0.5″ (12.7 mm) modules exist
and look identical in photos. Put calipers across yours, centre of one row to
centre of the other, then set `LS_ROW` at the top of `gen_pcb.py` and regenerate.

This is the single most likely way to get an unusable board back from the fab.

## How small it can go

18.6 × 27 mm is 502 mm², **39 % smaller than the 819 mm² it replaced** and
**41 % narrower** — which is the dimension that decides whether it fits a slot.

The width is now fully accounted for, and there is nothing left in it:

```
  0.5  edge
 16.8  XIAO pad block  (15.24 row pitch + 1.6 pad)
  0.4  clearance
  0.4  the one outer lane, shared by +5V (top) and GND (bottom)
  0.5  edge
 ─────
 18.6
```

The four CN2 signals never need a side lane at all: each connector pin sits
directly under the middle corridor, so it climbs straight out of its own pad
into its shifter pin, turning once. Only +5V and GND have to reach both the
XIAO's right-hand column and both connectors, and they share the single outer
lane one per layer. Remove that lane and the board is 17.8 mm, but those two
nets have nowhere to go.

**The height is set by the XIAO's body, not by its copper.** The pads span
15.24 mm; the module is 21.0 mm long, so it overhangs 2.88 mm at each end,
straight over the connectors. At 24 mm the modules physically sit on both JST
housings — the Gerbers look perfect and the boards arrive unbuildable.
`gen_pcb.py` now checks the module outlines against every connector pad on each
run for exactly this reason.

Going further would need **right-angle connectors** (the overhang stops
mattering and the board returns to ~24 mm), **discrete BSS138s instead of the
module**, which removes 12 through-holes and the nesting problem entirely, or
**4 layers**. Note that none of this saves money: JLCPCB charges the same for
anything up to 100 × 100 mm, so size only buys fit.

## Panelised for a JLCPCB minimum order

JLCPCB's minimum order is **5 pieces**, and its base tier covers any single
design up to **100 × 100 mm** — so a panel of copies costs what one small board
costs. `gen_panel.py` tiles the board into the largest grid that still fits:

```
  5 × 3 = 15 boards      panel 93.0 × 81.0 mm      5 panels = 75 boards
```

```bash
python3 gen_panel.py        # 5 x 3, the largest that fits
python3 gen_panel.py 3 2    # or any smaller grid
```

Output lands in `gerbers-panel/`, with `preview-panel.svg` to eyeball first.
The generator re-runs the board's DRC, connectivity, mechanical and pin-map
checks and refuses to panelise if any fail — a panel of a broken board is 15
broken boards.

### V-scoring, not mouse bites

The boards **abut with no gap**, which is what lets 15 fit. V-scoring needs a
straight line running the full width or height of the panel, and a grid of
identical rectangles is exactly that. Tab-routing would need a ~2 mm slot
between every pair and would cost a column.

Nothing had to change to make the copper safe for it: the board already keeps
every pad and track **0.5 mm** from its edge, and V-scoring wants 0.4 mm.

**The score lines are on their own layer** (`-V_Cut.gbr`), deliberately not on
`Edge_Cuts`. Anything on the outline layer is read as a route, and a router
following those lines would cut the panel into 15 loose boards at the fab
instead of scoring it. `Edge_Cuts` carries the panel border and nothing else.

### Ordering

Upload `gerbers-panel/` as a zip, then on the order page:

| Field | Value |
|---|---|
| Different Design | **1** |
| Delivery Format | **Panel by Customer** |
| Column × Row | **5 × 3** |
| Board size | 93 × 81 mm (JLC fills this from the Gerbers) |

Add a note asking for **V-scoring on the lines in `cn2-interceptor-panel-V_Cut.gbr`**.
Check their rendered preview before paying — it shows the score lines, and that
is the one place a misread would be visible before it is fabricated.

Panelised orders sometimes carry a small surcharge and the rules do change, so
confirm the price at upload rather than trusting this table. If it has moved,
`python3 gen_panel.py 2 2` gives four boards on a 37 × 54 mm panel, and a plain
single board is always `gerbers-kicad/`.

## Files

| | |
|---|---|
| `gen_pcb.py` | the board as explicit geometry — placement, nets, routing, its own DRC, preview |
| `gen_panel.py` | tiles the board into a JLCPCB-sized panel with V-score lines |
| `gen_kicad.py` | builds a native `.kicad_pcb` via the pcbnew API **and runs KiCad's own DRC** |
| `gen_eagle.py` | emits `.sch` and `.brd` from the same model |
| `gen_kicad_sch.py` | emits a native `.kicad_sch` from the same model |
| `cn2-interceptor.kicad_pcb` | native KiCad 7 board — open it, DRC it, edit it |
| `cn2-interceptor.kicad_sch` | native KiCad 7 schematic — nets joined by labels |
| `cn2-interceptor.sch` / `.brd` | Eagle 9 XML, for Eagle / Fusion 360 |
| `gerbers-kicad/` | **exported by KiCad itself** — this is the set to send to a fab |
| `gerbers/` | my own writer's output, kept as a cross-check |
| `gerbers-panel/` | **the 5 × 3 panel** — upload this zip for a 15-up order |
| `drc-report.txt` | KiCad's DRC output from the last run |

Regenerate everything:

```bash
python3 gen_pcb.py      # geometry DRC + connectivity + my gerbers + preview.svg
python3 gen_kicad.py    # .kicad_pcb + KiCad's own DRC report
python3 gen_kicad_sch.py  # .kicad_sch
python3 gen_eagle.py    # .sch + .brd   (refuses to run if the DRC fails)

kicad-cli pcb export gerbers --output gerbers-kicad \
  --layers F.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,Edge.Cuts \
  --no-x2 --subtract-soldermask cn2-interceptor.kicad_pcb
```

There is one source of truth. The schematic, the board and the Gerbers are three
renderings of the same Python model, so they cannot drift apart.

## What has been checked, and what has not

**Checked by KiCad itself** (`gen_kicad.py`, via `pcbnew.WriteDRCReport`):

```
violations       0   (excluding 4 lib_footprint_issues, see below)
unconnected pads 0
footprint errors 0
```

The four remaining reports are all `lib_footprint_issues: the current
configuration does not include the library 'cn2interceptor'` — the footprints are
generated into the board file rather than pulled from an installed library, so
KiCad cannot find a library to check them against. That is a bookkeeping
complaint, not a geometry or connectivity one.

**Checked by `gen_pcb.py`** on every run, independently of KiCad:

- 2186 pairwise clearance checks at 0.20 mm — copper-to-copper on each layer,
  and every track against every through-hole pad on both layers
- annular ring ≥ 0.30 mm, drill ≥ 0.30 mm, 0.5 mm edge keepout
- connectivity: every pad is on a net, every net has ≥ 2 pads, and every net
  forms a single connected island rather than two stranded halves
- both Eagle files parse as well-formed XML
- the board's pin map is compared against `firmware/include/config.h` and the
  run fails on a mismatch
- module bodies (not just their pads) are checked against every connector pad

**Not checked.** Be clear-eyed about this list:

- **Nothing has been fabricated.** No board has been made or assembled.
- **The Eagle `.sch`/`.brd` have not been opened in Eagle or Fusion.** They parse
  as well-formed XML and are generated from the same verified model, but no Eagle
  install has confirmed they import cleanly. The KiCad board has been through
  KiCad's own DRC and Gerber export.
- **No ERC has been run.** KiCad 7's CLI has no `sch erc` command (that arrived
  in KiCad 8). The `.kicad_sch` parses and plots cleanly through `kicad-cli sch
  export`, which is not the same as passing ERC — open it in Eeschema and run
  ERC if you want that confirmed.
- **Footprint dimensions come from datasheets, not from measuring your parts.**
  The XIAO's 15.24 mm row spacing is well documented; the level shifter's is not
  — see the warning above.
- **No thermal or current analysis.** It carries logic signals and the panel's
  own 5 V supply, which the OEM cable already carried, so this is unlikely to
  matter. It has not been calculated.

**Before ordering:** open the Gerbers in a viewer (JLCPCB and PCBWay both show
you the rendered board at upload), check the level shifter footprint against your
actual module, and confirm the JST-XH pin order matches your cable.

## Safety

The board carries the panel's 5 V supply and its data lines. It does not touch
mains. But what you can then *do* with it does —
see [`docs/safety.md`](../../docs/safety.md): byte 1 of the panel frame is a
direct load bitmap, and three of its bits fire heaters with no cycle running and
no water in the machine.
