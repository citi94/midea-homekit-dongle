# Senville/Midea mini-split → native HomeKit dongle

An ESP32 that plugs into the mini-split's "USB" service port and shows up in
the iOS Home app as a HeaterCooler accessory. No cloud, no Home Assistant, no
bridge — [HomeSpan](https://github.com/HomeSpan/HomeSpan) speaks the HomeKit
Accessory Protocol directly, [MideaUART](https://github.com/dudanov/MideaUART)
speaks the AC's serial protocol.

Along the way the dongle's flight recorder produced a fairly complete
reverse-engineering of the unit's control policy — the frequency grades,
the outdoor-temperature feedforward, the economy stand-down state that
quietly ignores the turbo button, and the documented service-mode escape
hatches. **See [FINDINGS.md](FINDINGS.md).**

## How the port works

The USB-A socket on the indoor unit is not USB. It's a 5V TTL UART, 9600 8N1:

| USB pin | Function |
|---|---|
| 1 (VBUS) | 5V supply (can power the ESP32) |
| 2 (D−) | AC TX |
| 3 (D+) | AC RX |
| 4 (GND) | GND |

TX/RX labeling varies between sources — if the handshake never appears in the
logs, swap them. It's harmless.

## Wiring

Target board: **ESP32-S3** dev board (S3-N16R8 module, dual USB-C, CH343P
USB-serial chip).

The AC side is **5V logic**; the ESP32-S3 is 3.3V. Use a bidirectional level
shifter (BSS138-style module) on both data lines. The board's own silicon
doesn't help here: the AMS1117 is the 3.3V regulator, the two J3Y (S8050)
transistors are the CH343's auto-reset circuit, and the S4 parts are power
path management for the two USB-C ports — everything on the board runs 3.3V
logic and none of it is wired to spare GPIOs.

```
AC USB port           level shifter          ESP32-S3 dev board
  1  5V  ──────────┬──── HV                ───── 5Vin
  4  GND ──────────┼──── GND ──────────────┬─── GND
  2  D− (AC TX) ───┼──── HV1 ↔ LV1 ────────┼─── GPIO18 (RX)
  3  D+ (AC RX) ───┴──── HV2 ↔ LV2 ────────┴─── GPIO17 (TX)
                         LV ── 3V3 pin
```

> **⚠️ Never have the AC's 5V (VBUS) and a USB-C port connected at the same
> time.** The two supplies will fight, or the AC's 5V will backfeed your
> computer. During bring-up, leave the VBUS wire disconnected and power the
> board from USB; only connect VBUS once you're done with the cable.

Notes:

- GPIO17/18 are chosen because they're free of S3 landmines: GPIO0/3/45/46
  are strapping pins, 19/20 are the native-USB port, and 26–37 belong to
  flash/PSRAM on the N16R8 module.
- For flashing/monitoring use the USB-C port that enumerates as a **CH343**
  serial device (the other port is the S3's native USB, unused here).
- HomeSpan status is shown on the onboard WS2812 RGB pixel (GPIO48; some
  clone boards route it to GPIO38 — change `STATUS_PIXEL_PIN` if it stays
  dark). The OTA env builds with `-DSTATUS_PIXEL_PIN=-1` (pixel off): once
  the dongle is inside the AC nobody can see it, and HomeSpan 2.1.8's
  1 KB blink task can overflow its stack on a status change and reboot
  the board (found via a core dump — see `/coredump` in `src/main.cpp`).
- Optional but recommended: a 470–1000 µF electrolytic across 5Vin/GND near
  the board — WiFi transmit bursts through a long thin cable can brown out
  the ESP32.

## Build & flash

```sh
pio run -t upload        # first flash over USB
pio device monitor       # HomeSpan CLI + Midea protocol logs, 115200
```

After the first flash, OTA is enabled: `pio run -e ota -t upload` (or
`-e s3zero-ota` for the S3-Zero board — the OTA envs carry the device IP
and auth flag). The OTA password defaults to `homespan-ota` — **change it**
(type `O` in the serial CLI) before the dongle goes live: anyone on your
WiFi who knows the default could flash their own firmware onto your HVAC.
The OTA envs read the password from `secrets.ini` (gitignored): copy
`secrets.example.ini` to `secrets.ini` and fill in yours.

## First boot

1. **WiFi**: with no stored credentials the ESP32 opens a *HomeSpan-Setup*
   access point (password `homespan`) — connect to it and enter your WiFi
   details. Or type `W` in the serial monitor and enter them there.
2. **Pair**: iOS Home app → Add Accessory → "More options…" → Mini-Split.
   Setup code: **4663-7726**.
3. **Plug into the AC**: watch the serial monitor (or `http://<ip>/status`) —
   you should see the protocol handshake and an autoconf capabilities dump
   within seconds. If not, swap the D−/D+ wires.

## What's exposed in HomeKit

- Power, mode (Auto / Heat / Cool), target temperature (17–30 °C, whole
  degrees — the unit's autoconf reports no half-degree support)
- Current room temperature (from the indoor unit's sensor)
- **Outdoor temperature** as a separate sensor (the outdoor coil sensor)
- Fan speed slider: 20 = auto, 40/60/80/100 = low/medium/high/turbo
  (dragging to 0 turns the AC off — that's iOS behavior, not a bug)
- Oscillate switch = both louvers (vertical + horizontal sweep)
- **Dry Mode switch**: on = dehumidify, off = back to Cool
- **Eco and Turbo switches** for the presets (the AC enforces its own rules —
  eco only works in Cool, turbo in Cool/Heat; a disallowed toggle snaps back)
- Heating/cooling activity indicator (the tile turns orange/blue when working)

Reliability details: commands are held until the AC's status confirms them
and re-sent up to 3 times, and while a command is unconfirmed, stale status
polls are not synced back — so quick successive changes in the Home app
don't get silently dropped or visually reverted.

Quirks and non-features, by design of HomeKit's HeaterCooler service:

- **Dry and fan-only modes** have no HomeKit representation. The remote still
  works for those; the accessory reports "idle" while in them, and changing
  fan/swing from the Home app won't kick the AC out of them (only an explicit
  power or mode change will).
- **Silent fan mode** is remote-only (it displays as "low" in HomeKit).
- In Auto mode the AC forces its own fan-auto, so the fan slider is inert,
  and HomeKit's heat/cool band is collapsed to its midpoint, since Midea
  takes a single setpoint.
- Commands beep the indoor unit like the OEM dongle (set
  `AC_BEEP_ON_COMMAND = false` in `src/main.cpp` to silence).

## Network

The dongle pins itself to a static IP in firmware (`WiFi.config` in
`src/main.cpp`) because the Starlink router offers no DHCP reservations:

- **`192.168.2.10`** — keep this in sync with `upload_port` in
  `platformio.ini` if it ever changes.
- The OTA password lives in `secrets.ini` (gitignored — see
  `secrets.example.ini`).

## Status dashboard

`http://192.168.2.10:8080/` — live dashboard served by the dongle: indoor /
outdoor / target temperatures, mode/fan/swing/preset badges, 4-hour history
charts (temperatures + compressor frequency) with hover readout, WiFi signal,
heap, and uptime. Raw JSON at `/api` (polled by the page every 3 s; handy for
scripting too).

Deep telemetry: the firmware polls the Midea "C1 group" engineering queries
(the data Midea's own service tools read; this unit answers all four groups):

- compressor frequency (actual + target), current, voltage
- coil temperatures (indoor evaporator T2, outdoor condenser T3), outdoor
  ambient T4, compressor discharge Tp
- indoor/outdoor fan RPM, defrost state, outdoor unit input power
- fault codes from the status frame, shown as a red banner with a decoded
  description (plus a filter-clean reminder banner)

Groups that a unit doesn't support are probed 3× at boot and then retired
(shown as `g1/g2/g5/g7` ticks in the dashboard's System row). MideaUART is
vendored in `lib/MideaUART` with small patches for the fault/panel fields —
see `lib/MideaUART/PATCHES.md`.

### Flight recorder

The firmware keeps a 3-day, 1-minute-resolution log of everything above in
RAM (survives WiFi drops, not reboots), plus an event ring of every state
transition:

- `http://192.168.2.10:8080/log.csv` — 4320 samples × (temps, compressor
  Hz, fan RPM, volts, mode/fan/preset, power/defrost/fault flags), ISO8601
  UTC timestamps, streamed as CSV.
- `http://192.168.2.10:8080/events` — the last 256 state transitions
  (power, mode, fan, preset, target, defrost, aux heat, faults, link)
  with timestamps.

This is the instrumentation behind [FINDINGS.md](FINDINGS.md).

### Absent drying mode

For drying out a wet room while nobody is in (the dashboard has a card for
it; `curl` form below). It alternates *heat 30 °C* (warms the room's fabric
so it releases water into the air) and *cool 17 °C* (condenses that water
out on a cold coil at the top compressor grade), both on medium fan, and
can hand over to *auto 20 °C* at a set time so the room is normal when you
arrive. Dry mode alone does worse on this unit — it's a fixed 31 Hz and
chills the room, see FINDINGS.md — and a long heat soak in a damp room is
mould weather.

```sh
curl 'http://192.168.2.10:8080/awaydry?on=1&heat=60&cool=60&first=cool&end=1788937200'
curl 'http://192.168.2.10:8080/awaydry?on=0'      # stop, AC left as is
```

Minutes per phase; `first` = `heat` (default) or `cool`; `end` = unix epoch.
It survives a dongle reboot (NVS) and switches itself off if someone
changes mode or setpoint from the remote or HomeKit mid-run (logged as
`awayDry → yielded` in `/events`).

### Guardian (standing protections)

Three rules that run on the dongle itself, independent of HomeKit. They
only act when the unit is in standby (off, no absent-drying run, no
HomeKit command in flight), so they never fight a person, and they release
the unit back to off when done. Dashboard card with toggles and thresholds;
every firing is logged in `/events` as `guard`.

- **Freeze**: indoor below 5 °C → heat 17 °C for an hour.
- **Overheat**: indoor above 37 °C → cool 25 °C for an hour.
- **Dew** (the rust one): after days of cold the machines and floor sit at
  the old temperature; when a thaw arrives with a dew point above that,
  every surface sweats. Dew forms on the fabric, not the air, so the trigger
  compares the forecast against a 36-hour average of the room temperature
  (the proxy for slab and machine temperature), which an ordinary cool
  morning doesn't move. The unit has no humidity sensor, so the dongle polls
  [Open-Meteo](https://open-meteo.com/) hourly (plain HTTP, no key) for the
  dew-point forecast at the workshop's coordinates and keeps the fabric
  above *highest dew point in the next 36 h + margin*. The unit's lowest
  setpoint is 17, so the dongle acts as the thermostat: heat 17 in bursts
  until the fabric estimate has climbed past target. A measured indoor
  humidity can be pushed to sharpen it (indoor dew point is then used too).

```sh
curl 'http://192.168.2.10:8080/guard'                         # status JSON
curl 'http://192.168.2.10:8080/guard?dew=1&dewMargin=2&lat=51.23&lon=1.39'
curl 'http://192.168.2.10:8080/guard?stop=1'                  # cancel a run
curl 'http://192.168.2.10:8080/hum?rh=55'                     # push indoor RH
```

## Debugging

- `http://192.168.2.10/status` — rolling HomeSpan web log: AC state changes,
  MideaUART warnings/errors, boot events. First stop when something misbehaves.
- `pio device monitor` — HomeSpan CLI + protocol logs, only when the board is
  on USB (use the native USB-C port; the CH343 port's auto-reset is broken on
  this specimen).

## Credits

- [HomeSpan](https://github.com/HomeSpan/HomeSpan) by Gregg Berman — the
  HomeKit Accessory Protocol implementation this rides on.
- [MideaUART](https://github.com/dudanov/MideaUART) by Sergey Dudanov (MIT)
  — the Midea appliance protocol library, vendored in `lib/MideaUART` with
  local patches ([PATCHES.md](lib/MideaUART/PATCHES.md)).
- [midea-msmart](https://github.com/mill1000/midea-msmart),
  [node-mideahvac](https://github.com/reneklootwijk/node-mideahvac) and
  [midea-local](https://github.com/rokam/midea-local) — protocol
  references used to cross-check the 0xC0/C1 field offsets.
- [GreatScott!'s AC hack video](https://www.youtube.com/watch?v=aKSIhqiKjm4)
  — independent hardware-side confirmation of the T4 feedforward behaviour
  documented in [FINDINGS.md](FINDINGS.md).

Firmware (`src/main.cpp`, the dashboard, the flight recorder) written for
this project. MIT licensed — see [LICENSE](LICENSE).
