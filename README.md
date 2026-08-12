<h1 align="center">Smart Baby Washer</h1>

<p align="center">
  <b>Turn a dumb baby-bottle washer into a smart one — without modifying it.</b>
</p>

<p align="center">
  <img alt="License MIT" src="https://img.shields.io/badge/license-MIT-blue">
  <img alt="Platform ESP32-C3" src="https://img.shields.io/badge/platform-ESP32--C3-black">
  <img alt="Home Assistant native" src="https://img.shields.io/badge/Home%20Assistant-native-41BDF5">
  <img alt="No cloud" src="https://img.shields.io/badge/cloud-none-brightgreen">
  <img alt="Reversible install" src="https://img.shields.io/badge/install-reversible-success">
</p>

An **ESP32-C3 sits in the cable** between the washer's front panel and its
controller board. It reads every frame in both directions and can rewrite any of
them, so the machine behaves exactly as it always did — and gains a phone app,
Home Assistant, and cycles you can program yourself.

### ✅ Nothing is cut, nothing is soldered to the machine

The module plugs **inline** on the existing panel connector: controller on one
side, panel on the other, power and ground passing straight through. Unplug it
and the washer is stock again. No OEM firmware is replaced, no relay is spliced,
no warranty sticker is disturbed.

<p align="center">
  <img src="docs/diagrams/architecture.svg" width="880"
       alt="The ESP32-C3 sits on the CN2 UART between the main board and the front panel, and bridges to Home Assistant over WiFi.">
</p>

## What your washer gains

| Stock machine | With this |
|---|---|
| Walk to it, press a button | **Start, stop and resume from your phone** or Home Assistant |
| Six fixed programs | Six built-ins **plus six of your own**, every stage editable |
| A countdown you have to trust | **Live temperature, water flow, stage and lid** — second by second |
| No idea when it finishes | **Notifications** on finish, pause and fault |
| A two-character error code | The code **in plain English**, with what triggers it |
| No history | Temperature and flow **graphs**, and a full cycle log |
| Nothing stops a dry heater | Interlocks the machine lacks — **no heater without a verified fill** |

<p align="center">
  <img src="docs/images/webui/app-phone.png" width="320"
       alt="The app on a phone: state and water temperature, a grid of wash programs with one selected, a START button and the stage list.">
</p>

Everything is **local** — the app is served by the ESP32 itself and Home Assistant
talks to it over plain HTTP. No cloud, no account, no vendor app.

<p align="center">
  <img src="docs/images/webui/overview.png" width="880"
       alt="The engineering web page: machine state, wash pump relay, decoded frames, cycle runner, error codes, lid status, and temperature and flow graphs.">
</p>

The engineering page above is the other half: a live decode of the panel link in
both directions, per-bit load control, fault injection, and a virtual-controller
mode that runs a whole cycle with nothing energised.

> ⚠️ **It can also drive 120 V heaters and a water pump directly, and the
> machine's own controller will not stop you.** Read
> [`docs/safety.md`](docs/safety.md) before you open anything.

⚠️ **Developed on exactly one machine** — a Momcozy D8 DeepClean (`BW05`,
controller board `BBW04001-UL-P`). It should work on any washer carrying the same
board, and the method generalises to any panel-link washer, but the pin map and
bit meanings are this board's. See
[does it fit my machine?](#does-it-fit-my-machine)

<p align="center">
  <img src="docs/diagrams/cycle-timeline.svg" width="880"
       alt="Timelines of a wash cycle and a self-clean cycle: temperature curve above six load lanes, every fill marked with its commanded target and the flow count delivered.">
</p>

## What you build

Four wires cut into the panel link, through a level shifter, into the ESP32.
Power and ground stay connected straight through, so the panel never loses its
supply.

⚠️ Wire to the shifter's **printed pad names**, not to where you expect the
numbers to be — with HV facing the machine the pads read 4·3·2·1 down the board.
The photo inset in the diagram shows it.

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
