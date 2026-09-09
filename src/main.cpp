// Midea/Senville mini-split -> native HomeKit dongle.
//
// The AC's "USB" port is a 5V TTL UART (9600 8N1) on the D-/D+ pins.
// MideaUART speaks the appliance protocol; HomeSpan exposes a HomeKit
// HeaterCooler accessory directly to iOS — no bridge, no cloud.
//
// Wiring (through a bidirectional level shifter, AC side = 5V, ESP = 3.3V):
//   AC USB pin 1 (VBUS) -> 5Vin (powers the board) + shifter HV
//   AC USB pin 2 (D-)   -> AC TX -> shifter -> GPIO18 (ESP RX)
//   AC USB pin 3 (D+)   -> AC RX -> shifter -> GPIO17 (ESP TX)
//   AC USB pin 4 (GND)  -> GND
// NEVER connect the AC's VBUS and a USB-C port at the same time.
// If the AC never responds, swap D-/D+ — labeling varies between writeups.

#include <Arduino.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <time.h>
#include "HomeSpan.h"
#include <Appliance/AirConditioner/AirConditioner.h>
#include "dashboard.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "homespan-ota"  // HomeSpan default; see src/secrets.example.h
#endif

#define FIRMWARE_VERSION "1.7.0"

using namespace dudanov::midea::ac;

// GPIO17/18 are free, unstrapped pins on the ESP32-S3 (avoid 0/3/45/46
// straps, 19/20 native USB, 26-37 flash/PSRAM on N16R8 modules).
// The s3zero env overrides these (see platformio.ini): the BSS138 shifter
// stacks onto the S3-Zero's edge header, landing channels 1/2 on GP2/GP1.
#ifndef AC_RX_PIN
#define AC_RX_PIN 18
#endif
#ifndef AC_TX_PIN
#define AC_TX_PIN 17
#endif
// Onboard WS2812 RGB pixel (GPIO48 on most S3 devkits, 21 on S3-Zero boards)
#ifndef STATUS_PIXEL_PIN
#define STATUS_PIXEL_PIN 48
#endif
static constexpr int PIN_AC_RX = AC_RX_PIN;  // from AC TX
static constexpr int PIN_AC_TX = AC_TX_PIN;  // to AC RX
static constexpr int PIN_STATUS_PIXEL = STATUS_PIXEL_PIN;

static constexpr bool AC_BEEP_ON_COMMAND = true;

// Re-send an unacknowledged command after this long, this many times.
// MideaUART's control() silently drops commands while one is in flight,
// so fire-and-forget from update() is not enough.
static constexpr uint32_t RESEND_INTERVAL_MS = 1500;
static constexpr uint8_t RESEND_ATTEMPTS = 3;

// C1 "group data" query (msmart-ng GetGroupDataCommand format): body
// 41 21 01 (0x40|group) + zero padding. Same mechanism as the library's own
// power query (group 4). Groups: 1 compressor/coils, 2 indoor fan,
// 5 humidity/outdoor fan/defrost, 7 outdoor power.
class GroupDataQuery : public dudanov::midea::FrameData {
 public:
  explicit GroupDataQuery(uint8_t group)
      : FrameData({0x41, 0x21, 0x01, (uint8_t)(0x40 | group), 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00}) {
    this->appendCRC();
  }
};

struct Telemetry {
  float compHz = NAN, compHzTarget = NAN, compAmps = NAN, compVolts = NAN;
  float t1 = NAN, coilIn = NAN, coilOut = NAN, ambOut = NAN, discharge = NAN;
  float fanInRpm = NAN, fanOutRpm = NAN, humIn = NAN, outPowerW = NAN;
  bool defrost = false;
};

// Deep-telemetry support is model-dependent and not declared in the B5
// capability report — the only way to know is to ask. Each group gets 3
// strikes during probing, then is marked dead and never queried again.
class AirConditionerEx : public AirConditioner {
 public:
  Telemetry tele;
  bool groupOk[8] = {};
  // last raw response per group, for offset diagnosis via /api
  struct RawGroup { uint8_t len = 0; uint8_t bytes[32]; };
  RawGroup raw[8];

  void queueGroup(uint8_t g) {
    using namespace dudanov::midea;
    m_queueRequest(FrameType::DEVICE_QUERY, GroupDataQuery(g),
        [this, g](FrameData data) -> ResponseStatus {
          if (!data.hasPowerInfo() || data.size() < 12 || (data.data()[3] & 0x0F) != g)
            return ResponseStatus::RESPONSE_WRONG;
          m_parseGroup(g, data.data(), data.size());
          return ResponseStatus::RESPONSE_OK;
        },
        [this, g]() { groupOk[g] = true; m_fails[g] = 0; },
        [this, g]() {
          if (!groupOk[g] && ++m_fails[g] >= 3)
            m_fails[g] = DEAD;
        });
  }
  bool groupDead(uint8_t g) const { return m_fails[g] >= DEAD; }

  // Bus REPL for /probe: send one arbitrary frame, capture whatever comes
  // back. Prospecting, not parsing — the handler accepts ANY response frame,
  // so don't probe while you care about a poll completing correctly.
  String probeResp;
  bool probeDone = true;
  void probeFrame(uint8_t type, dudanov::midea::FrameData data) {
    using namespace dudanov::midea;
    probeResp = "";
    probeDone = false;
    m_queueRequestPriority(static_cast<FrameType>(type), std::move(data),
        [this](FrameData d) -> ResponseStatus {
          char b[4];
          for (uint8_t i = 0; i < d.size(); i++) {
            snprintf(b, sizeof(b), "%02x", d.data()[i]);
            probeResp += b;
          }
          probeDone = true;
          return ResponseStatus::RESPONSE_OK;
        },
        nullptr,
        [this]() { probeDone = true; });  // retries exhausted, no response
  }

 private:
  static constexpr int8_t DEAD = 127;
  int8_t m_fails[8] = {};

  void m_parseGroup(uint8_t g, const uint8_t *d, uint8_t n) {
    raw[g].len = n < sizeof(raw[g].bytes) ? n : sizeof(raw[g].bytes);
    memcpy(raw[g].bytes, d, raw[g].len);
    switch (g) {
      // Raw-dump diagnosis on this unit (see memory/README): the offsets
      // below are correct, but this model ZERO-FILLS target-Hz, current,
      // discharge temp, and all of groups 5/7. A zero in those fields can't
      // be a real reading (a running compressor draws >0 A), so 0 maps to
      // NAN = "not reported" rather than a fake measurement.
      case 1:  // compressor & refrigerant circuit
        if (n < 15) return;
        tele.compHz = d[4];
        tele.compHzTarget = d[5] ? (float)d[5] : NAN;
        tele.compAmps = d[7] ? (float)d[7] : NAN;
        tele.compVolts = d[8] ? (float)d[8] : NAN;
        tele.t1 = (d[10] - 30) / 2.0f;      // indoor ambient (unit's view)
        tele.coilIn = (d[11] - 30) / 2.0f;  // indoor coil T2
        tele.coilOut = (d[12] - 50) / 2.0f; // outdoor coil T3
        tele.ambOut = (d[13] - 50) / 2.0f;  // outdoor ambient T4
        tele.discharge = d[14] ? (float)d[14] : NAN;  // Tp, raw degC
        break;
      case 2:  // indoor fan
        if (n < 9) return;
        tele.fanInRpm = d[5] * 8;
        break;
      case 5:  // humidity sensor, outdoor fan, defrost
        if (n < 11) return;
        tele.humIn = d[4] ? (float)d[4] : NAN;
        tele.fanOutRpm = d[8] ? (float)(d[8] * 8) : NAN;
        tele.defrost = d[10] != 0;
        break;
      case 7:  // outdoor unit input power
        if (n < 12) return;
        tele.outPowerW = (d[10] | (d[11] << 8)) ? (float)(d[10] | (d[11] << 8)) : NAN;
        break;
    }
  }
};

static AirConditionerEx ac;

// Fault-code descriptions (numeric byte from the 0xC0 status frame; table
// from Midea SDK strings via node-mideahvac. Panel E-x/P-x codes are
// model-specific and don't map 1:1 — show both number and description.)
static const char *errName(uint8_t e) {
  switch (e) {
    case 0: return "none";
    case 1: return "indoor-display comms";
    case 2: return "indoor main board";
    case 3: return "indoor-outdoor comms";
    case 4: return "zero-crossing detect";
    case 5: return "indoor fan stall";
    case 6: return "outdoor condenser sensor";
    case 7: return "outdoor ambient sensor";
    case 8: return "discharge temp sensor";
    case 9: return "outdoor board";
    case 10: return "indoor temp sensor";
    case 11: return "indoor evaporator sensor";
    case 12: return "outdoor fan stall";
    case 13: return "IPM module protection";
    case 14: return "voltage protection";
    case 15: return "compressor top temp";
    case 16: return "outdoor low-temp protection";
    case 17: return "compressor position";
    case 18: return "display board";
    case 21: return "pipe temp protection";
    case 23: return "exhaust high temp";
    case 25: return "heating cold-wind protection";
    case 26: return "current protection";
    case 29: return "evaporator temp protection";
    case 30: return "condenser temp freq-limit";
    case 31: return "exhaust temp protection";
    case 32: return "indoor/outdoor mismatch";
    case 33: return "refrigerant leak";
    case 38: return "water tank full";
    default: return "unknown fault";
  }
}

