# Local patches vs upstream

Vendored from https://github.com/dudanov/MideaUART commit eeea6c3 (v1.1.9).
Offsets cross-checked against mill1000/midea-msmart (`StateResponse`),
reneklootwijk/node-mideahvac (`parsers/C0.js`) and rokam/midea-local
(`devices/ac/message.py`) — all use the same body indexing as this library
(byte 0 = the 0xC0/0xC1 message-type byte).

1. `include/Appliance/AirConditioner/StatusData.h` — added getters for 0xC0
   fields upstream ignores: `getErrorCode()` (byte 16, 0 = none),
   `getErrorFlag()` (1&0x80), `getFilterAlert()` (13&0x20), `getDisplayOn()`
   (14 bits 4-6 != 7), `getAuxHeat()` (9&0x08), `getPurifier()` (9&0x20).

2. `include/Appliance/AirConditioner/AirConditioner.h` +
   `src/Appliance/AirConditioner/AirConditioner.cpp` — `m_readStatus` now
   tracks errorCode / filterAlert / displayOn / auxHeat as appliance state
   (participates in the state callback), with public getters.

3. Same files — `m_readStatus` stamps `m_statusMs` on EVERY valid status
   response; `getStatusAgeMs()` exposes it. Rationale: the state callback
   only fires on state CHANGES, so "time since last callback" reads as a
   dead link whenever the AC is simply in steady state. Liveness must come
   from responses, not changes.

Note: upstream's `getIndoorHum()` actually reports the humidity *setpoint*
(C0 byte 19 & 0x7F), not measured humidity. Measured indoor humidity comes
from the C1 group-5 query, implemented app-side in `src/main.cpp`
(`AirConditionerEx`), along with the other C1 group telemetry queries
(group 1 compressor/coils, 2 indoor fan, 5 humidity/defrost, 7 outdoor power).
