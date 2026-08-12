# CN2 interceptor carrier board

A 31.5 × 26 mm, 2-layer carrier that sits in the CN2 cable between the D8
controller and its front panel. CN2 pins 3 and 4 (GND, +5 V) pass straight
through; pins 1 and 2 are routed through a level shifter to a XIAO ESP32-C3,
which then sees and can rewrite every byte in both directions.

<p align="center">
  <img src="preview-kicad.svg" width="820" alt="Board: red = top copper, blue = bottom copper. J2 (panel) left, J1 (controller) right, level shifter nested between the XIAO's pin rows.">
</p>

Left to right: **J2 (panel) · XIAO left row · level shifter · XIAO right row ·
J1 (controller)**. U2 sits *inside* the XIAO's 15.24 mm row gap — its own rows
are 10.16 mm apart, so it drops straight into the middle.

## ⚠️ Mount U2 on the underside

Packing the shifter inside the XIAO's rows puts the two modules on top of each
other in plan view:

```
XIAO module   17.5 × 21.0 mm   x  9.37 .. 26.87    y  2.50 .. 23.50
shifter       15.7 × 13.2 mm   x 10.27 .. 25.97    y  6.40 .. 19.60
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
| J1 | JST-XH 4-pin, 2.54 mm, vertical | cable to the **controller** CN2 |
| J2 | JST-XH 4-pin, 2.54 mm, vertical | cable to the **panel** CN2 |
| U1 | Seeed XIAO ESP32-C3 | 2×7, 2.54 mm pitch, 15.24 mm between rows |
| U2 | 4-channel BSS138 level shifter | 2×6, 2.54 mm pitch |
| J3 | JST-XH 3-pin, 2.54 mm, vertical | flow **meter** side — see below |
| J4 | JST-XH 3-pin, 2.54 mm, vertical | flow **controller** side |

Plus two 1×7 and two 1×6 header strips if you want the modules socketed rather
than soldered down. Socketing the XIAO is worth it — it keeps `D5`, `D10` and
`D7` reachable for the flow and switch simulation, which is why the board carries
no separate header for them.

## J3 / J4 — flow-meter relay

The FV cable is cut and **both ends land on the board**, exactly like CN2: J3
takes the meter, J4 carries on to the controller, and the ESP32 sits in between.
It can pass the real pulses straight through, rewrite their rate, or synthesise
flow entirely on a dry bench.

| Pin | J3 (meter) | J4 (controller) | XIAO |
|---|---|---|---|
| 1 | `Vcc` | `Vcc` | — passes straight through |
| 2 | `GND` | `GND` | board ground |
| 3 | pulses out | pulses in | `D10 / GPIO10` ← in, `D7 / GPIO20` → out |

Vcc and GND pass through J3↔J4 untouched, the same way CN2 pins 3 and 4 do. Only
the signal line is broken.

### ⚠️ Measure FV Vcc before you connect this

**The ESP32-C3 is not 5 V tolerant, and this board does not level-shift the flow
signal** — the converter's four channels are all spoken for by CN2.

`docs/hardware.md` records the meter as having a 10 kΩ internal pull-up, and
warns that running it at 5 V "will put 5 V on a non-5V-tolerant GPIO". If FV Vcc
measures 5 V on your machine, **`GPIO10` needs protection before J3 is plugged
in.** A 10 k / 20 k divider on J3.3 is enough, or feed J3.1 from the board's 3V3
instead of passing the controller's Vcc through, which drops the whole signal to
3.3 V.

There is a second consequence of cutting the line: **the controller's FV input
loses the meter's pull-up**, because that pull-up is inside the meter and now
sits on the far side of the break. `FLOW_OUT` therefore has to drive the line
actively, or J4.3 needs its own pull-up to FV Vcc. The firmware currently drives
`PIN_FLOW_SIM` open-drain, which assumed a pull-up that is no longer there.

Neither is designed in yet. Measure FV Vcc first; the answer decides both.

### Why those two pins, and what it cost

The XIAO has only three GPIOs free that are safe to use: `GPIO7` (D5), `GPIO10`
(D10) and `GPIO20` (D7). `GPIO2`, `GPIO8` and `GPIO9` are strapping pins and
`GPIO21` emits ROM boot chatter at every reset.

Both flow signals must come off the **same side** of the XIAO to be routable on
two layers, and only `GPIO10` and `GPIO20` are on the right-hand column. That
leaves `GPIO7` for one switch simulator, so **the lid switch (SW2) keeps it and
SW1 loses its pin** — SW1 is the unidentified 3-pin switch, so it is the one
worth giving up. It stays reachable on the XIAO's own header pin if you socket
the module.

`firmware/include/config.h` matches this board: `PIN_FLOW_SIM` moved from
`GPIO7` to `GPIO20`, `PIN_SW2_SIM` from `GPIO10` to `GPIO7`, `PIN_FLOW_IN` is new
on `GPIO10`, and the SW1 simulator is gone — `/api/sim` accepts `sw=2` only.

## Connections

| ch | CN2 | | Converter | XIAO | UART |
|---|---|---|---|---|---|
| 1 | J1 CONTROLLER pin 1 — TX, output | → | `HV1` · `LV1` | `D1 / GPIO3` | `Serial0` **RX** |
| 2 | J1 CONTROLLER pin 2 — RX, input | ← | `HV2` · `LV2` | `D2 / GPIO4` | `Serial0` **TX** |
| 3 | J2 PANEL pin 1 — RX, input | ← | `HV3` · `LV3` | `D3 / GPIO5` | `Serial1` **TX** |
| 4 | J2 PANEL pin 2 — TX, output | → | `HV4` · `LV4` | `D4 / GPIO6` | `Serial1` **RX** |

Identical to [`docs/wiring.md`](../../docs/build.md#4-go-inline) and to
`firmware/include/config.h`, so a board built from this needs no firmware change.

Power: controller pin 4 = panel pin 4 = converter `HV` = XIAO `5V`, one net.
XIAO `3V3` → converter `LV`. All grounds common.

## ⚠️ Measure the level shifter before ordering

**Row spacing on 4-channel BSS138 modules is not standardised.** SparkFun's
BOB-12009 and most clones use 0.4″ (10.16 mm), but 0.5″ (12.7 mm) modules exist
and look identical in photos. Put calipers across yours, centre of one row to
centre of the other, then set `LS_ROW` at the top of `gen_pcb.py` and regenerate.

This is the single most likely way to get an unusable board back from the fab.

## How small it can go

31.5 × 26 mm is 819 mm², **45 % smaller than the 50 × 30 it started at** — and
that is with two more connectors than the first version had.

The last reduction came from pulling J1 and J2 in against the block and noticing
that pin 4 on both connectors is the *lowest* pin, so the +5V ring can come up
each connector's own column from underneath instead of needing a side lane at
either end. That removed two lanes outright. The width is fully accounted for:

```
  0.5  edge
  0.9  GND ring lane
  1.8  J2 pads
  1.4  ch4 and ch3 lanes
 16.8  XIAO + shifter block  (15.24 row pitch + 1.6 pad)
  2.1  GND, ch2 and ch1 lanes
  1.8  J1 pads
  0.7  flow-in lane
  1.8  J3 / J4 pads
  0.7  FV Vcc lane
  0.9  GND ring lane
  0.5  edge
  3.6  clearances between the above
 ─────
 31.5