// Round-robin one group query every 10s (each group refreshes ~every 40s);
// dead groups are skipped so unsupported models cost nothing after probing.
static void telemetryTick() {
  static const uint8_t GROUPS[4] = {1, 5, 2, 7};
  static uint32_t lastMs = 0;
  static uint8_t idx = 0;
  if (millis() - lastMs < 10000)
    return;
  lastMs = millis();
  for (uint8_t k = 0; k < 4; k++) {
    const uint8_t g = GROUPS[(idx + k) % 4];
    if (!ac.groupDead(g)) {
      ac.queueGroup(g);
      idx = (idx + k + 1) % 4;
      return;
    }
  }
}

// HomeKit RotationSpeed (0-100, step 20) <-> Midea fan modes.
// 0 is not used: dragging the slider to 0 makes iOS also write Active=0,
// so 0 can only ever mean "off". FAN_SILENT is folded into LOW (remote-only).
static FanMode speedToFan(int s) {
  if (s <= 20) return FanMode::FAN_AUTO;
  if (s <= 40) return FanMode::FAN_LOW;
  if (s <= 60) return FanMode::FAN_MEDIUM;
  if (s <= 80) return FanMode::FAN_HIGH;
  return FanMode::FAN_TURBO;
}

static int fanToSpeed(FanMode f) {
  switch (f) {
    case FanMode::FAN_SILENT:
    case FanMode::FAN_LOW: return 40;
    case FanMode::FAN_MEDIUM: return 60;
    case FanMode::FAN_HIGH: return 80;
    case FanMode::FAN_TURBO: return 100;
    default: return 20;  // FAN_AUTO
  }
}

// All HomeKit-initiated commands funnel through here so every service shares
// the same retry-until-confirmed machinery (see MideaHeaterCooler::loop)
static void sendAcControl(const Control &c);

struct MideaHeaterCooler : Service::HeaterCooler {
  SpanCharacteristic *active;
  SpanCharacteristic *curTemp;
  SpanCharacteristic *curState;
  SpanCharacteristic *tgtState;
  SpanCharacteristic *coolTemp;
  SpanCharacteristic *heatTemp;
  SpanCharacteristic *rotSpeed;
  SpanCharacteristic *swing;

  // set from the MideaUART state callback, consumed in loop()
  volatile bool acDirty = false;

  // Last HomeKit command, kept until the AC's status confirms it so drops
  // are retried and stale status polls can't revert the user's change.
  Control pending;
  bool pendingValid = false;
  uint32_t pendingSentAt = 0;
  uint8_t pendingAttempts = 0;

  MideaHeaterCooler() : Service::HeaterCooler() {
    active = new Characteristic::Active(0);
    curTemp = new Characteristic::CurrentTemperature(21);
    curState = new Characteristic::CurrentHeaterCoolerState(0);
    tgtState = new Characteristic::TargetHeaterCoolerState(2);
    // autoconf reported halfDegree=0: this unit only takes whole degrees
    coolTemp = new Characteristic::CoolingThresholdTemperature(24);
    coolTemp->setRange(17, 30, 1);
    heatTemp = new Characteristic::HeatingThresholdTemperature(21);
    heatTemp->setRange(17, 30, 1);
    rotSpeed = new Characteristic::RotationSpeed(20);
    rotSpeed->setRange(0, 100, 20);
    swing = new Characteristic::SwingMode(0);
  }

  // HomeKit -> AC
  boolean update() override {
    Control c;
    // Only claim a mode when iOS actually wrote power or mode; otherwise a
    // fan/swing tweak would yank the AC out of remote-set Dry/Fan-only mode.
    const bool modeIntent = active->updated() || tgtState->updated();

    if (active->getNewVal() == 0) {
      c.mode = Mode::MODE_OFF;
    } else {
      Mode m;
      switch (tgtState->getNewVal()) {
        case 0: m = Mode::MODE_AUTO; break;
        case 1: m = Mode::MODE_HEAT; break;
        default: m = Mode::MODE_COOL; break;
      }
      if (modeIntent)
        c.mode = m;

      if (modeIntent || coolTemp->updated() || heatTemp->updated()) {
        switch (m) {
          case Mode::MODE_HEAT:
            c.targetTemp = heatTemp->getNewVal<float>();
            break;
          case Mode::MODE_COOL:
            c.targetTemp = coolTemp->getNewVal<float>();
            break;
          default:
            // AUTO: Midea takes one setpoint; use the band's midpoint,
            // rounded to whole degrees (all this unit supports)
            c.targetTemp = roundf((coolTemp->getNewVal<float>() + heatTemp->getNewVal<float>()) / 2.0f);
            break;
        }
      }

      // in AUTO the AC forces fan to auto; don't send a doomed override
      if (rotSpeed->updated() && m != Mode::MODE_AUTO)
        c.fanMode = speedToFan(rotSpeed->getNewVal());

      // this unit has both louvers, so oscillate sweeps both axes
      if (swing->updated())
        c.swingMode = swing->getNewVal() ? SwingMode::SWING_BOTH : SwingMode::SWING_OFF;
    }

    sendAcControl(c);
    return true;
  }

  bool pendingConverged() const {
    return !pending.mode.hasUpdate(ac.getMode()) &&
           !pending.fanMode.hasUpdate(ac.getFanMode()) &&
           !pending.swingMode.hasUpdate(ac.getSwingMode()) &&
           !pending.preset.hasUpdate(ac.getPreset()) &&
           (!pending.targetTemp.hasValue() ||
            fabsf(pending.targetTemp.value() - ac.getTargetTemp()) < 0.26f);
  }

  // defined below the service globals; sensor part runs unconditionally,
  // switch part is skipped while a command is unconfirmed
  void syncSensorsAndSwitches(bool power, Mode mode);

  // Hysteresis: near the setpoint, hold the previous state instead of
  // flapping HEATING/COOLING <-> IDLE on every 0.5° sensor step.
  static int deriveCurrentState(int prev, Mode mode, float indoor, float target) {
    switch (mode) {
      case Mode::MODE_HEAT:
        if (indoor <= target - 0.5f) return 2;  // HEATING
        if (indoor >= target + 0.5f) return 1;  // IDLE
        return prev == 2 ? 2 : 1;
      case Mode::MODE_COOL:
        if (indoor >= target + 0.5f) return 3;  // COOLING
        if (indoor <= target - 0.5f) return 1;
        return prev == 3 ? 3 : 1;
      case Mode::MODE_AUTO:
        if (indoor >= target + 1.0f) return 3;
        if (indoor <= target - 1.0f) return 2;
        return (prev == 2 || prev == 3) ? prev : 1;
      default:
        return 1;  // DRY / FAN_ONLY: report IDLE
    }
  }

  // AC -> HomeKit
  void loop() override {
    // Retry an unconfirmed command; give up after RESEND_ATTEMPTS and let
    // the status sync below pull iOS back to the AC's real state.
    if (pendingValid) {
      if (pendingConverged()) {
        pendingValid = false;
      } else if (millis() - pendingSentAt > RESEND_INTERVAL_MS) {
        if (pendingAttempts < RESEND_ATTEMPTS) {
          pendingAttempts++;
          pendingSentAt = millis();
          ac.control(pending);
        } else {
          pendingValid = false;
        }
      }
    }

    if (!acDirty)
      return;
    acDirty = false;

    const bool power = ac.getPowerState();
    const Mode mode = ac.getMode();
    const float indoor = ac.getIndoorTemp();
    const float target = constrain(ac.getTargetTemp(), 17.0f, 30.0f);

    // exactly 0.0 = "no status yet" sentinel (real indoor 0.0°C is vanishingly rare)
    if (indoor != 0.0f && fabsf(curTemp->getVal<float>() - indoor) > 0.24f)
      curTemp->setVal(indoor);

    syncSensorsAndSwitches(power, mode);

    const int cs = power ? deriveCurrentState(curState->getVal(), mode, indoor, target) : 0;
    if (curState->getVal() != cs)
      curState->setVal(cs);

    // While a command is unconfirmed, don't let a stale status poll revert
    // the user's just-made changes in iOS.
    if (pendingValid)
      return;

    if (active->getVal() != (power ? 1 : 0))
      active->setVal(power ? 1 : 0);

    int ts = -1;
    switch (mode) {
      case Mode::MODE_AUTO: ts = 0; break;
      case Mode::MODE_HEAT: ts = 1; break;
      case Mode::MODE_COOL: ts = 2; break;
      default: break;  // DRY / FAN_ONLY have no HomeKit equivalent
    }
    if (ts >= 0 && tgtState->getVal() != ts)
      tgtState->setVal(ts);

    // Sync only the active mode's threshold: writing both would collapse
    // the AUTO band and clobber the other mode's remembered setpoint.
    switch (mode) {
      case Mode::MODE_HEAT:
        if (fabsf(heatTemp->getVal<float>() - target) > 0.24f)
          heatTemp->setVal(target);
        break;
      case Mode::MODE_COOL:
        if (fabsf(coolTemp->getVal<float>() - target) > 0.24f)
          coolTemp->setVal(target);
        break;
      default:
        break;  // AUTO: leave the user's band alone
    }

    const int sp = fanToSpeed(ac.getFanMode());
    if (rotSpeed->getVal() != sp)
      rotSpeed->setVal(sp);

    const int sw = (ac.getSwingMode() != SwingMode::SWING_OFF) ? 1 : 0;
    if (swing->getVal() != sw)
      swing->setVal(sw);

    WEBLOG("AC state: power=%d mode=%d indoor=%.1f target=%.1f fan=%d swing=%d outdoor=%.1f hum=%.0f%% powerUse=%.1f",
           power, (int)mode, indoor, ac.getTargetTemp(), (int)ac.getFanMode(), (int)ac.getSwingMode(),
           ac.getOutdoorTemp(), ac.getIndoorHum(), ac.getPowerUsage());
  }
};

