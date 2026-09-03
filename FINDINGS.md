# Field notes: what a Midea mini-split is actually thinking

Three days of minute-resolution telemetry from inside a Senville (Midea OEM)
wall-mounted R32 mini-split, logged by the dongle in this repo over the
unit's own service UART. The trigger was the classic owner's complaint:
*"it's set to 17 °C, it's been on all day, and it isn't really trying."*
It turned out nothing is broken. The unit runs exactly the control policy
it was designed to run — it just never tells you what that policy is.

Everything below is reconstructed from the unit's own engineering telemetry
(the "C1 group" queries Midea's service tools use): compressor frequency,
inverter voltage, coil temperatures (indoor T2 / outdoor T3), outdoor
ambient T4, fan RPM, fault codes — sampled every minute into a 3-day ring
(`/log.csv`) with every state transition captured in an event log
(`/events`). One unit, one hot fortnight, n=1; hypotheses are labelled.

## 1. The compressor runs in discrete grades

Observed frequencies over three days: **20, 31, 40, 47, 61 Hz** — never
anything in between for more than a transition. Midea's own service manuals
describe the control loop as moving frequency "up or down a grade" through
zones based on ΔT (room minus setpoint), so these are the grades for this
capacity class.

## 2. Outdoor temperature (T4) sets the ceiling, feedforward

The grade the unit is *allowed* to reach tracks outdoor ambient, not demand:

| outdoor T4 | highest grade observed |
|---|---|
| ~22–24 °C | 31–40 Hz |
| ~26–27 °C | 47 Hz |
| ~30 °C | 61 Hz (never seen before that day) |

Setpoint 17 °C with a warm room does **not** unlock the higher grades on a
mild day. The firmware uses T4 as a feedforward predictor of wall heat
flux: hot outside → heat will keep coming through the envelope → spend
compressor Hz; mild outside → don't. This is the mechanism GreatScott!
exploited from the hardware side in
["Apparently You Can Hack an Air Conditioner! (+30% Power)"](https://www.youtube.com/watch?v=aKSIhqiKjm4)
by strapping a small closed-loop heater to the T4 probe — his +30% is the
gap between the mild-day grade cap and the hot-day one. Our telemetry and
his heating pad agree perfectly.

## 3. The economy stand-down state ("the nap")

After roughly 1¾–2½ hours of hard running, or when progress toward the
setpoint stalls, the unit drops to a **20 Hz hold** and stays there —
indoor comfortable-ish (23–25 °C), setpoint 17, compressor idling along.
While in this state:

- **Turbo is accepted but vetoed.** The preset sets (the protocol confirms
  it, the fan goes to turbo speed) but the compressor stays at 20 Hz. The
  button beeps; nothing happens where it matters.
- **A mains power-cycle clears it.** A fresh boot sprints at the full
  T4-allowed grade (47 Hz on a 26 °C day) with no turbo needed. Off/on is
  the real turbo button.
- **It also exits on its own when it starts losing.** On the 30 °C day the
  unit spontaneously re-engaged (20 → 61 Hz) as indoor temperature drifted
  up through ~25 °C. So it's not a pure timer: the model that fits the
  event log is *progress-based* — "setpoint unreachable in reasonable
  time" quietly becomes "hold what we have efficiently", with a wake-up
  when the hold starts failing.

## 4. The effective setpoint band is 22–25 °C

With the setpoint pinned at 17 °C for days, indoor temperature lived in a
22–25 °C band. Low setpoints are treated as "maximum effort within
policy", not as a target the unit intends to reach. (To be fair to the
firmware: 22 °C with a fan and the low humidity a working evaporator
produces *feels* close to what people imagine 17 °C feels like.)

## 5. Metronomic 40 Hz blips every ~31 minutes

During every long 20 Hz hold, the compressor steps to 40 Hz for 1–2
minutes on an almost perfect ~31-minute period, then drops back. Our
working hypothesis is **oil return** — a periodic velocity burst to sweep
oil back to the compressor, which low-frequency operation strands in the
lines. Side effect observed directly: the suction-side surge briefly
chills the outdoor unit's compressor area enough to shed a gush of
condensate from the coil area — alarming to watch, entirely benign, and
**not** a reverse-cycle defrost (the defrost flag never set and T3 never
went below ambient; the event log rules it out).

## 6. Turbo is a 30-minute suggestion

When turbo *is* honoured (outside the stand-down state), it self-cancels
after ~30 minutes and takes over fan control while active (reports
fan=auto). This matches Midea's service-manual description of the turbo
function to the minute.

## 7. Nothing was wrong with the machine

Across the whole log: zero fault codes, indoor coil (T2) riding safely
above freezing with healthy superheat behaviour, steady condensate
production, textbook coil temperature spreads. Every "it's not trying"
symptom was policy, not a fault and not low charge.

## Why the policy exists

Seasonal efficiency ratings (SEER / EN 14825) are computed overwhelmingly
at **part load** — the test points are 100/74/47/21 % of rated capacity,
weighted toward mild days. Firmware that loafs at 20 Hz whenever it
believes it can get away with it is firmware that wins the label war. The
protection layers underneath (T2 anti-freeze, T3/discharge limits, current
limits) are real engineering; the grade tables above them are, in effect,
marketing-shaped comfort policy. Both of these things are true at once.

## The documented escape hatches (no hardware required)

Midea's own service manuals ship the override, they just don't tell owners:

- **Rating capacity test mode** (older generation,
  [MSV1 service manual](http://www.columbus-klima.hu/write/upload/szerviz/MD_SM_MSV1_09_12HRDN1_QC2A.pdf)
  §9.5.4): cooling mode, 17 °C, high fan, then press **TURBO 6 times
  within 10 seconds** on the remote. Buzzer sounds ~2 s, compressor locks
  to the rated frequency for up to 5 hours (exits on any fan/setpoint
  change). All protections stay active.
- **Inquiry mode** (current generation, e.g.
  [Midea/Carrier DLFSHCH service manual](https://www.mideacomfort.us/downloads/SINGLE-ZONE%20U-match/2%20TON%20AND%20BELOW/Service%20Manual/WALL%20MOUNTED/INFINI%20Hyper%20Heat%20Series%20(SEER%2025%EF%BC%8CEnergy%20Star)/DLFSHCH/Service%20Manual-DLFSHCH.pdf)):
  hold **On/Off + Fan for 8 s** on the remote → codes 0–30 shown on the
  indoor display, including T1–T4, discharge temp, target/actual
  compressor Hz, amps, volts, EEV opening — and code 14, "**Sn — capacity
  test (special usage)**".
- **Forced cooling** (the touch button under the front panel, press twice
  within 5 s while off): compressor at a fixed mid frequency (F2) for 30
  minutes, then reverts to auto/24 °C. Service/pump-down use.

## What the UART will *not* do: spoof the room temperature

The obvious software-only trick — tell the unit over the service port that
the room is 30 °C so it stops coasting — was tested exhaustively on this
unit (firmware v1.5.0's `/probe` endpoint sends raw frames) and does not
exist:

- The **0x40 control frame** honours exactly the documented fields
  (power, mode, setpoint, fan, timers, swing, eco/turbo/sleep, display,
  freeze-protect). Every other byte (6, 8–23) was written one at a time
  with a temperature-shaped value; the unit accepted each frame, echoed a
  C0 with its *real* T1 reading, and nothing persisted. Byte 22 is echoed
  back in the reply but not stored (a message-ID slot, not a sensor).
- The **0x41 query** ignores its subtype byte entirely — 0x00, 0x01,
  0x11, 0x20, 0x41, 0xA1, 0xC1, 0xE1, 0xFF all return the same C0 as
  0x81. Only 0x21 (C1 engineering groups) and the 0x61 display toggle
  have distinct handlers.
- The **B0/B1 property protocol** (where newer units expose extras such
  as "remote temperature") is absent: no response. Only B5 (capabilities)
  answers.
- The electronic-ID query returns all 0xFF (never programmed).

The reason is structural: *Follow Me* lives in the IR decoder. The remote
sends its own thermistor reading in every IR frame, and the indoor board
substitutes it for T1 while those frames keep arriving. The UART path was
written for cloud dongles, which have no sensor of their own, so no
"remote temperature" field was ever added to the 0x40 command on this
generation. To spoof the room temperature you must either speak IR (a
940 nm LED on a spare GPIO of the dongle would do it) or change what the
T1 thermistor sees — which is GreatScott's resistor trick, just on the
indoor sensor instead of the outdoor one.

## Reproducing this

Flash the dongle (see [README](README.md)), let it run a few days, then:

- `http://<dongle-ip>:8080/log.csv` — 3 days × 1-minute samples, ISO8601
  UTC timestamps.
- `http://<dongle-ip>:8080/events` — every power/mode/fan/preset/target/
  defrost/fault transition with timestamps.

The stand-down state, the grade table, and the 31-minute blips fall out of
a single hot afternoon of data.
