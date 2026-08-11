# Smart Baby Washer

Make a dumb baby-bottle washer app-controlled — without replacing its brain.

A **Seeed XIAO ESP32-C3** sits in the middle of the ribbon between the front panel
and the controller board, forwards every byte, and can rewrite any of them in
flight. The machine keeps working exactly as it did. You gain a web app, a Home
Assistant integration, and a cycle runner you can program.

⚠️ **All of this was developed on exactly one machine: a Momcozy D8 DeepClean
(internal model `BW05`, controller board `BBW04001-UL-P`).** Nothing here has
been tested on any other washer, including other Momcozy models. It should work
on a machine carrying the same controller board, and the *method* generalises to
any panel-link washer — but the pin map, the bit meanings and the cycle programs
are this board's, and none of them are guaranteed to carry over.

<p align="center">
  <img src="docs/diagrams/architecture.svg" width="880"
       alt="The ESP32-C3 sits on the CN2 UART between the main board and the front panel, and bridges to Home Assistant over WiFi.">
</p>

> ⚠️ **This drives 120 V heaters and a water pump directly.** The machine's own
> controller will not stop you. Read [`docs/safety.md`](docs/safety.md) before you
> open anything.

## What you get

<p align="center">
  <img src="docs/images/webui/app-phone.png" width="300"
       alt="The app on a phone: state and water temperature, a grid of wash programs with one selected, a START button and the stage list.">
</p>

<p align="center">
  <img src="docs/images/webui/overview.png" width="880"
       alt="The engineering web page: machine state, wash pump relay, decoded frames, cycle runner, error codes, lid status, and temperature and flow graphs.">
</p>

- **A web app** on the ESP32 — pick a program, start it, watch temperature and
  progress. Mobile-sized. No cloud, no account.
- **Home Assistant** — entities, commands and a dashboard over plain HTTP. No
  MQTT broker, no custom component.
- **A programmable cycle runner** — six built-in programs plus six of your own,
  with per-stage durations you can edit. It carries interlocks the machine does
  not: no heater without a verified fill, and pause-not-abort on lid open or no
  water.
- **Full visibility** — live decode of both directions of the panel link,
  temperature and flow graphs, and the machine's real error codes with plain
  English next to them.

<p align="center">
  <img src="docs/diagrams/cycle-timeline.svg" width="880"
       alt="Timelines of a wash cycle and a self-clean cycle: temperature curve above six load lanes, every fill marked with its commanded target and the flow count delivered.">
</p>

## What you build

Four wires cut into the panel link, through a level shifter, into the ESP32.
Power and ground stay connected straight through, so the panel never loses its
supply.

⚠️ Note the shifter's real pad order — with **HV facing the machine, the channels
run 4·3·2·1 down the board**, not 1·2·3·4. Wire to the silkscreen, not to where
you expect the numbers to be.

<p align="center">
  <img src="docs/diagrams/wiring-as-built.svg" width="1000"
       alt="As-built wiring: the controller and front panel connected through all four channels of a BSS138 level converter to a XIAO ESP32-C3, with power and ground passing straight through.">
</p>

<p align="center">
  <img src="docs/images/diy-module.jpg" width="880"
       alt="The finished module inside the machine: a XIAO ESP32-C3 on a scrap of perfboard with a 4-pin JST lead to the panel link and an external antenna, tucked beside the pump and wiring loom.">
</p>

A scrap of perfboard is enough — that is a XIAO with a 4-pin JST lead to the
panel link, living beside the pump. The optional carrier PCB in
[`hardware/pcb/`](hardware/pcb/) is tidier, not better.

💡 **Fit the external antenna.** The XIAO has a u.FL connector, and inside a metal
and wet plastic appliance the on-board antenna struggles. WiFi dropping out is
also OTA dropping out.

Full procedure — including the passive detect step that confirms which pin is
which **before** anything is cut — in [**build.md**](docs/build.md).

## Getting there

1. ⚠️ [**safety.md**](docs/safety.md) — what the machine does *not* protect you
   from. Not optional reading.
2. [**build.md**](docs/build.md) — parts, where to tap, level shifting, wiring.
3. [**flash.md**](docs/flash.md) — build the firmware, get it on the board, keep
   OTA working once it is sealed inside the machine.
4. [**use.md**](docs/use.md) — the web app, the cycle runner, Home Assistant.

Reference: [**board.md**](docs/board.md) — what is inside the machine ·
[**protocol.md**](docs/protocol.md) ·
[**cycles.md**](docs/cycles.md) ·
[**api.md**](docs/api.md) ·
[**troubleshooting.md**](docs/troubleshooting.md) ·
[**hardware/pcb/**](hardware/pcb/) (optional carrier board)

## Quick start

```bash
cp firmware/include/secrets.h.example firmware/include/secrets.h   # WiFi + OTA password
cd firmware
pio run -e c3 -t upload
```

Open `http://baby-washer.local/`.

**Start in LISTEN mode** — it physically cannot drive a line, so nothing you do
can hurt the machine while you confirm the wiring.

## Does it fit my machine?

**Honest answer: only a Momcozy D8 is known to work.** One machine, one board.

The closer yours is, the better your odds:

| | |
|---|---|
| **Same controller board** — `BBW04001-UL-P` | should work as-is. Check the silkscreen before you buy anything |
| Another Momcozy, different board | the firmware will likely decode the link, but expect different bit meanings. Treat every load bit as unknown until you have watched it act |
| Any other washer with a separate panel | the *approach* applies — tap the panel link, decode it, forward it. Everything past that is your own work |

What you need at minimum is a washer where a **separate front panel** talks to a
**controller board** over a few wires — look for a 4-pin connector carrying
ground, +5 V and two signal lines.

[`docs/protocol.md`](docs/protocol.md) describes the frame format so you can tell
quickly whether yours is the same protocol, and `POST /api/detect` finds the pin
map for you without ever driving a line. **Start in LISTEN mode and confirm what
each bit does on your own machine before trusting any of it.**

A capture from a machine that is not a D8 is genuinely useful — open an issue with
the frame dump from `/api/frames` and the board part number.

## Licence

MIT — see [`LICENSE`](LICENSE). No affiliation with Momcozy. Names and part
numbers appear for identification only.