// One-shot dump of the autoconf capability report to the weblog, so the
// unit's feature set is readable at http://<ip>/status
static void dumpCapabilitiesOnce() {
  static bool done = false;
  if (done)
    return;
  const auto st = ac.getAutoconfStatus();
  if (st == dudanov::midea::AUTOCONF_PROGRESS)
    return;
  done = true;
  if (st != dudanov::midea::AUTOCONF_OK) {
    WEBLOG("Autoconf did not complete (status=%d) — capabilities unknown", (int)st);
    return;
  }
  const Capabilities &c = ac.getCapabilities();
  WEBLOG("Caps modes: auto=%d cool=%d heat=%d dry=%d fanSpeedControl=%d",
         c.supportAutoMode(), c.supportCoolMode(), c.supportHeatMode(), c.supportDryMode(), c.fanSpeedControl());
  WEBLOG("Caps presets: eco=%d turbo=%d frostProtection=%d",
         c.supportEcoPreset(), c.supportTurboPreset(), c.supportFrostProtectionPreset());
  WEBLOG("Caps swing: vertical=%d horizontal=%d", c.supportVerticalSwing(), c.supportHorizontalSwing());
  WEBLOG("Caps temps: cool=%.1f-%.1f heat=%.1f-%.1f auto=%.1f-%.1f halfDegree=%d",
         c.minTempCool(), c.maxTempCool(), c.minTempHeat(), c.maxTempHeat(),
         c.minTempAuto(), c.maxTempAuto(), c.decimals());
  WEBLOG("Caps sensors/power: indoorHumidity=%d powerMeter=%d powerCalSetting=%d",
         c.indoorHumidity(), c.powerCal(), c.powerCalSetting());
  WEBLOG("Caps extras: displayControl=%d auxElectricHeat=%d buzzer=%d unitChangeable=%d activeClean=%d",
         c.supportLightControl(), c.electricAuxHeating(), c.buzzer(), c.unitChangeable(), c.activeClean());
  WEBLOG("Caps airflow: breezeControl=%d smartEye=%d windOnMe=%d windOfMe=%d silkyCool=%d noWindOnMe=%d",
         c.breezeControl(), c.smartEye(), c.windOnMe(), c.windOfMe(), c.silkyCool(), c.oneKeyNoWindOnMe());
}

// Dry mode has no HomeKit representation, so expose it as a switch:
// on = dehumidify, off = back to Cool
struct DrySwitch : Service::Switch {
  SpanCharacteristic *on;
  DrySwitch() : Service::Switch() {
    on = new Characteristic::On(0);
  }
  boolean update() override {
    Control c;
    if (on->getNewVal())
      c.mode = Mode::MODE_DRY;
    else if (ac.getMode() == Mode::MODE_DRY)
      c.mode = Mode::MODE_COOL;
    else
      return true;  // already not in dry; don't power on a sleeping AC
    sendAcControl(c);
    return true;
  }
};

// Eco / Turbo presets as switches. The AC enforces its own rules (eco only
// in Cool, turbo in Cool/Heat); a disallowed preset just fails to converge
// and the switch snaps back on the next status sync.
struct PresetSwitch : Service::Switch {
  SpanCharacteristic *on;
  const Preset preset;
  PresetSwitch(Preset p) : Service::Switch(), preset(p) {
    on = new Characteristic::On(0);
  }
  boolean update() override {
    Control c;
    if (on->getNewVal())
      c.preset = preset;
    else if (ac.getPreset() == preset)
      c.preset = Preset::PRESET_NONE;
    else
      return true;
    sendAcControl(c);
    return true;
  }
};

struct OutdoorSensor : Service::TemperatureSensor {
  SpanCharacteristic *temp;
  OutdoorSensor() : Service::TemperatureSensor() {
    temp = new Characteristic::CurrentTemperature(21);
    // expand (never shrink) the HAP default 0-100 range for sub-zero
    // readings — the proven pattern from HomeSpan's own sensor examples
    temp->setRange(-50, 100);
  }
};

static MideaHeaterCooler *mideaService;
static DrySwitch *drySwitch;
static PresetSwitch *ecoSwitch;
static PresetSwitch *turboSwitch;
static OutdoorSensor *outdoorSensor;

static void sendAcControl(const Control &c) {
  mideaService->pending = c;
  mideaService->pendingValid = true;
  mideaService->pendingAttempts = 0;
  mideaService->pendingSentAt = millis();
  ac.control(c);
}

void MideaHeaterCooler::syncSensorsAndSwitches(bool power, Mode mode) {
  const float outdoor = ac.getOutdoorTemp();
  if (outdoor != 0.0f && fabsf(outdoorSensor->temp->getVal<float>() - outdoor) > 0.24f)
    outdoorSensor->temp->setVal(outdoor);

  if (pendingValid)  // don't revert a just-toggled switch from stale status
    return;

  const int dry = (power && mode == Mode::MODE_DRY) ? 1 : 0;
  if (drySwitch->on->getVal() != dry)
    drySwitch->on->setVal(dry);

  const Preset p = ac.getPreset();
  const int eco = (power && p == Preset::PRESET_ECO) ? 1 : 0;
  if (ecoSwitch->on->getVal() != eco)
    ecoSwitch->on->setVal(eco);
  const int turbo = (power && p == Preset::PRESET_TURBO) ? 1 : 0;
  if (turboSwitch->on->getVal() != turbo)
    turboSwitch->on->setVal(turbo);
}

// ---------- status dashboard (port 8080) ----------

static WebServer dash(8080);

// ---------- flight recorder ----------
// 3 days of minute samples in a packed RAM ring (~73KB of heap budget),
// plus a 256-entry event ring that captures every state TRANSITION the
// loop iteration it is seen (mode/preset/defrost/error/link changes land
// here even if they revert between minute samples). Lost on reboot — the
// "boot" event marks the discontinuity. Archive with:
//   curl http://192.168.2.10:8080/log.csv   (full ring, streamed)
//   curl http://192.168.2.10:8080/events    (transitions)
// HIST_LEN is the tail window /api serves the dashboard chart, unchanged.
static constexpr int HIST_LEN = 240;
static constexpr int LOG_LEN = 4320;  // 3 days x 1440 minute samples
static constexpr uint32_t LOG_PERIOD_MS = 60000;
static constexpr int16_t T_NA = INT16_MIN;  // "sensor absent" in temp fields

struct __attribute__((packed)) LogRec {
  int16_t indoor, outdoor, target, coilIn, coilOut;  // degC x10, T_NA absent
  uint8_t hz;       // compressor Hz
  uint8_t fanRpm8;  // indoor fan rpm / 8 (protocol raw byte — lossless)
  uint8_t volts;    // bus volts (protocol raw byte)
  uint8_t mode, fan, preset;  // compact codes -> *_NAMES tables
  uint8_t flags;    // bit0 power 1 defrost 2 auxHeat 3 link 4 display 5 filter
  uint8_t err;
};
static LogRec logRing[LOG_LEN];
static int logHead = 0, logCount = 0;
static uint32_t lastSampleMs = 0;

// Event kinds; oldV/newV are the coded values (target is degC x10)
enum EvtKind : uint8_t { EV_BOOT, EV_POWER, EV_MODE, EV_FAN, EV_PRESET,
                         EV_TARGET, EV_DEFROST, EV_AUXHEAT, EV_ERR, EV_LINK,
                         EV_AWAYDRY, EV_GUARD };
static const char *EVT_NAMES[] = {"boot", "power", "mode", "fan", "preset",
                                  "target", "defrost", "auxHeat", "err", "link",
                                  "awayDry", "guard"};
// EV_GUARD values
static const char *GUARD_NAMES[] = {"idle", "freeze", "overheat", "dew", "yielded"};
// EV_AWAYDRY values
static const char *AWAYDRY_NAMES[] = {"off", "heat", "cool", "yielded", "done"};
struct __attribute__((packed)) LogEvent {
  uint32_t t;  // uptime seconds (wraps with millis() at 49.7 days)
  uint8_t kind;
  int16_t oldV, newV;
};
static constexpr int EVT_LEN = 256;
static LogEvent evtRing[EVT_LEN];
static int evtHead = 0, evtCount = 0;

static const char *MODE_NAMES[] = {"off", "auto", "cool", "dry", "heat", "fan"};
static const char *FAN_NAMES[] = {"auto", "silent", "low", "medium", "high", "turbo"};
static const char *PRESET_NAMES[] = {"none", "sleep", "turbo", "eco", "frost"};

static uint8_t modeCode(Mode m) {
  switch (m) {
    case Mode::MODE_AUTO: return 1;
    case Mode::MODE_COOL: return 2;
    case Mode::MODE_DRY: return 3;
    case Mode::MODE_HEAT: return 4;
    case Mode::MODE_FAN_ONLY: return 5;
    default: return 0;
  }
}

static uint8_t fanCode(FanMode f) {
  switch (f) {
    case FanMode::FAN_SILENT: return 1;
    case FanMode::FAN_LOW: return 2;
    case FanMode::FAN_MEDIUM: return 3;
    case FanMode::FAN_HIGH: return 4;
    case FanMode::FAN_TURBO: return 5;
    default: return 0;
  }
}

static uint8_t presetCode(Preset p) {
  switch (p) {
    case Preset::PRESET_SLEEP: return 1;
    case Preset::PRESET_TURBO: return 2;
    case Preset::PRESET_ECO: return 3;
    case Preset::PRESET_FREEZE_PROTECTION: return 4;
    default: return 0;
  }
}

