# Home Assistant

Two files. No MQTT broker, no custom component, no cloud — HA polls the ESP32's
HTTP API directly.

```
packages/momcozy_d8.yaml    entities, commands, automations
lovelace-dashboard.yaml     the dashboard
```

## Install

1. Copy `packages/momcozy_d8.yaml` into `config/packages/`
2. In `configuration.yaml`:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

3. Set the ESP32's address — it appears **four times** in the package, in the
   `rest:` block and the three `rest_command:` entries
4. Restart HA
5. Settings → Dashboards → new dashboard → ⋮ → **Raw configuration editor** →
   paste `lovelace-dashboard.yaml`

## What you get

| Entity | |
|---|---|
| `sensor.d8_state` | Idle / Running / **Paused** / Complete / Stopped |
| `sensor.d8_water_temperature` | °C, uncalibrated and reads low |
| `sensor.d8_flow_count` | fill counter — cumulative for the **current fill**, zeroed by the controller when it completes |
| `sensor.d8_program` | the program the machine is set to |
| `sensor.d8_stage` | `3/10 — wash + heat` |
| `sensor.d8_error` | `E0`/`E3`/`E4`/`E5`/`E7`, or `none` |
| `binary_sensor.d8_running` | |
| `binary_sensor.d8_paused` | lid open, or the tank needs refilling |
| `binary_sensor.d8_lid` | on = **open** |
| `binary_sensor.d8_fault` | |
| `input_select.d8_program` | two-way — follows the machine if changed elsewhere |
| `script.d8_start_cycle` / `d8_stop_cycle` / `d8_resume_cycle` | |

Plus notifications when a cycle finishes or stops, and when a fault appears.

## ⚠️ What this actually controls

A cycle started from HA does **not** press the machine's buttons. Nothing the
panel does before it commands a load reaches the CN2 link, so there is no button
to press remotely. It runs the **ESP32's own cycle runner**, which drives every
load directly.

That matters because the machine protects almost none of it. Measured, not
assumed: the controller gates nothing except the intake motor, the heaters fire
with no water and the lid open, and **neither end has a fill timeout**. The
runner's interlocks are the only ones in the loop — see
[`../docs/safety.md`](../docs/safety.md) and
[`../docs/cycle.md`](../docs/cycles.md).

So:

- **Starting a cycle is a `script`, not a `switch`**, and every command is a
  `button`-style action. A stuck automation cannot leave a heater latched on.
- `script.d8_start_cycle` **refuses if the lid is open.** The machine itself will
  not.
- There is deliberately no "start at 3 am" example. Do not run this unattended.

## Notes

**Fault codes latch.** Clearing the condition is not enough — the machine needs a
power cycle before it will run again. The dashboard says so when a fault is
showing.

**Polling is 5 s.** The ESP32's own page polls at 1 Hz; 5 s is plenty for a
machine whose stages last minutes, and it keeps HA's recorder small. Lower
`scan_interval` if you want a tighter flow-count trace, but the device-side ring
at `/api/flowlog` is the right tool for per-pulse detail.

**Program indices** in `input_select` must match the firmware's order. If you
reorder the programs in `cn2.cpp`, reorder the list here too.