```

Nothing further comes off without changing the design: the block is fixed by the
XIAO, and every lane carries a net that has to cross the board. Real reductions
would need **4 layers** (lets the rings run under everything, maybe 28 × 24), or
**both connectors on one edge** (one set of lanes instead of two, maybe 30 × 24),
or **discrete BSS138s instead of the module**, which removes 12 through-holes and
the whole nesting problem.

## Files

| | |
|---|---|
| `gen_pcb.py` | the board as explicit geometry — placement, nets, routing, its own DRC, preview |
| `gen_kicad.py` | builds a native `.kicad_pcb` via the pcbnew API **and runs KiCad's own DRC** |
| `gen_eagle.py` | emits `.sch` and `.brd` from the same model |
| `gen_kicad_sch.py` | emits a native `.kicad_sch` from the same model |
| `cn2-interceptor.kicad_pcb` | native KiCad 7 board — open it, DRC it, edit it |
| `cn2-interceptor.kicad_sch` | native KiCad 7 schematic — nets joined by labels |
| `cn2-interceptor.sch` / `.brd` | Eagle 9 XML, for Eagle / Fusion 360 |
| `gerbers-kicad/` | **exported by KiCad itself** — this is the set to send to a fab |
| `gerbers/` | my own writer's output, kept as a cross-check |
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

- 2834 pairwise clearance checks at 0.20 mm — copper-to-copper on each layer,
  and every track against every through-hole pad on both layers
- annular ring ≥ 0.30 mm, drill ≥ 0.30 mm, 0.5 mm edge keepout
- connectivity: every pad is on a net, every net has ≥ 2 pads, and every net
  forms a single connected island rather than two stranded halves
- both Eagle files parse as well-formed XML

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