static const char *modeName(Mode m) { return MODE_NAMES[modeCode(m)]; }
static const char *fanName(FanMode f) { return FAN_NAMES[fanCode(f)]; }
static const char *presetName(Preset p) { return PRESET_NAMES[presetCode(p)]; }

static const char *swingName(SwingMode s) {
  switch (s) {
    case SwingMode::SWING_VERTICAL: return "vertical";
    case SwingMode::SWING_HORIZONTAL: return "horizontal";
    case SwingMode::SWING_BOTH: return "both";
    default: return "off";
  }
}

static int16_t packTemp(float v) {
  return (isnan(v) || v == 0.0f) ? T_NA : (int16_t)lroundf(v * 10.0f);
}

static void logEvent(uint8_t kind, int16_t oldV, int16_t newV) {
  evtRing[evtHead] = {millis() / 1000, kind, oldV, newV};
  evtHead = (evtHead + 1) % EVT_LEN;
  if (evtCount < EVT_LEN)
    evtCount++;
}

// Runs every loop: cheap change-detection for the event ring, and a minute
// sample into the big ring. Nothing is recorded until the AC has spoken
// once, so sample spacing stays uniform (the CSV time axis relies on it).
static void recorderTick() {
  static bool started = false;
  static int16_t pPower, pMode, pFan, pPreset, pTarget, pDefrost, pAux, pErr, pLink;
  const bool link = ac.getStatusAgeMs() < 15000;
  if (!started) {
    if (ac.getIndoorTemp() == 0.0f)  // no status from the AC yet
      return;
    started = true;
    logEvent(EV_BOOT, 0, 0);
    pPower = ac.getPowerState(); pMode = modeCode(ac.getMode());
    pFan = fanCode(ac.getFanMode()); pPreset = presetCode(ac.getPreset());
    pTarget = packTemp(ac.getTargetTemp()); pDefrost = ac.tele.defrost;
    pAux = ac.getAuxHeat(); pErr = ac.getErrorCode(); pLink = link;
  }
  struct { uint8_t kind; int16_t *prev, now; } watch[] = {
      {EV_POWER, &pPower, (int16_t)ac.getPowerState()},
      {EV_MODE, &pMode, (int16_t)modeCode(ac.getMode())},
      {EV_FAN, &pFan, (int16_t)fanCode(ac.getFanMode())},
      {EV_PRESET, &pPreset, (int16_t)presetCode(ac.getPreset())},
      {EV_TARGET, &pTarget, packTemp(ac.getTargetTemp())},
      {EV_DEFROST, &pDefrost, (int16_t)ac.tele.defrost},
      {EV_AUXHEAT, &pAux, (int16_t)ac.getAuxHeat()},
      {EV_ERR, &pErr, (int16_t)ac.getErrorCode()},
      {EV_LINK, &pLink, (int16_t)link},
  };
  for (auto &w : watch) {
    if (*w.prev != w.now) {
      logEvent(w.kind, *w.prev, w.now);
      *w.prev = w.now;
    }
  }
  if (millis() - lastSampleMs < LOG_PERIOD_MS)
    return;
  lastSampleMs = millis();
  LogRec &r = logRing[logHead];
  r.indoor = packTemp(ac.getIndoorTemp());
  r.outdoor = packTemp(ac.getOutdoorTemp());
  r.target = packTemp(ac.getTargetTemp());
  r.coilIn = packTemp(ac.tele.coilIn);
  r.coilOut = packTemp(ac.tele.coilOut);
  r.hz = isnan(ac.tele.compHz) ? 0 : (uint8_t)ac.tele.compHz;
  r.fanRpm8 = isnan(ac.tele.fanInRpm) ? 0 : (uint8_t)(ac.tele.fanInRpm / 8.0f);
  r.volts = isnan(ac.tele.compVolts) ? 0 : (uint8_t)ac.tele.compVolts;
  r.mode = modeCode(ac.getMode());
  r.fan = fanCode(ac.getFanMode());
  r.preset = presetCode(ac.getPreset());
  r.flags = (ac.getPowerState() ? 1 : 0) | (ac.tele.defrost ? 2 : 0) |
            (ac.getAuxHeat() ? 4 : 0) | (link ? 8 : 0) |
            (ac.getDisplayOn() ? 16 : 0) | (ac.getFilterAlert() ? 32 : 0);
  r.err = ac.getErrorCode();
  logHead = (logHead + 1) % LOG_LEN;
  if (logCount < LOG_LEN)
    logCount++;
}

static void jsonNum(String &j, float v) {
  if (isnan(v)) {
    j += "null";
  } else {
    char b[16];
    snprintf(b, sizeof(b), "%.1f", v);
    j += b;
  }
}

static bool awayDryOn = false;
static bool awayDryHeat = true;              // current phase
static uint8_t awayDryHeatMin = 45, awayDryCoolMin = 30;
static uint32_t awayDryPhaseMs = 0;          // phase start (millis)
static uint32_t awayDryEndEpoch = 0;         // 0 = run until stopped
static void guardJson(String &j);            // defined with the guardian below

static void handleApi() {
  const bool link = ac.getStatusAgeMs() < 15000;
  String j;
  j.reserve(6144);
  char b[192];
  snprintf(b, sizeof(b),
           "{\"link\":%d,\"power\":%d,\"mode\":\"%s\",\"fan\":\"%s\","
           "\"swing\":\"%s\",\"preset\":\"%s\",",
           link, ac.getPowerState(), modeName(ac.getMode()), fanName(ac.getFanMode()),
           swingName(ac.getSwingMode()), presetName(ac.getPreset()));
  j += b;
  j += "\"indoor\":"; jsonNum(j, ac.getIndoorTemp() == 0.0f ? NAN : ac.getIndoorTemp());
  j += ",\"outdoor\":"; jsonNum(j, ac.getOutdoorTemp() == 0.0f ? NAN : ac.getOutdoorTemp());
  j += ",\"target\":"; jsonNum(j, ac.getTargetTemp() == 0.0f ? NAN : ac.getTargetTemp());
  snprintf(b, sizeof(b),
           ",\"err\":%u,\"errName\":\"%s\",\"filter\":%d,\"displayOn\":%d,\"auxHeat\":%d,\"defrost\":%d,",
           ac.getErrorCode(), errName(ac.getErrorCode()), ac.getFilterAlert(),
           ac.getDisplayOn(), ac.getAuxHeat(), ac.tele.defrost);
  j += b;
  snprintf(b, sizeof(b), "\"groups\":{\"g1\":%d,\"g2\":%d,\"g5\":%d,\"g7\":%d},\"tele\":{",
           ac.groupOk[1], ac.groupOk[2], ac.groupOk[5], ac.groupOk[7]);
  j += b;
  struct { const char *k; float v; } tv[] = {
      {"hz", ac.tele.compHz}, {"hzTarget", ac.tele.compHzTarget},
      {"amps", ac.tele.compAmps}, {"volts", ac.tele.compVolts},
      {"powW", ac.tele.outPowerW}, {"t1", ac.tele.t1},
      {"coilIn", ac.tele.coilIn}, {"coilOut", ac.tele.coilOut},
      {"ambOut", ac.tele.ambOut}, {"discharge", ac.tele.discharge},
      {"fanIn", ac.tele.fanInRpm}, {"fanOut", ac.tele.fanOutRpm},
      {"humIn", ac.tele.humIn},
  };
  for (size_t i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
    if (i) j += ',';
    j += '"'; j += tv[i].k; j += "\":";
    jsonNum(j, tv[i].v);
  }
  j += "},\"raw\":{";
  {
    bool first = true;
    for (uint8_t g = 0; g < 8; g++) {
      if (!ac.raw[g].len)
        continue;
      if (!first) j += ',';
      first = false;
      snprintf(b, sizeof(b), "\"g%u\":\"", g);
      j += b;
      for (uint8_t i = 0; i < ac.raw[g].len; i++) {
        snprintf(b, sizeof(b), "%02x ", ac.raw[g].bytes[i]);
        j += b;
      }
      j += '"';
    }
  }
  j += "},";
  {
    const uint32_t phaseLen = (awayDryHeat ? awayDryHeatMin : awayDryCoolMin) * 60000UL;
    const uint32_t el = millis() - awayDryPhaseMs;
    snprintf(b, sizeof(b),
             "\"awayDry\":{\"on\":%d,\"phase\":\"%s\",\"left\":%lu,\"heatMin\":%u,\"coolMin\":%u,\"end\":%lu},",
             awayDryOn, awayDryOn ? (awayDryHeat ? "heat" : "cool") : "off",
             (unsigned long)((awayDryOn && el < phaseLen) ? (phaseLen - el) / 1000 : 0),
             awayDryHeatMin, awayDryCoolMin, (unsigned long)awayDryEndEpoch);
    j += b;
  }
  guardJson(j);
  snprintf(b, sizeof(b),
           "\"rssi\":%d,\"heap\":%u,\"uptime\":%lu,\"fw\":\"" FIRMWARE_VERSION "\","
           "\"logCount\":%d,\"evtCount\":%d,\"hist\":{\"dt\":%lu,",
           WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
           (unsigned long)(millis() / 1000), logCount, evtCount,
           (unsigned long)(LOG_PERIOD_MS / 1000));
  j += b;
  // dashboard chart window: the newest HIST_LEN samples of the big ring
  const int n = logCount < HIST_LEN ? logCount : HIST_LEN;
  const char *keys[4] = {"\"indoor\":[", "\"outdoor\":[", "\"target\":[", "\"hz\":["};
  for (int a = 0; a < 4; a++) {
    j += keys[a];
    for (int i = 0; i < n; i++) {
      const LogRec &r = logRing[(logHead - n + i + 2 * LOG_LEN) % LOG_LEN];
      if (i) j += ',';
      float v;
      switch (a) {
        case 0: v = r.indoor == T_NA ? NAN : r.indoor / 10.0f; break;
        case 1: v = r.outdoor == T_NA ? NAN : r.outdoor / 10.0f; break;
        // preserve old histSet behavior: target is null while powered off
        case 2: v = (!(r.flags & 1) || r.target == T_NA) ? NAN : r.target / 10.0f; break;
        default: v = r.hz; break;
      }
      jsonNum(j, v);
    }
    j += (a < 3) ? "]," : "]}}";
  }
  dash.send(200, "application/json", j);
}

// Full flight-recorder ring as streamed CSV. Sample times are reconstructed
// from "minutes before now" (the ring is uniformly spaced by construction),
// as ISO8601 UTC once NTP has synced, else as negative seconds-before-now.
static void handleLogCsv() {
  dash.setContentLength(CONTENT_LENGTH_UNKNOWN);
  dash.send(200, "text/csv", "");
  const time_t nowEpoch = time(nullptr);
  const bool haveTime = nowEpoch > 1600000000;  // sanity: after Sep 2020
  const uint32_t sinceSampleS = (millis() - lastSampleMs) / 1000;
  String c;
  c.reserve(2048);
  c += "time,indoor,outdoor,target,coilIn,coilOut,hz,fanRpm,volts,"
       "mode,fan,preset,power,defrost,auxHeat,link,err\n";
  char b[160];
  for (int i = 0; i < logCount; i++) {
    const LogRec &r = logRing[(logHead - logCount + i + 2 * LOG_LEN) % LOG_LEN];
    const uint32_t age = (uint32_t)(logCount - 1 - i) * (LOG_PERIOD_MS / 1000) +
                         sinceSampleS;
    if (haveTime) {
      const time_t ts = nowEpoch - (time_t)age;
      struct tm tmv;
      gmtime_r(&ts, &tmv);
      strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
      c += b;
    } else {
      snprintf(b, sizeof(b), "-%lu", (unsigned long)age);
      c += b;
    }
    struct { int16_t v; } temps[] = {{r.indoor}, {r.outdoor}, {r.target},
                                     {r.coilIn}, {r.coilOut}};
    for (auto &t : temps) {
      if (t.v == T_NA)
        c += ',';
      else {
        snprintf(b, sizeof(b), ",%.1f", t.v / 10.0f);
        c += b;
      }
    }
    snprintf(b, sizeof(b), ",%u,%u,%u,%s,%s,%s,%u,%u,%u,%u,%u\n", r.hz,
             r.fanRpm8 * 8, r.volts, MODE_NAMES[r.mode], FAN_NAMES[r.fan],
             PRESET_NAMES[r.preset], r.flags & 1 ? 1 : 0, r.flags & 2 ? 1 : 0,
             r.flags & 4 ? 1 : 0, r.flags & 8 ? 1 : 0, r.err);
    c += b;
    if (c.length() > 1400) {
      dash.sendContent(c);
      c = "";
    }
  }
  if (c.length())
    dash.sendContent(c);
  dash.sendContent("");  // end of chunked body
}

// State-transition log: every change the recorder saw, newest last.
// "t" is uptime seconds; "ago" is seconds before now. Coded values are
// translated to names where they have them (mode/fan/preset).
static void handleEvents() {
  const uint32_t nowS = millis() / 1000;
  String j;
  j.reserve(4096);
  char b[160];
  snprintf(b, sizeof(b), "{\"uptime\":%lu,\"events\":[", (unsigned long)nowS);
  j += b;
  for (int i = 0; i < evtCount; i++) {
    const LogEvent &e = evtRing[(evtHead - evtCount + i + 2 * EVT_LEN) % EVT_LEN];
    if (i) j += ',';
    const char *oldS = nullptr, *newS = nullptr;
    char oldB[16], newB[16];
    switch (e.kind) {
      case EV_MODE: oldS = MODE_NAMES[e.oldV]; newS = MODE_NAMES[e.newV]; break;
      case EV_FAN: oldS = FAN_NAMES[e.oldV]; newS = FAN_NAMES[e.newV]; break;
      case EV_PRESET: oldS = PRESET_NAMES[e.oldV]; newS = PRESET_NAMES[e.newV]; break;
      case EV_AWAYDRY: oldS = AWAYDRY_NAMES[e.oldV]; newS = AWAYDRY_NAMES[e.newV]; break;
      case EV_GUARD: oldS = GUARD_NAMES[e.oldV]; newS = GUARD_NAMES[e.newV]; break;
      case EV_TARGET:
        snprintf(oldB, sizeof(oldB), "%.1f", e.oldV / 10.0f);
        snprintf(newB, sizeof(newB), "%.1f", e.newV / 10.0f);
        oldS = oldB; newS = newB;
        break;
      default:
        snprintf(oldB, sizeof(oldB), "%d", e.oldV);
        snprintf(newB, sizeof(newB), "%d", e.newV);
        oldS = oldB; newS = newB;
        break;
    }
    snprintf(b, sizeof(b),
             "{\"t\":%lu,\"ago\":%lu,\"what\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}",
             (unsigned long)e.t, (unsigned long)(nowS - e.t), EVT_NAMES[e.kind],
             oldS, newS);
    j += b;
  }
  j += "]}";
  dash.send(200, "application/json", j);
}

// Route MideaUART's internal log lines to the USB serial console, and
// surface errors/warnings (timeouts, bad frames) in the weblog since the
// USB port is unreachable once the dongle is inside the AC
static void mideaLogger(int level, const char *tag, int line, String format, va_list args) {
  char buf[256];
  vsnprintf(buf, sizeof(buf), format.c_str(), args);
  Serial.printf("[MideaUART:%s:%d] %s\n", tag, line, buf);
  if (level <= 2)  // 1=ERROR, 2=WARN
    WEBLOG("MideaUART %s: %s", level == 1 ? "ERROR" : "WARN", buf);
}

// Bus REPL: /probe?type=03&hex=41210144...[&crc=0]
// Sends one raw frame body to the AC (body CRC appended unless crc=0; the
// outer 0xAA framing/checksum is the library's job) and waits up to ~4s for
// the reply, returned as hex. type is the frame-type byte: 03 = query
// (safe, read-only), 02 = control (changes state — know what you're
// sending). Hex may contain spaces. Prospecting tool for unmapped protocol
// space; the flight recorder catches any behavioral side effects.
static void handleProbe() {
  const String hex = dash.arg("hex");
  const uint8_t type = strtol(dash.arg("type").c_str(), nullptr, 16);
  uint8_t body[48];
  int n = 0, hi = -1;
  for (size_t i = 0; i < hex.length(); i++) {
    const char ch = hex[i];
    int v;
    if (ch >= '0' && ch <= '9') v = ch - '0';
    else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
    else continue;  // tolerate spaces / commas / 0x prefixes
    if (hi < 0) { hi = v; continue; }
    if (n >= (int)sizeof(body)) { dash.send(400, "text/plain", "body too long\n"); return; }
    body[n++] = hi << 4 | v;
    hi = -1;
  }
  if (!n || hi >= 0 || !type) {
    dash.send(400, "text/plain", "usage: /probe?type=03&hex=4121014400...[&crc=0]\n");
    return;
  }
  dudanov::midea::FrameData d(body, n);
  if (dash.arg("crc") != "0") d.appendCRC();
  ac.probeFrame(type, std::move(d));
  // Pump the AC link synchronously so curl gets the answer in one shot.
  // We're inside dash.handleClient() here, so ac.loop() is not re-entered.
  const uint32_t t0 = millis();
  while (!ac.probeDone && millis() - t0 < 4000) {
    ac.loop();
    delay(2);
  }
  dash.send(200, "text/plain",
            ac.probeResp.length() ? ac.probeResp + "\n" : String("(no response)\n"));
}

// IR-button combo replay over UART twins: LED button -> displayToggle
// (0x41/0x61 frame), SWING -> swing change, TURBO -> preset toggle. Paced
// like keypresses; the service-manual combos want all presses within 10s.
//   /combo?seq=inquiry  = LED x3 + SWING x3  (RG57 parameter-check entry)
//   /combo?seq=turbo6   = TURBO x6           (MSV1 rating-capacity test)
static constexpr uint32_t COMBO_STEP_MS = 1400;
static char comboSteps[8];
static int comboLen = 0, comboNext = 0;
static uint32_t comboLastMs = 0;

static void comboTick() {
  if (comboNext >= comboLen) return;
  if (comboNext > 0 && millis() - comboLastMs < COMBO_STEP_MS) return;
  comboLastMs = millis();
  const char step = comboSteps[comboNext++];
  if (step == 'L') {
    ac.displayToggle();
  } else if (step == 'S') {
    Control c;
    c.swingMode = (ac.getSwingMode() == SwingMode::SWING_OFF)
                      ? SwingMode::SWING_BOTH : SwingMode::SWING_OFF;
    ac.control(c);
  } else {  // 'T'
    Control c;
    c.preset = (ac.getPreset() == Preset::PRESET_TURBO)
                   ? Preset::PRESET_NONE : Preset::PRESET_TURBO;
    ac.control(c);
  }
}

static void handleCombo() {
  const String seq = dash.arg("seq");
  const char *steps;
  if (seq == "inquiry")     steps = "LLLSSS";
  else if (seq == "turbo6") steps = "TTTTTT";
  else { dash.send(400, "text/plain", "usage: /combo?seq=inquiry|turbo6\n"); return; }
  comboLen = strlen(steps);
  memcpy(comboSteps, steps, comboLen);
  comboNext = 0;
  comboLastMs = 0;  // first step fires on the next loop pass
  dash.send(200, "text/plain",
            String("firing ") + seq + ": " + steps + ", one step per " +
            (unsigned)COMBO_STEP_MS + "ms — listen for the 2s buzzer\n");
}

// ---- Absent drying mode ---------------------------------------------------
// For drying out a wet room while nobody is there. Alternates two phases:
//   heat 30 C, medium fan  — warms the fabric of the room so it gives up its
//                            water into the air (the air saturates within an
//                            hour or so, which is why this can't run alone);
//   cool 17 C, medium fan  — condenses that water out on a cold coil at the
//                            top compressor grade, before it re-condenses on
//                            the windows and outside walls.
// A single long heat soak followed by dry mode does worse: dry mode is a
// fixed 31 Hz on this unit (see FINDINGS.md), and a warm humid room for
// hours is mould's favourite weather.
//   /awaydry?on=1[&heat=45&cool=30][&first=cool]   start (minutes per phase;
//                                     first phase defaults to heat)
//     &end=<unix epoch>               at that time: set auto 20 C and stop
//                                     ("done"), so the room is normal on arrival
//   /awaydry?on=0                     stop (leaves the AC in its current state)
// Persisted in NVS so a dongle reboot resumes rather than stranding the AC
// in one phase. Yields (switches itself off, logs "yielded") if someone
// changes mode or setpoint from the remote or HomeKit mid-phase.
static uint32_t awayDryAssertMs = 0;
static bool awayDryResume = false;  // set by awayDryLoad when NVS says "on"

static void awayDrySave() {
  Preferences p;
  p.begin("dongle");
  p.putBool("adOn", awayDryOn);
  p.putUChar("adHeat", awayDryHeatMin);
  p.putUChar("adCool", awayDryCoolMin);
  p.putULong("adEnd", awayDryEndEpoch);
  p.end();
}

static void awayDryLoad() {
  Preferences p;
  p.begin("dongle", true);
  awayDryOn = p.getBool("adOn", false);
  awayDryHeatMin = p.getUChar("adHeat", 45);
  awayDryCoolMin = p.getUChar("adCool", 30);
  awayDryEndEpoch = p.getULong("adEnd", 0);
  p.end();
  awayDryResume = awayDryOn;
}

static float awayDryTarget() { return awayDryHeat ? 30.0f : 17.0f; }
static Mode awayDryMode() { return awayDryHeat ? Mode::MODE_HEAT : Mode::MODE_COOL; }

static void awayDryAssert() {
  Control c;
  c.mode = awayDryMode();
  c.targetTemp = awayDryTarget();
  c.fanMode = FanMode::FAN_MEDIUM;
  c.preset = Preset::PRESET_NONE;
  sendAcControl(c);
  awayDryAssertMs = millis();
}

static void awayDryStart(bool heatFirst) {
  awayDryOn = true;
  awayDryResume = false;
  awayDryHeat = heatFirst;
  awayDryPhaseMs = millis();
  awayDryAssert();
  logEvent(EV_AWAYDRY, 0, awayDryHeat ? 1 : 2);
  WEBLOG("Absent drying: on, %s phase first (%u/%u min)",
         awayDryHeat ? "heat" : "cool", awayDryHeatMin, awayDryCoolMin);
}

static void awayDryStop(bool yielded) {
  if (!awayDryOn) return;
  logEvent(EV_AWAYDRY, awayDryHeat ? 1 : 2, yielded ? 3 : 0);
  awayDryOn = false;
  awayDrySave();
  WEBLOG("Absent drying: %s", yielded ? "yielded to manual change" : "off");
}

static void awayDryFinish() {
  Control c;
  c.mode = Mode::MODE_AUTO;
  c.targetTemp = 20.0f;
  c.preset = Preset::PRESET_NONE;
  sendAcControl(c);
  logEvent(EV_AWAYDRY, awayDryHeat ? 1 : 2, 4);
  awayDryOn = false;
  awayDryEndEpoch = 0;
  awayDrySave();
  WEBLOG("Absent drying: done, handed over to auto 20 C");
}

static void awayDryTick() {
  if (!awayDryOn) return;
  if (ac.getIndoorTemp() == 0.0f) return;  // AC hasn't spoken yet
  if (awayDryResume) {                      // resumed from NVS after a reboot
    awayDryStart(true);
    return;
  }
  const time_t epoch = time(nullptr);
  if (awayDryEndEpoch && epoch > 1600000000 && (uint32_t)epoch >= awayDryEndEpoch) {
    awayDryFinish();
    return;
  }
  const uint32_t now = millis();
  const uint32_t phaseLen = (awayDryHeat ? awayDryHeatMin : awayDryCoolMin) * 60000UL;
  if (now - awayDryPhaseMs >= phaseLen) {
    const uint8_t from = awayDryHeat ? 1 : 2;
    awayDryHeat = !awayDryHeat;
    awayDryPhaseMs = now;
    awayDryAssert();
    logEvent(EV_AWAYDRY, from, awayDryHeat ? 1 : 2);
    return;
  }
  // Yield to a human: once our command has settled, any later mode/setpoint
  // change that isn't ours means someone is in the room with the remote.
  if (mideaService->pendingValid) return;
  if (now - awayDryAssertMs < 90000UL) return;
  if (ac.getStatusAgeMs() > 15000) return;
  const bool diverged = !ac.getPowerState() || ac.getMode() != awayDryMode() ||
                        fabsf(ac.getTargetTemp() - awayDryTarget()) > 0.6f;
  if (diverged) awayDryStop(true);
}

static void handleAwayDry() {
  if (!dash.hasArg("on")) {
    dash.send(400, "text/plain", "usage: /awaydry?on=1[&heat=45&cool=30][&first=cool] | /awaydry?on=0\n");
    return;
  }
  if (dash.arg("on") == "1") {
    if (dash.hasArg("heat")) awayDryHeatMin = constrain(dash.arg("heat").toInt(), 5, 180);
    if (dash.hasArg("cool")) awayDryCoolMin = constrain(dash.arg("cool").toInt(), 5, 180);
    awayDryEndEpoch = dash.hasArg("end") ? (uint32_t)strtoul(dash.arg("end").c_str(), nullptr, 10) : 0;
    awayDryStart(dash.arg("first") != "cool");
    awayDrySave();
    dash.send(200, "text/plain",
              String("absent drying on: heat 30/") + awayDryHeatMin +
              "min <-> cool 17/" + awayDryCoolMin + "min, medium fan" +
              (awayDryEndEpoch ? ", then auto 20 at end time" : "") + "\n");
  } else {
    awayDryStop(false);
    dash.send(200, "text/plain", "absent drying off (AC left as is)\n");
  }
}

// ---- Guardian ---------------------------------------------------------------
// Standing protections that run on the dongle itself, no HomeKit or phone
// involved. Each only ever acts from standby (unit off, no absent-drying run,
// no HomeKit command in flight), so it never fights a person, and each
// releases the unit back to off when done.
//   freeze    T1 below freezeC   -> heat 17 for an hour
//   overheat  T1 above overheatC -> cool 25 for an hour
//   dew       the rust one. After a cold snap the machines and slab sit at
//             the old temperature; when a wet front arrives with a dew point
//             above that, every surface sweats. The unit has no humidity
//             sensor, so the dongle polls Open-Meteo hourly for the dew-point
//             forecast at the workshop's coordinates and keeps the room above
//             (highest dew point in the next 36 h + margin). The unit's lowest
//             setpoint is 17, so the dongle is the thermostat: heat 17 while
//             T1 is under target, off once it's a degree over, repeat.
//             A measured indoor humidity pushed to /hum?rh=NN sharpens it
//             (indoor dew point is then used as well as the forecast).
//   /guard?freeze=1&freezeC=5&overheat=1&overheatC=37&dew=1&dewMargin=2
//         [&lat=..&lon=..][&wx=1 to refetch now]        (all persisted)
//   /guard?stop=1     cancel a running protection (unit off, normal cooldown)
//   /hum?rh=55        pushed indoor relative humidity (valid 2 h)
struct GuardCfg {
  bool freeze = true, overheat = true, dew = true;
  float freezeC = 5.0f, overheatC = 37.0f, dewMargin = 2.0f;
  float lat = 51.228264f, lon = 1.388887f;
};
static GuardCfg guard;
static constexpr uint32_t GUARD_RUN_MS = 60UL * 60000UL;       // freeze/overheat
static constexpr uint32_t GUARD_DEW_MIN_MS = 20UL * 60000UL;   // shortest dew burst
static constexpr uint32_t GUARD_COOLDOWN_MS = 30UL * 60000UL;
static constexpr uint32_t GUARD_DEW_COOLDOWN_MS = 10UL * 60000UL;
static constexpr uint32_t GUARD_YIELD_COOLDOWN_MS = 60UL * 60000UL;
static constexpr float MASS_TAU_S = 36.0f * 3600.0f;           // fabric-temp EMA
static constexpr int WX_LOOKAHEAD_H = 36;

static float massTemp = NAN;                 // slow estimate of the room's fabric
static float wxDewNow = NAN, wxDewMax = NAN, wxTempNow = NAN;
static uint32_t wxFetchedMs = 0;             // 0 = never
static char wxErr[48] = "not fetched yet";
static uint32_t wxNextMs = 20000;            // first fetch shortly after boot
static float humRh = NAN;                    // pushed indoor RH
static uint32_t humAtMs = 0;
static uint8_t guardRun = 0;                 // GUARD_NAMES index, 0 = idle
static uint32_t guardRunMs = 0, guardAssertMs = 0, guardCooldownUntil = 0;
static uint8_t guardLast = 0;                // last thing that fired
static uint32_t guardLastMs = 0;
static uint32_t acLastOnMs = 0, massSavedMs = 0;

// -- weather fetch runs in its own task so a slow HTTP round-trip never stalls
//    HomeSpan or the UART. It only writes the wxRes* block, and the main loop
//    consumes it when wxReady flips.
static volatile bool wxBusy = false, wxReady = false;
static float wxResDewNow, wxResDewMax, wxResTemp;
static bool wxResOk;
static char wxResErr[48];

static bool wxParse(const String &s) {
  int c = s.indexOf("\"current\":{");
  if (c < 0) return false;
  int t = s.indexOf("\"temperature_2m\":", c);
  int d = s.indexOf("\"dew_point_2m\":", c);
  if (t < 0 || d < 0) return false;
  wxResTemp = atof(s.c_str() + t + 17);
  wxResDewNow = atof(s.c_str() + d + 15);
  int h = s.indexOf("\"hourly\":{");
  if (h < 0) return false;
  int a = s.indexOf("\"dew_point_2m\":[", h);
  if (a < 0) return false;
  const time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  const int idx = tmv.tm_hour;  // hourly[] starts at 00:00 UTC today
  float mx = -100.0f;
  int i = 0;
  const char *p = s.c_str() + a + 16;
  while (*p && *p != ']') {
    if (*p == ',' || *p == ' ') { p++; continue; }
    if (*p == 'n') {  // null
      while (*p && *p != ',' && *p != ']') p++;
      i++;
      continue;
    }
    char *e;
    const float v = strtof(p, &e);
    if (e == p) break;
    if (i >= idx && i < idx + WX_LOOKAHEAD_H && v > mx) mx = v;
    i++;
    p = e;
  }
  wxResDewMax = mx > -99.0f ? mx : NAN;
  return true;
}

static void wxTask(void *) {
  char url[260];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
           "&current=temperature_2m,dew_point_2m&hourly=dew_point_2m"
           "&forecast_days=2&timezone=UTC",
           guard.lat, guard.lon);
  wxResOk = false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if (http.begin(client, url)) {
    const int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      if (wxParse(body)) wxResOk = true;
      else snprintf(wxResErr, sizeof(wxResErr), "parse failed (%u bytes)", body.length());
    } else {
      snprintf(wxResErr, sizeof(wxResErr), "http %d", code);
    }
    http.end();
  } else {
    snprintf(wxResErr, sizeof(wxResErr), "connect failed");
  }
  wxReady = true;
  wxBusy = false;
  vTaskDelete(nullptr);
}

static void wxKick() {
  if (wxBusy || WiFi.status() != WL_CONNECTED) return;
  wxBusy = true;
  wxReady = false;
  xTaskCreate(wxTask, "wx", 8192, nullptr, 1, nullptr);
}

static void wxTick() {
  if (wxReady) {
    wxReady = false;
    if (wxResOk) {
      wxDewNow = wxResDewNow; wxDewMax = wxResDewMax; wxTempNow = wxResTemp;
      wxFetchedMs = millis();
      wxErr[0] = 0;
      wxNextMs = millis() + 60UL * 60000UL;
      WEBLOG("Weather: outdoor %.1fC dew %.1fC, max dew next %dh %.1fC",
             wxTempNow, wxDewNow, WX_LOOKAHEAD_H, wxDewMax);
    } else {
      strlcpy(wxErr, wxResErr, sizeof(wxErr));
      wxNextMs = millis() + 5UL * 60000UL;
      WEBLOG("Weather fetch failed: %s", wxErr);
    }
  }
  if ((int32_t)(millis() - wxNextMs) >= 0 && !wxBusy) {
    wxNextMs = millis() + 2UL * 60000UL;  // in case the task never reports
    wxKick();
  }
}

static float dewPoint(float t, float rh) {  // Magnus
  if (isnan(t) || isnan(rh) || rh <= 0) return NAN;
  const float a = 17.62f, b = 243.12f;
  const float g = logf(rh / 100.0f) + a * t / (b + t);
  return b * g / (a - g);
}

static bool wxFresh() { return wxFetchedMs && millis() - wxFetchedMs < 3UL * 3600000UL; }
static bool humFresh() { return humAtMs && millis() - humAtMs < 2UL * 3600000UL; }

// Target the room must stay above so nothing sweats
static float dewTarget() {
  float d = NAN;
  if (wxFresh() && !isnan(wxDewMax)) d = wxDewMax;
  if (humFresh()) {
    const float id = dewPoint(ac.getIndoorTemp(), humRh);
    if (!isnan(id) && (isnan(d) || id > d)) d = id;
  }
  return isnan(d) ? NAN : d + guard.dewMargin;
}

static void guardSave() {
  Preferences p;
  p.begin("dongle");
  p.putBool("gFr", guard.freeze);   p.putFloat("gFrC", guard.freezeC);
  p.putBool("gOv", guard.overheat); p.putFloat("gOvC", guard.overheatC);
  p.putBool("gDw", guard.dew);      p.putFloat("gDwM", guard.dewMargin);
  p.putFloat("gLat", guard.lat);    p.putFloat("gLon", guard.lon);
  p.end();
}

static void guardLoad() {
  Preferences p;
  p.begin("dongle", true);
  guard.freeze = p.getBool("gFr", true);     guard.freezeC = p.getFloat("gFrC", 5.0f);
  guard.overheat = p.getBool("gOv", true);   guard.overheatC = p.getFloat("gOvC", 37.0f);
  guard.dew = p.getBool("gDw", true);        guard.dewMargin = p.getFloat("gDwM", 2.0f);
  guard.lat = p.getFloat("gLat", 51.228264f); guard.lon = p.getFloat("gLon", 1.388887f);
  massTemp = p.getFloat("massT", NAN);
  p.end();
}

static void guardStart(uint8_t what) {
  Control c;
  c.mode = (what == 2) ? Mode::MODE_COOL : Mode::MODE_HEAT;
  c.targetTemp = (what == 2) ? 25.0f : 17.0f;
  c.fanMode = FanMode::FAN_AUTO;
  c.preset = Preset::PRESET_NONE;
  sendAcControl(c);
  guardRun = what;
  guardRunMs = guardAssertMs = millis();
  guardLast = what;
  guardLastMs = millis();
  logEvent(EV_GUARD, 0, what);
  WEBLOG("Guardian: %s protection on (T1 %.1fC)", GUARD_NAMES[what], ac.getIndoorTemp());
}

static void guardStop(bool yielded) {
  if (!guardRun) return;
  const uint8_t was = guardRun;
  if (!yielded) {
    Control c;
    c.mode = Mode::MODE_OFF;
    sendAcControl(c);
  }
  logEvent(EV_GUARD, was, yielded ? 4 : 0);
  WEBLOG("Guardian: %s protection %s", GUARD_NAMES[was],
         yielded ? "yielded to a manual change" : "done, unit off");
  guardRun = 0;
  guardCooldownUntil = millis() + (yielded ? GUARD_YIELD_COOLDOWN_MS
                                   : was == 3 ? GUARD_DEW_COOLDOWN_MS
                                              : GUARD_COOLDOWN_MS);
}

static void guardTick() {
  static uint32_t lastMs = 0;
  wxTick();
  const uint32_t now = millis();
  if (now - lastMs < 5000) return;
  lastMs = now;
  const float t1 = ac.getIndoorTemp();
  const bool link = ac.getStatusAgeMs() < 15000;
  if (!link || t1 == 0.0f) return;
  const bool power = ac.getPowerState();
  if (power) acLastOnMs = now;

  // Fabric-temperature estimate: T1 sampled only after the unit has been off
  // for 10 min (while running, the intake sensor reads the unit's own air).
  if (!power && now - acLastOnMs > 10UL * 60000UL) {
    if (isnan(massTemp)) massTemp = t1;
    else massTemp += (t1 - massTemp) * (5.0f / MASS_TAU_S);
    if (now - massSavedMs > 3600000UL) {
      massSavedMs = now;
      Preferences p; p.begin("dongle"); p.putFloat("massT", massTemp); p.end();
    }
  }

  if (guardRun) {
    const uint32_t ran = now - guardRunMs;
    if (!mideaService->pendingValid && now - guardAssertMs > 90000UL) {
      const Mode want = (guardRun == 2) ? Mode::MODE_COOL : Mode::MODE_HEAT;
      const float wantT = (guardRun == 2) ? 25.0f : 17.0f;
      if (!power || ac.getMode() != want || fabsf(ac.getTargetTemp() - wantT) > 0.6f) {
        guardStop(true);
        return;
      }
    }
    bool done = ran >= GUARD_RUN_MS;
    if (guardRun == 3 && ran >= GUARD_DEW_MIN_MS) {
      const float tgt = dewTarget();
      if (isnan(tgt) || t1 >= tgt + 1.0f) done = true;
    }
    if (done) guardStop(false);
    return;
  }

  if ((int32_t)(now - guardCooldownUntil) < 0) return;
  const bool standby = !power && !awayDryOn && !mideaService->pendingValid;
  if (!standby) return;
  if (guard.freeze && t1 < guard.freezeC) { guardStart(1); return; }
  if (guard.overheat && t1 > guard.overheatC) { guardStart(2); return; }
  if (guard.dew) {
    const float tgt = dewTarget();
    if (!isnan(tgt) && t1 < tgt - 0.5f) guardStart(3);
  }
}

static void guardJson(String &j) {
  char b[200];
  snprintf(b, sizeof(b),
           "\"guard\":{\"freeze\":{\"on\":%d,\"c\":%.1f},\"overheat\":{\"on\":%d,\"c\":%.1f},"
           "\"dew\":{\"on\":%d,\"margin\":%.1f,\"target\":",
           guard.freeze, guard.freezeC, guard.overheat, guard.overheatC,
           guard.dew, guard.dewMargin);
  j += b;
  jsonNum(j, dewTarget());
  j += "},\"mass\":"; jsonNum(j, massTemp);
  j += ",\"wx\":{\"dewNow\":"; jsonNum(j, wxFresh() ? wxDewNow : NAN);
  j += ",\"dewMax\":"; jsonNum(j, wxFresh() ? wxDewMax : NAN);
  j += ",\"temp\":"; jsonNum(j, wxFresh() ? wxTempNow : NAN);
  snprintf(b, sizeof(b), ",\"age\":%lu,\"err\":\"%s\",\"lat\":%.5f,\"lon\":%.5f},\"hum\":{\"rh\":",
           (unsigned long)(wxFetchedMs ? (millis() - wxFetchedMs) / 1000 : 0), wxErr,
           guard.lat, guard.lon);
  j += b;
  jsonNum(j, humFresh() ? humRh : NAN);
  j += ",\"dew\":"; jsonNum(j, humFresh() ? dewPoint(ac.getIndoorTemp(), humRh) : NAN);
  const int32_t cd = (int32_t)(guardCooldownUntil - millis());
  snprintf(b, sizeof(b),
           ",\"age\":%lu},\"run\":\"%s\",\"ran\":%lu,\"last\":\"%s\",\"lastAgo\":%lu,\"cooldown\":%ld},",
           (unsigned long)(humAtMs ? (millis() - humAtMs) / 1000 : 0), GUARD_NAMES[guardRun],
           (unsigned long)(guardRun ? (millis() - guardRunMs) / 1000 : 0),
           GUARD_NAMES[guardLast], (unsigned long)(guardLastMs ? (millis() - guardLastMs) / 1000 : 0),
           (long)(cd > 0 ? cd / 1000 : 0));
  j += b;
}

static float argF(const char *k, float cur, float lo, float hi) {
  if (!dash.hasArg(k)) return cur;
  const float v = dash.arg(k).toFloat();
  return v < lo ? lo : v > hi ? hi : v;
}

static void handleGuard() {
  if (dash.arg("stop") == "1") guardStop(false);
  if (dash.hasArg("freeze")) guard.freeze = dash.arg("freeze") == "1";
  if (dash.hasArg("overheat")) guard.overheat = dash.arg("overheat") == "1";
  if (dash.hasArg("dew")) guard.dew = dash.arg("dew") == "1";
  guard.freezeC = argF("freezeC", guard.freezeC, -5, 15);
  guard.overheatC = argF("overheatC", guard.overheatC, 25, 45);
  guard.dewMargin = argF("dewMargin", guard.dewMargin, 0, 6);
  const float lat = argF("lat", guard.lat, -90, 90), lon = argF("lon", guard.lon, -180, 180);
  const bool moved = lat != guard.lat || lon != guard.lon;
  guard.lat = lat; guard.lon = lon;
  guardSave();
  if (moved || dash.arg("wx") == "1") wxNextMs = millis();  // fetch on next tick
  String j = "{";
  guardJson(j);
  j.remove(j.length() - 1);  // trailing comma
  j += "}";
  dash.send(200, "application/json", j);
}

static void handleHum() {
  if (!dash.hasArg("rh")) { dash.send(400, "text/plain", "usage: /hum?rh=55\n"); return; }
  humRh = argF("rh", NAN, 1, 100);
  humAtMs = millis();
  char b[96];
  snprintf(b, sizeof(b), "indoor RH %.0f%% -> dew point %.1fC (T1 %.1fC)\n", humRh,
           dewPoint(ac.getIndoorTemp(), humRh), ac.getIndoorTemp());
  dash.send(200, "text/plain", b);
}

void setup() {
  Serial.begin(115200);  // USB console: HomeSpan CLI + logs

  dudanov::midea::ApplianceBase::setLogger(mideaLogger);
  Serial2.begin(9600, SERIAL_8N1, PIN_AC_RX, PIN_AC_TX);
  ac.setStream(&Serial2);
  ac.setBeeper(AC_BEEP_ON_COMMAND);
  ac.setAutoconf(true);  // must precede ac.setup() or the query never fires

  // Static IP: Starlink's router has no DHCP reservations, so pin it here.
  // Keep in sync with upload_port in platformio.ini and the Apple Passwords
  // entry ("Mini-Split dongle OTA").
  WiFi.config(IPAddress(192, 168, 2, 10), IPAddress(192, 168, 2, 1),
              IPAddress(255, 255, 255, 0), IPAddress(192, 168, 2, 1));

  homeSpan.setStatusPixel(PIN_STATUS_PIXEL);
  homeSpan.setPairingCode("46637726");  // HomeKit setup code 4663-7726
  homeSpan.enableAutoStartAP();         // no WiFi creds -> "HomeSpan-Setup" AP
  // dongle lives inside the AC, flash over WiFi. Password comes from
  // src/secrets.h (gitignored; see src/secrets.example.h) and must match
  // secrets.ini's ota_auth (the espota client side).
  homeSpan.enableOTA(OTA_PASSWORD);
  homeSpan.enableWebLog(100, "pool.ntp.org", "UTC", "status");  // http://<ip>/status
  // Bridge layout: the outdoor sensor is its own accessory so it can be
  // assigned to its own room ("Outside") in the Home app, independent of
  // the climate accessory.
  homeSpan.begin(Category::Bridges, "Mini-Split");

  // One-shot pairing wipe, keyed per structural change (accessory layout
  // changes alter the HomeKit identity/database, so force a clean unpair on
  // first boot of each new layout). Bump the key name on future changes.
  Preferences prefs;
  prefs.begin("dongle");
  const bool hkWiped = prefs.getBool("hkbridge2");
  if (!hkWiped)
    prefs.putBool("hkbridge2", true);
  prefs.end();
  if (!hkWiped)
    homeSpan.processSerialCommand("H");  // erases pairing data, reboots
  awayDryLoad();  // resumes an absent-drying run across reboots
  guardLoad();    // freeze / overheat / dew protections

  // Every function is its own bridged accessory: accessory-level names are
  // the only names this user's iOS reliably displays (service-level
  // ConfiguredName defaults were ignored), and each tile is roomable.
  new SpanAccessory();  // the bridge itself
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Manufacturer("Midea (Senville)");
  new Characteristic::Model("ESP32 UART dongle");

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Mini-Split");
  mideaService = new MideaHeaterCooler();

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Dry Mode");
  drySwitch = new DrySwitch();

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Eco");
  ecoSwitch = new PresetSwitch(Preset::PRESET_ECO);

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Turbo");
  turboSwitch = new PresetSwitch(Preset::PRESET_TURBO);

  new SpanAccessory();  // outdoor coil sensor, roomable on its own
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Outdoor Temperature");
  outdoorSensor = new OutdoorSensor();

  ac.addOnStateCallback([]() { mideaService->acDirty = true; });
  ac.setup();

  dash.on("/", []() { dash.send_P(200, "text/html", DASH_HTML); });
  dash.on("/api", handleApi);
  dash.on("/log.csv", handleLogCsv);  // 3-day flight recorder, streamed
  dash.on("/events", handleEvents);   // state transitions with timestamps
  dash.on("/probe", handleProbe);     // bus REPL: raw frame in, raw frame out
  dash.on("/combo", handleCombo);     // IR button combos via UART twins
  dash.on("/awaydry", handleAwayDry); // heat/cool alternation for a wet room
  dash.on("/guard", handleGuard);     // standing protections config/status
  dash.on("/hum", handleHum);         // push a measured indoor humidity
  // Raw NVS partition dump: full identity backup (WiFi credentials + HomeKit
  // pairing keys). Restore to a spare board with:
  //   curl -o nvs.bin http://192.168.2.10:8080/nvsdump
  //   esptool.py write_flash 0x9000 nvs.bin
  // A restored clone must NEVER run on the same network as the original.
  dash.on("/nvsdump", []() {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if (!p) { dash.send(500, "text/plain", "nvs partition not found"); return; }
    dash.setContentLength(p->size);
    dash.send(200, "application/octet-stream", "");
    WiFiClient client = dash.client();
    uint8_t buf[1024];
    for (size_t off = 0; off < p->size; off += sizeof(buf)) {
      if (esp_partition_read(p, off, buf, sizeof(buf)) != ESP_OK) return;
      client.write(buf, sizeof(buf));
    }
  });
  dash.onNotFound([]() { dash.sendHeader("Location", "/"); dash.send(302); });
  dash.begin();
}

// NOTE: homeSpan.poll() and ac.loop() share the Arduino loop task, which is
// what makes acDirty and the getVal/setVal calls race-free. Do not switch to
// homeSpan.autoPoll() (separate FreeRTOS task) without adding locking.
void loop() {
  homeSpan.poll();
  ac.loop();
  dumpCapabilitiesOnce();
  dash.handleClient();
  recorderTick();
  telemetryTick();
  comboTick();
  awayDryTick();
  guardTick();
}
