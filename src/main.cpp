#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <M5PM1.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <math.h>

namespace {

constexpr const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

constexpr uint32_t STALE_AFTER_MS = 90000;
constexpr uint32_t BLE_NOTIFY_CHUNK_DELAY_MS = 8;
constexpr uint32_t LONG_PRESS_MS = 650;
constexpr uint32_t SETTINGS_IDLE_MS = 12000;
constexpr uint32_t ACTIVITY_SOUND_COOLDOWN_MS = 1400;
constexpr uint32_t SHAKE_WAKE_ARM_DELAY_MS = 2500;
constexpr float SHAKE_WAKE_DELTA = 0.85f;
constexpr uint8_t DASHBOARD_FONT_HEIGHT = 14;
constexpr uint8_t DASHBOARD_LINE_HEIGHT = 15;
constexpr uint8_t TOP_BAR_HEIGHT = 17;
constexpr uint32_t STATUS_ANIMATION_MS = 420;
constexpr uint32_t WORK_ANIMATION_MS = 260;
constexpr uint32_t POWER_TELEMETRY_MS = 30000;
constexpr uint32_t SPEAKER_IDLE_OFF_MS = 180;
constexpr uint32_t BLE_CONN_TUNE_DELAY_MS = 800;
constexpr uint32_t PERIPHERAL_ENFORCE_MS = 30000;
constexpr uint32_t MAX_POWER_DEEP_SLEEP_MS = 20UL * 60UL * 1000UL;
constexpr uint32_t TRAVEL_SHUTDOWN_AFTER_MS = 15000;
constexpr uint8_t LOW_BATTERY_POWER_MAX_PCT = 20;
constexpr size_t BODY_LINE_COUNT = 190;
constexpr size_t RAW_ACTIVITY_COUNT = 8;
constexpr size_t SEEN_SEQ_COUNT = 24;
constexpr size_t BLE_NOTIFY_CHUNK_SIZE = 20;
constexpr size_t RX_BUFFER_LIMIT = 8192;

enum InputEvent : uint8_t {
  INPUT_NONE = 0,
  INPUT_A_SINGLE,
  INPUT_B_SINGLE,
  INPUT_A_LONG,
  INPUT_B_LONG
};

enum SoundMode : uint8_t {
  SOUND_OFF = 0,
  SOUND_SOFT,
  SOUND_ALERTS
};

enum TextNavMode : uint8_t {
  TEXT_NAV_PAGE = 0,
  TEXT_NAV_LINE
};

enum SettingIndex : uint8_t {
  SETTING_BRIGHTNESS = 0,
  SETTING_POWER,
  SETTING_SOUND,
  SETTING_TEXT_NAV,
  SETTING_AUTO_NEWEST,
  SETTING_COUNT
};

enum PowerMode : uint8_t {
  POWER_BALANCED = 0,
  POWER_SAVER,
  POWER_MAX,
  POWER_TRAVEL
};

enum Cue : uint8_t {
  CUE_NONE = 0,
  CUE_ACTIVITY,
  CUE_CONNECTED,
  CUE_COMPLETED,
  CUE_DISCONNECTED
};

enum StatusMode : uint8_t {
  MODE_OFF = 0,
  MODE_IDLE,
  MODE_WORK,
  MODE_WAIT,
  MODE_STALE,
  MODE_ERR
};

struct ButtonTracker {
  bool down = false;
  bool longSent = false;
  uint32_t pressMs = 0;
};

struct RateLimitWindowState {
  bool available = false;
  String label;
  int usedPct = -1;
  int remainingPct = -1;
  uint32_t windowMins = 0;
  uint32_t resetsAt = 0;
};

struct SettingsState {
  uint8_t brightness = 1;
  PowerMode power = POWER_BALANCED;
  SoundMode sound = SOUND_SOFT;
  TextNavMode textNav = TEXT_NAV_PAGE;
  bool autoNewest = true;
  uint8_t selected = 0;
  bool open = false;
  uint32_t lastInputMs = 0;
};

struct ActivityRecord {
  String speaker;
  String kind;
  String text;
};

struct PowerTelemetry {
  int pct = -1;
  int voltageMv = -1;
  int currentMa = 0;
  bool hasCurrent = false;
  bool usb = false;
  uint32_t sampledMs = 0;
};

struct AppState {
  String deviceName;
  uint16_t total = 0;
  uint16_t running = 0;
  uint16_t waiting = 0;
  String statusSpeaker = "System";
  String statusKind = "idle";
  String statusText = "Waiting for Codex";
  String legacySignature;
  uint64_t tokens = 0;
  bool hasTokens = false;
  int remainingPct = -1;
  RateLimitWindowState primaryLimit;
  RateLimitWindowState secondaryLimit;
  String bodyLines[BODY_LINE_COUNT];
  size_t bodyStart = 0;
  size_t bodyCount = 0;
  ActivityRecord rawActivities[RAW_ACTIVITY_COUNT];
  size_t rawStart = 0;
  size_t rawCount = 0;
  uint16_t scrollOffset = 0;
  bool hasNew = false;
  String seenSeq[SEEN_SEQ_COUNT];
  uint8_t seenSeqCursor = 0;
  uint32_t lastSnapshotMs = 0;
  uint32_t approvals = 0;
  uint32_t denials = 0;
  SettingsState settings;
};

NimBLECharacteristic* txCharacteristic = nullptr;
NimBLEServer* bleServer = nullptr;
Preferences prefs;
M5Canvas canvas(&M5.Display);
M5PM1 pm1;
PowerTelemetry powerTelemetry;
ButtonTracker buttonA;
ButtonTracker buttonB;
AppState app;
String rxBuffer;
bool bleConnected = false;
bool needsRedraw = true;
bool comboActive = false;
Cue pendingCue = CUE_NONE;
uint32_t lastActivityCueMs = 0;
uint32_t autoSleepEligibleSinceMs = 0;
uint32_t lastShakeCheckMs = 0;
uint32_t sleepEnteredMs = 0;
uint32_t speakerOffAtMs = 0;
uint32_t bleTuneRequestedMs = 0;
uint32_t lastPeripheralEnforceMs = 0;
bool displaySleeping = false;
bool displayDimmed = false;
bool accelPrimed = false;
bool pmicReady = false;
bool speakerOutputActive = false;
bool bleConnParamsPending = false;
bool travelShutdownStarted = false;
float lastAx = 0.0f;
float lastAy = 0.0f;
float lastAz = 0.0f;

String chipSuffix() {
  const uint64_t chip = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned int>(chip & 0xFFFF));
  return String(suffix);
}

String cleanText(String text) {
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.replace("\t", " ");
  while (text.indexOf("  ") >= 0) {
    text.replace("  ", " ");
  }
  text.trim();
  return text;
}

uint8_t utf8CharLength(const String& text, int index) {
  if (index < 0 || index >= static_cast<int>(text.length())) {
    return 0;
  }
  const uint8_t ch = static_cast<uint8_t>(text.charAt(index));
  if ((ch & 0x80) == 0) {
    return 1;
  }
  if ((ch & 0xE0) == 0xC0) {
    return 2;
  }
  if ((ch & 0xF0) == 0xE0) {
    return 3;
  }
  if ((ch & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

size_t utf8CharCount(const String& text) {
  size_t count = 0;
  for (int i = 0; i < static_cast<int>(text.length()); i += max<uint8_t>(1, utf8CharLength(text, i))) {
    count += 1;
  }
  return count;
}

String utf8SubstringChars(const String& text, size_t charLimit) {
  size_t count = 0;
  int end = 0;
  while (end < static_cast<int>(text.length()) && count < charLimit) {
    end += max<uint8_t>(1, utf8CharLength(text, end));
    count += 1;
  }
  return text.substring(0, end);
}

String fitText(String text, size_t limit) {
  text = cleanText(text);
  if (utf8CharCount(text) <= limit) {
    return text;
  }
  return utf8SubstringChars(text, limit > 3 ? limit - 3 : limit) + "...";
}

bool readUsbPowered() {
  const int16_t vbusMv = M5.Power.getVBUSVoltage();
  return M5.Power.isCharging() == 1 || vbusMv > 4000;
}

int readBatteryPercent() {
  const int level = M5.Power.getBatteryLevel();
  if (level < 0 || level > 100) {
    return -1;
  }
  return level;
}

bool samplePowerTelemetry(bool force = false) {
  const uint32_t now = millis();
  if (!force && powerTelemetry.sampledMs > 0 && now - powerTelemetry.sampledMs < POWER_TELEMETRY_MS) {
    return false;
  }

  const int oldPct = powerTelemetry.pct;
  const bool oldUsb = powerTelemetry.usb;
  const int oldVoltage = powerTelemetry.voltageMv;

  powerTelemetry.pct = readBatteryPercent();
  const int16_t voltage = M5.Power.getBatteryVoltage();
  powerTelemetry.voltageMv = voltage > 0 ? voltage : -1;
  const int32_t current = M5.Power.getBatteryCurrent();
  powerTelemetry.hasCurrent = current > -2000 && current < 2000;
  powerTelemetry.currentMa = powerTelemetry.hasCurrent ? static_cast<int>(current) : 0;
  powerTelemetry.usb = readUsbPowered();
  powerTelemetry.sampledMs = now;

  return oldPct != powerTelemetry.pct || oldUsb != powerTelemetry.usb || abs(oldVoltage - powerTelemetry.voltageMv) >= 50;
}

int batteryPercent() {
  if (powerTelemetry.sampledMs == 0) {
    samplePowerTelemetry(true);
  }
  return powerTelemetry.pct;
}

bool isUsbPowered() {
  if (powerTelemetry.sampledMs == 0) {
    samplePowerTelemetry(true);
  }
  return powerTelemetry.usb;
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return canvas.color565(r, g, b);
}

void useUiFont() {
  canvas.setFont(&fonts::efontCN_14);
  canvas.setTextSize(1);
}

void useBodyFontFor(const String&) {
  canvas.setFont(&fonts::efontCN_14);
  canvas.setTextSize(1);
}

StatusMode currentStatusMode() {
  if (app.statusKind == "error") {
    return MODE_ERR;
  }
  if (!bleConnected) {
    return MODE_OFF;
  }
  if (app.lastSnapshotMs > 0 && millis() - app.lastSnapshotMs > STALE_AFTER_MS) {
    return MODE_STALE;
  }
  if (app.waiting > 0) {
    return MODE_WAIT;
  }
  if (app.running > 0) {
    return MODE_WORK;
  }
  return MODE_IDLE;
}

bool lowBatteryPowerSaveActive() {
  const int pct = batteryPercent();
  return !isUsbPowered() && pct >= 0 && pct <= LOW_BATTERY_POWER_MAX_PCT && app.settings.power < POWER_MAX;
}

PowerMode effectivePowerMode() {
  if (lowBatteryPowerSaveActive()) {
    return POWER_MAX;
  }
  return app.settings.power;
}

bool travelModeActive() {
  return app.settings.power == POWER_TRAVEL;
}

uint16_t statusColor(StatusMode mode) {
  switch (mode) {
    case MODE_ERR:
      return rgb(122, 36, 48);
    case MODE_OFF:
      return rgb(8, 9, 10);
    case MODE_STALE:
      return rgb(75, 66, 106);
    case MODE_WAIT:
      return rgb(117, 85, 42);
    case MODE_WORK:
      return rgb(14, 95, 111);
    case MODE_IDLE:
    default:
      return rgb(42, 46, 50);
  }
}

String modeLabel(StatusMode mode) {
  switch (mode) {
    case MODE_ERR:
      return "ERR";
    case MODE_OFF:
      return "OFF";
    case MODE_STALE:
      return "STALE";
    case MODE_WAIT:
      return "WAIT";
    case MODE_WORK:
      return "WORK";
    case MODE_IDLE:
    default:
      return "IDLE";
  }
}

bool statusModeAnimates(StatusMode mode) {
  return mode == MODE_WORK || mode == MODE_WAIT || mode == MODE_STALE || mode == MODE_ERR;
}

uint32_t powerProfileFactor() {
  switch (effectivePowerMode()) {
    case POWER_TRAVEL:
      return 3;
    case POWER_MAX:
      return 2;
    case POWER_SAVER:
      return 1;
    case POWER_BALANCED:
    default:
      return 0;
  }
}

uint32_t statusAnimationInterval(StatusMode mode) {
  const uint32_t base = mode == MODE_WORK ? WORK_ANIMATION_MS : STATUS_ANIMATION_MS;
  return base + powerProfileFactor() * 120;
}

uint16_t limitColor(int remainingPct) {
  if (remainingPct < 0) {
    return rgb(76, 78, 82);
  }
  if (remainingPct <= 15) {
    return rgb(206, 84, 78);
  }
  if (remainingPct <= 35) {
    return rgb(214, 151, 67);
  }
  if (remainingPct <= 65) {
    return rgb(114, 166, 109);
  }
  return rgb(74, 166, 190);
}

uint8_t brightnessValue() {
  static const uint8_t values[] = {24, 76, 148};
  if (lowBatteryPowerSaveActive()) {
    return values[0];
  }
  return values[constrain(app.settings.brightness, 0, 2)];
}

void applyBrightness() {
  M5.Display.setBrightness(brightnessValue());
}

uint32_t autoDimAfterMs() {
  return 0;
}

uint32_t autoSleepAfterMs() {
  return 10000;
}

uint32_t deepSleepAfterMs() {
  return effectivePowerMode() == POWER_MAX ? MAX_POWER_DEEP_SLEEP_MS : 0;
}

uint32_t travelShutdownAfterMs() {
  return travelModeActive() ? TRAVEL_SHUTDOWN_AFTER_MS : 0;
}

uint32_t activeCpuMhz() {
  switch (effectivePowerMode()) {
    case POWER_TRAVEL:
    case POWER_MAX:
      return 80;
    case POWER_SAVER:
      return 120;
    case POWER_BALANCED:
    default:
      return 160;
  }
}

uint32_t sleepCpuMhz() {
  return 80;
}

uint32_t shakeCheckMs() {
  switch (effectivePowerMode()) {
    case POWER_TRAVEL:
      return 700;
    case POWER_MAX:
      return 500;
    case POWER_SAVER:
      return 320;
    case POWER_BALANCED:
    default:
      return 220;
  }
}

void setCpuMhzIfNeeded(uint32_t mhz) {
  static uint32_t currentMhz = 0;
  if (currentMhz == mhz) {
    return;
  }
  setCpuFrequencyMhz(mhz);
  currentMhz = mhz;
}

void enforceUnusedPowerRails(bool force = false) {
  const uint32_t now = millis();
  if (!force && lastPeripheralEnforceMs > 0 && now - lastPeripheralEnforceMs < PERIPHERAL_ENFORCE_MS) {
    return;
  }
  lastPeripheralEnforceMs = now;

  M5.Power.setLed(0);
  M5.Power.setExtOutput(false);
  if (!pmicReady) {
    return;
  }
  pm1.disableLeds();
  pm1.setLedEnLevel(false);
  pm1.setBoostEnable(false);
  pm1.boostSetPowerHold(false);
}

void shutdownSpeakerOutput() {
  M5.Speaker.stop();
  M5.Speaker.setVolume(0);
  speakerOutputActive = false;
  speakerOffAtMs = 0;
}

void setupSpeakerOutput() {
  M5.Speaker.begin();
  M5.Speaker.setAllChannelVolume(255);
  M5.Speaker.setChannelVolume(0, 255);
  M5.Speaker.setVolume(0);
  speakerOutputActive = false;
  speakerOffAtMs = 0;
}

void setupPmicPowerSaving() {
  pmicReady = pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) == M5PM1_OK;
  if (!pmicReady) {
    return;
  }
  pm1.setLedEnLevel(false);
  pm1.disableLeds();
  pm1.setI2cSleepTime(2);
  pm1.setAutoWakeEnable(true);
  pm1.setBoostEnable(false);
  pm1.boostSetPowerHold(false);
  pm1.ldoSetPowerHold(false);
  pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_4, false);
  enforceUnusedPowerRails(true);
}

void loadSettings() {
  app.settings.brightness = prefs.isKey("br") ? prefs.getUChar("br", 1) : (isUsbPowered() ? 1 : 0);
  if (app.settings.brightness > 2) {
    app.settings.brightness = isUsbPowered() ? 1 : 0;
  }
  app.settings.power = static_cast<PowerMode>(prefs.isKey("pwr") ? prefs.getUChar("pwr", POWER_BALANCED) : (isUsbPowered() ? POWER_BALANCED : POWER_SAVER));
  if (app.settings.power > POWER_TRAVEL) {
    app.settings.power = POWER_BALANCED;
  }
  app.settings.sound = static_cast<SoundMode>(prefs.getUChar("snd", SOUND_SOFT));
  if (app.settings.sound > SOUND_ALERTS) {
    app.settings.sound = SOUND_SOFT;
  }
  app.settings.textNav = static_cast<TextNavMode>(prefs.getUChar("nav", TEXT_NAV_PAGE));
  if (app.settings.textNav > TEXT_NAV_LINE) {
    app.settings.textNav = TEXT_NAV_PAGE;
  }
  app.settings.autoNewest = prefs.getBool("auto", true);
}

void saveSettings() {
  prefs.putUChar("br", app.settings.brightness);
  prefs.putUChar("pwr", app.settings.power);
  prefs.putUChar("snd", app.settings.sound);
  prefs.putUChar("nav", app.settings.textNav);
  prefs.putBool("auto", app.settings.autoNewest);
}

void wakeDisplay() {
  travelShutdownStarted = false;
  if (!displaySleeping) {
    if (displayDimmed) {
      displayDimmed = false;
      applyBrightness();
      needsRedraw = true;
    }
    autoSleepEligibleSinceMs = 0;
    return;
  }
  displaySleeping = false;
  displayDimmed = false;
  setCpuMhzIfNeeded(activeCpuMhz());
  M5.Display.wakeup();
  applyBrightness();
  autoSleepEligibleSinceMs = 0;
  needsRedraw = true;
}

void enterDisplaySleep() {
  if (displaySleeping) {
    return;
  }
  app.settings.open = false;
  accelPrimed = false;
  autoSleepEligibleSinceMs = 0;
  sleepEnteredMs = millis();
  travelShutdownStarted = false;
  displaySleeping = true;
  displayDimmed = false;
  pendingCue = CUE_NONE;
  shutdownSpeakerOutput();
  M5.Power.setLed(0);
  if (pmicReady) {
    pm1.disableLeds();
  }
  M5.Display.sleep();
  setCpuMhzIfNeeded(sleepCpuMhz());
  needsRedraw = false;
}

void requestCue(Cue cue) {
  if (cue == CUE_NONE || app.settings.sound == SOUND_OFF) {
    return;
  }
  if (app.settings.sound == SOUND_ALERTS && cue == CUE_ACTIVITY) {
    return;
  }
  if (cue == CUE_ACTIVITY && millis() - lastActivityCueMs < ACTIVITY_SOUND_COOLDOWN_MS) {
    return;
  }
  if (pendingCue == CUE_NONE || cue != CUE_ACTIVITY) {
    pendingCue = cue;
  }
}

void toneSoft(float frequency, uint32_t duration) {
  if (!M5.Speaker.isRunning()) {
    M5.Speaker.begin();
  }
  M5.Speaker.setAllChannelVolume(255);
  M5.Speaker.setChannelVolume(0, 255);
  M5.Speaker.setVolume(app.settings.sound == SOUND_ALERTS ? 84 : 58);
  M5.Speaker.tone(frequency, duration);
  speakerOutputActive = true;
  speakerOffAtMs = millis() + duration + SPEAKER_IDLE_OFF_MS;
}

void handleSpeakerPower() {
  if (!speakerOutputActive || speakerOffAtMs == 0) {
    return;
  }
  if (static_cast<int32_t>(millis() - speakerOffAtMs) >= 0 && !M5.Speaker.isPlaying()) {
    M5.Speaker.setVolume(0);
    speakerOutputActive = false;
    speakerOffAtMs = 0;
  }
}

void playPendingCue() {
  if (pendingCue == CUE_NONE || app.settings.sound == SOUND_OFF) {
    pendingCue = CUE_NONE;
    return;
  }

  const Cue cue = pendingCue;
  pendingCue = CUE_NONE;
  if (cue == CUE_ACTIVITY) {
    lastActivityCueMs = millis();
    toneSoft(1046, 48);
    return;
  }
  if (cue == CUE_CONNECTED) {
    toneSoft(740, 58);
    delay(72);
    toneSoft(988, 68);
    return;
  }
  if (cue == CUE_COMPLETED) {
    toneSoft(880, 70);
    delay(86);
    toneSoft(1175, 86);
    return;
  }
  if (cue == CUE_DISCONNECTED) {
    toneSoft(330, 120);
  }
}

void sendLine(const String& line) {
  if (!bleConnected || txCharacteristic == nullptr) {
    return;
  }

  const uint8_t* data = reinterpret_cast<const uint8_t*>(line.c_str());
  const size_t length = line.length();
  for (size_t offset = 0; offset < length; offset += BLE_NOTIFY_CHUNK_SIZE) {
    const size_t chunkSize = min(BLE_NOTIFY_CHUNK_SIZE, length - offset);
    txCharacteristic->setValue(const_cast<uint8_t*>(data + offset), chunkSize);
    txCharacteristic->notify();
    delay(BLE_NOTIFY_CHUNK_DELAY_MS);
  }
}

void sendJson(JsonDocument& doc) {
  String line;
  serializeJson(doc, line);
  line += "\n";
  sendLine(line);
}

void sendAck(const char* command, bool ok, const char* error = nullptr) {
  JsonDocument doc;
  doc["ack"] = command;
  doc["ok"] = ok;
  if (error != nullptr) {
    doc["error"] = error;
  }
  sendJson(doc);
}

void sendStatusAck() {
  samplePowerTelemetry(true);
  JsonDocument doc;
  doc["ack"] = "status";
  doc["ok"] = true;
  JsonObject data = doc["data"].to<JsonObject>();
  data["name"] = app.deviceName;
  data["sec"] = false;
  JsonObject battery = data["bat"].to<JsonObject>();
  const int pct = batteryPercent();
  if (pct >= 0) {
    battery["pct"] = pct;
  }
  if (powerTelemetry.voltageMv > 0) {
    battery["mv"] = powerTelemetry.voltageMv;
  }
  if (powerTelemetry.hasCurrent) {
    battery["ma"] = powerTelemetry.currentMa;
  }
  battery["usb"] = isUsbPowered();
  JsonObject sys = data["sys"].to<JsonObject>();
  sys["up"] = millis() / 1000;
  sys["heap"] = ESP.getFreeHeap();
  sys["cpu_mhz"] = getCpuFrequencyMhz();
  JsonObject stats = data["stats"].to<JsonObject>();
  stats["appr"] = app.approvals;
  stats["deny"] = app.denials;
  JsonObject settings = data["settings"].to<JsonObject>();
  settings["brightness"] = app.settings.brightness;
  settings["power"] = app.settings.power;
  settings["effective_power"] = effectivePowerMode();
  settings["low_battery_max"] = lowBatteryPowerSaveActive();
  settings["sound"] = app.settings.sound;
  settings["nav"] = app.settings.textNav;
  settings["auto_newest"] = app.settings.autoNewest;
  settings["auto_dim_ms"] = autoDimAfterMs();
  settings["auto_sleep_ms"] = autoSleepAfterMs();
  settings["deep_sleep_ms"] = deepSleepAfterMs();
  settings["travel_shutdown_ms"] = travelShutdownAfterMs();
  sendJson(doc);
}

void sendControl(const char* action) {
  JsonDocument doc;
  doc["cmd"] = "control";
  doc["action"] = action;
  sendJson(doc);
}

uint8_t bodyLineHeight() {
  return DASHBOARD_LINE_HEIGHT;
}

uint8_t bodyTopY() {
  return 92;
}

uint8_t bodyBottomY() {
  return canvas.height();
}

uint8_t visibleBodyLines() {
  const uint8_t lineHeight = bodyLineHeight();
  return max<uint8_t>(1, (bodyBottomY() - bodyTopY()) / lineHeight);
}

uint16_t maxScrollOffset() {
  const uint8_t visible = visibleBodyLines();
  if (app.bodyCount <= visible) {
    return 0;
  }
  return static_cast<uint16_t>(app.bodyCount - visible);
}

void clampScroll() {
  app.scrollOffset = min<uint16_t>(app.scrollOffset, maxScrollOffset());
  if (app.scrollOffset == 0) {
    app.hasNew = false;
  }
}

void resetBodyLines() {
  app.bodyStart = 0;
  app.bodyCount = 0;
}

void appendBodyLine(const String& line) {
  const size_t writeIndex = app.bodyCount < BODY_LINE_COUNT ? (app.bodyStart + app.bodyCount) % BODY_LINE_COUNT : app.bodyStart;
  app.bodyLines[writeIndex] = line;
  if (app.bodyCount < BODY_LINE_COUNT) {
    app.bodyCount += 1;
  } else {
    app.bodyStart = (app.bodyStart + 1) % BODY_LINE_COUNT;
  }
}

String bodyLineAt(size_t chronologicalIndex) {
  if (chronologicalIndex >= app.bodyCount) {
    return "";
  }
  return app.bodyLines[(app.bodyStart + chronologicalIndex) % BODY_LINE_COUNT];
}

uint16_t wrapTextLines(const String& firstPrefix, const String& nextPrefix, String text, bool appendLines) {
  text = cleanText(text);
  if (text.isEmpty()) {
    return 0;
  }

  String remaining = text;
  String prefix = firstPrefix;
  uint16_t added = 0;
  const int maxWidth = canvas.width() - 8;
  useBodyFontFor(text);

  while (!remaining.isEmpty()) {
    String chunk;
    int consumed = 0;
    int lastBreak = -1;

    while (consumed < static_cast<int>(remaining.length())) {
      const uint8_t charLen = max<uint8_t>(1, utf8CharLength(remaining, consumed));
      const String nextChar = remaining.substring(consumed, min<int>(remaining.length(), consumed + charLen));
      const String candidate = prefix + chunk + nextChar;
      if (!chunk.isEmpty() && canvas.textWidth(candidate.c_str()) > maxWidth) {
        break;
      }
      chunk += nextChar;
      consumed += charLen;
      if (nextChar == " " || nextChar == "/" || nextChar == "-" || nextChar == "," || nextChar == ".") {
        lastBreak = consumed;
      }
    }

    if (consumed < static_cast<int>(remaining.length()) && lastBreak > 0 && lastBreak < consumed) {
      consumed = lastBreak;
      chunk = remaining.substring(0, consumed);
    }
    if (consumed <= 0) {
      consumed = max<uint8_t>(1, utf8CharLength(remaining, 0));
      chunk = remaining.substring(0, consumed);
    }

    chunk.trim();
    if (!chunk.isEmpty()) {
      if (appendLines) {
        appendBodyLine(prefix + chunk);
      }
      added += 1;
    }
    remaining = remaining.substring(consumed);
    remaining.trim();
    prefix = nextPrefix;
  }

  useUiFont();
  return added;
}

String activityHeader(String speaker, String kind) {
  speaker = fitText(speaker.isEmpty() ? "Codex" : speaker, 9);
  kind = fitText(kind.isEmpty() ? "message" : kind, 9);
  if (kind == "message" || kind == "status") {
    return "[" + speaker + "]";
  }
  return "[" + speaker + " " + kind + "]";
}

uint16_t appendActivityBlockLines(const String& speaker, const String& kind, String text, bool appendLines) {
  text = cleanText(text);
  if (text.isEmpty()) {
    return 0;
  }

  uint16_t added = 0;
  if (appendLines) {
    appendBodyLine(activityHeader(speaker, kind));
  }
  added += 1;
  added += wrapTextLines("", "", text, appendLines);
  if (appendLines) {
    appendBodyLine("");
  }
  added += 1;
  return added;
}

void storeActivityBlock(const String& speaker, const String& kind, const String& text) {
  const size_t writeIndex = app.rawCount < RAW_ACTIVITY_COUNT ? (app.rawStart + app.rawCount) % RAW_ACTIVITY_COUNT : app.rawStart;
  app.rawActivities[writeIndex] = {speaker, kind, text};
  if (app.rawCount < RAW_ACTIVITY_COUNT) {
    app.rawCount += 1;
  } else {
    app.rawStart = (app.rawStart + 1) % RAW_ACTIVITY_COUNT;
  }
}

ActivityRecord rawActivityAt(size_t chronologicalIndex) {
  if (chronologicalIndex >= app.rawCount) {
    return {};
  }
  return app.rawActivities[(app.rawStart + chronologicalIndex) % RAW_ACTIVITY_COUNT];
}

void rebuildBodyLines() {
  resetBodyLines();
  for (size_t i = 0; i < app.rawCount; ++i) {
    const ActivityRecord record = rawActivityAt(i);
    appendActivityBlockLines(record.speaker, record.kind, record.text, true);
  }
  clampScroll();
}

uint16_t appendActivityBlock(const String& speaker, const String& kind, String text) {
  text = cleanText(text);
  if (text.isEmpty()) {
    return 0;
  }

  const uint16_t added = appendActivityBlockLines(speaker, kind, text, false);
  storeActivityBlock(speaker, kind, text);
  rebuildBodyLines();
  return min<uint16_t>(added, BODY_LINE_COUNT);
}

bool isToolActivity(String speaker, String kind) {
  speaker.toLowerCase();
  kind.toLowerCase();
  return speaker.indexOf("tool") >= 0 || kind.indexOf("tool") >= 0 || kind == "exec" || kind == "command";
}

void applyNewLines(uint8_t added) {
  if (added == 0) {
    return;
  }
  if (displaySleeping) {
    wakeDisplay();
  }
  if (app.scrollOffset == 0 && app.settings.autoNewest) {
    app.hasNew = false;
  } else {
    app.scrollOffset = min<uint16_t>(maxScrollOffset(), app.scrollOffset + added);
    app.hasNew = true;
  }
  requestCue(CUE_ACTIVITY);
  needsRedraw = true;
}

bool rememberSeq(const String& seq) {
  if (seq.isEmpty()) {
    return true;
  }
  for (const String& seen : app.seenSeq) {
    if (seen == seq) {
      return false;
    }
  }
  app.seenSeq[app.seenSeqCursor] = seq;
  app.seenSeqCursor = (app.seenSeqCursor + 1) % SEEN_SEQ_COUNT;
  return true;
}

bool setCurrentStatus(String speaker, String kind, String text) {
  speaker = fitText(speaker.isEmpty() ? "Codex" : speaker, 12);
  kind = fitText(kind.isEmpty() ? "status" : kind, 16);
  text = fitText(text.isEmpty() ? kind : text, 96);
  const bool completed = kind == "complete" || kind == "completed" || kind == "task_complete";
  const bool changed = speaker != app.statusSpeaker || text != app.statusText || kind != app.statusKind;

  app.statusSpeaker = speaker;
  app.statusKind = kind;
  app.statusText = text;
  if (
    displaySleeping
    && changed
    && (
      kind == "running"
      || kind == "working"
      || kind == "started"
      || kind == "message"
      || kind == "connected"
      || kind == "disconnected"
      || kind == "completed"
      || kind == "complete"
      || kind == "error"
    )
  ) {
    wakeDisplay();
  }
  if (changed && completed) {
    requestCue(CUE_COMPLETED);
  }
  if (changed) {
    needsRedraw = true;
  }
  return changed;
}

bool handleStatus(JsonObject status) {
  const String speaker = status["speaker"] | "Codex";
  const String kind = status["kind"] | "status";
  const String text = status["text"] | "";
  return setCurrentStatus(speaker, kind, text);
}

uint8_t handleActivity(JsonArray activity) {
  uint8_t added = 0;
  for (JsonVariant value : activity) {
    if (!value.is<JsonObject>()) {
      continue;
    }
    JsonObject item = value.as<JsonObject>();
    const String seq = item["seq"].isNull() ? "" : item["seq"].as<String>();
    if (!rememberSeq(seq)) {
      continue;
    }
    const String speaker = item["speaker"] | "Codex";
    const String kind = item["kind"] | "message";
    const String text = item["text"] | "";
    if (isToolActivity(speaker, kind)) {
      setCurrentStatus("Tool", kind, text);
      continue;
    }
    if (displaySleeping) {
      wakeDisplay();
    }
    added += appendActivityBlock(speaker, kind, text);
  }
  applyNewLines(added);
  return added;
}

String legacySnapshotSignature(JsonDocument& doc) {
  String signature = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "";
  if (doc["entries"].is<JsonArray>()) {
    JsonArray entries = doc["entries"].as<JsonArray>();
    for (JsonVariant value : entries) {
      signature += "|";
      signature += value.as<String>();
    }
  }
  return signature;
}

void handleLegacyText(JsonDocument& doc) {
  const String signature = legacySnapshotSignature(doc);
  if (signature.isEmpty() || signature == app.legacySignature) {
    return;
  }
  app.legacySignature = signature;

  if (doc["msg"].is<const char*>()) {
    setCurrentStatus("Codex", "status", doc["msg"].as<String>());
  }

  uint8_t added = 0;
  if (doc["entries"].is<JsonArray>()) {
    JsonArray entries = doc["entries"].as<JsonArray>();
    for (int index = static_cast<int>(entries.size()) - 1; index >= 0; --index) {
      added += appendActivityBlock("Codex", "legacy", entries[index].as<String>());
    }
  } else if (doc["msg"].is<const char*>()) {
    added += appendActivityBlock("Codex", "legacy", doc["msg"].as<String>());
  }
  applyNewLines(added);
}

bool updateWindow(JsonObject source, RateLimitWindowState& target, const char* fallbackLabel) {
  const bool oldAvailable = target.available;
  const String oldLabel = target.label;
  const int oldUsed = target.usedPct;
  const int oldRemaining = target.remainingPct;
  const uint32_t oldWindow = target.windowMins;
  const uint32_t oldReset = target.resetsAt;

  target.available = true;
  target.label = source["label"] | fallbackLabel;
  if (source["remaining_percent"].is<int>()) {
    target.remainingPct = constrain(source["remaining_percent"].as<int>(), 0, 100);
  } else if (source["remainingPercent"].is<int>()) {
    target.remainingPct = constrain(source["remainingPercent"].as<int>(), 0, 100);
  }
  if (source["used_percent"].is<int>()) {
    target.usedPct = constrain(source["used_percent"].as<int>(), 0, 100);
  } else if (source["usedPercent"].is<int>()) {
    target.usedPct = constrain(source["usedPercent"].as<int>(), 0, 100);
  }
  if (target.remainingPct < 0 && target.usedPct >= 0) {
    target.remainingPct = 100 - target.usedPct;
  }
  target.windowMins = source["window_mins"] | source["windowDurationMins"] | target.windowMins;
  target.resetsAt = source["resets_at"] | source["resetsAt"] | target.resetsAt;

  return oldAvailable != target.available
    || oldLabel != target.label
    || oldUsed != target.usedPct
    || oldRemaining != target.remainingPct
    || oldWindow != target.windowMins
    || oldReset != target.resetsAt;
}

bool updateRateLimits(JsonObject rateLimits) {
  bool changed = false;
  if (rateLimits["primary"].is<JsonObject>()) {
    changed = updateWindow(rateLimits["primary"].as<JsonObject>(), app.primaryLimit, "5h") || changed;
  }
  if (rateLimits["secondary"].is<JsonObject>()) {
    changed = updateWindow(rateLimits["secondary"].as<JsonObject>(), app.secondaryLimit, "7d") || changed;
  }
  return changed;
}

void handleSnapshot(JsonDocument& doc) {
  bool changed = false;
  const uint16_t nextTotal = doc["total"] | app.total;
  const uint16_t nextRunning = doc["running"] | 0;
  const uint16_t nextWaiting = doc["waiting"] | 0;
  changed = nextTotal != app.total || nextRunning != app.running || nextWaiting != app.waiting;
  app.total = nextTotal;
  app.running = nextRunning;
  app.waiting = nextWaiting;

  if (doc["tokens"].is<uint64_t>()) {
    const uint64_t nextTokens = doc["tokens"].as<uint64_t>();
    changed = changed || !app.hasTokens || nextTokens != app.tokens;
    app.tokens = nextTokens;
    app.hasTokens = true;
  } else if (doc["tokens"].is<uint32_t>()) {
    const uint64_t nextTokens = doc["tokens"].as<uint32_t>();
    changed = changed || !app.hasTokens || nextTokens != app.tokens;
    app.tokens = nextTokens;
    app.hasTokens = true;
  }

  if (doc["rate_limit_remaining_percent"].is<int>()) {
    const int nextPct = constrain(doc["rate_limit_remaining_percent"].as<int>(), 0, 100);
    changed = changed || nextPct != app.remainingPct;
    app.remainingPct = nextPct;
  } else if (doc["remaining_pct"].is<int>()) {
    const int nextPct = constrain(doc["remaining_pct"].as<int>(), 0, 100);
    changed = changed || nextPct != app.remainingPct;
    app.remainingPct = nextPct;
  } else if (doc["remaining"].is<int>()) {
    const int nextPct = constrain(doc["remaining"].as<int>(), 0, 100);
    changed = changed || nextPct != app.remainingPct;
    app.remainingPct = nextPct;
  }

  if (doc["rate_limits"].is<JsonObject>()) {
    changed = updateRateLimits(doc["rate_limits"].as<JsonObject>()) || changed;
  }

  const bool hasStructuredText = doc["status"].is<JsonObject>() || doc["activity"].is<JsonArray>();
  if (doc["status"].is<JsonObject>()) {
    changed = handleStatus(doc["status"].as<JsonObject>()) || changed;
  } else if (doc["msg"].is<const char*>()) {
    changed = setCurrentStatus("Codex", "status", doc["msg"].as<String>()) || changed;
  }

  if (doc["activity"].is<JsonArray>()) {
    changed = handleActivity(doc["activity"].as<JsonArray>()) > 0 || changed;
  } else if (!hasStructuredText) {
    handleLegacyText(doc);
    changed = true;
  }

  app.lastSnapshotMs = millis();
  if (displaySleeping && changed && (app.running > 0 || app.waiting > 0)) {
    wakeDisplay();
  }
  clampScroll();
  if (changed) {
    needsRedraw = true;
  }
}

void handleCommand(JsonDocument& doc) {
  const String command = doc["cmd"].as<String>();
  if (command == "status") {
    sendStatusAck();
    return;
  }
  if (command == "owner") {
    sendAck("owner", true);
    return;
  }
  if (command == "name") {
    const String requested = fitText(doc["name"].as<String>(), 24);
    if (!requested.isEmpty()) {
      app.deviceName = requested;
    }
    sendAck("name", true);
    needsRedraw = true;
    return;
  }
  if (command == "unpair") {
    sendAck("unpair", true);
    return;
  }

  sendAck(command.c_str(), false, "unsupported");
}

void handleLine(const String& line) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, line);
  if (error) {
    setCurrentStatus("System", "error", "JSON error");
    return;
  }

  if (doc["cmd"].is<const char*>()) {
    handleCommand(doc);
    return;
  }

  if (doc["time"].is<JsonArray>()) {
    setCurrentStatus("System", "time", "Time synced");
    return;
  }

  handleSnapshot(doc);
}

void processRx(const std::string& value) {
  for (char ch : value) {
    if (ch == '\n') {
      const String line = rxBuffer;
      rxBuffer = "";
      if (!line.isEmpty()) {
        handleLine(line);
      }
    } else if (rxBuffer.length() < RX_BUFFER_LIMIT) {
      rxBuffer += ch;
    } else {
      rxBuffer = "";
      setCurrentStatus("System", "error", "RX overflow");
    }
  }
}

void applyBlePowerPolicy();
void requestBleConnectionTuning();

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server) override {
    bleServer = server;
    wakeDisplay();
    bleConnected = true;
    requestBleConnectionTuning();
    setCurrentStatus("System", "connected", "BLE connected");
    requestCue(CUE_CONNECTED);
  }

  void onDisconnect(NimBLEServer*) override {
    wakeDisplay();
    bleConnected = false;
    bleConnParamsPending = false;
    setCurrentStatus("System", "disconnected", "BLE disconnected");
    requestCue(CUE_DISCONNECTED);
    delay(80);
    applyBlePowerPolicy();
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    processRx(characteristic->getValue());
  }
};

uint16_t advertisingMinInterval() {
  switch (effectivePowerMode()) {
    case POWER_TRAVEL:
      return 3200;  // 2.0s while waiting for a last connection before shutdown.
    case POWER_MAX:
      return 2400;  // 1.5s in 0.625ms units.
    case POWER_SAVER:
      return 1600;  // 1.0s.
    case POWER_BALANCED:
    default:
      return 800;   // 0.5s.
  }
}

uint16_t advertisingMaxInterval() {
  switch (effectivePowerMode()) {
    case POWER_TRAVEL:
      return 4800;  // 3.0s.
    case POWER_MAX:
      return 3200;  // 2.0s.
    case POWER_SAVER:
      return 2400;  // 1.5s.
    case POWER_BALANCED:
    default:
      return 1200;  // 0.75s.
  }
}

void connectionParamsForPower(uint16_t& minInterval, uint16_t& maxInterval, uint16_t& latency, uint16_t& timeout) {
  switch (effectivePowerMode()) {
    case POWER_TRAVEL:
      minInterval = 240;  // 300ms in 1.25ms units.
      maxInterval = 400;  // 500ms.
      latency = 10;
      timeout = 800;      // 8s in 10ms units.
      break;
    case POWER_MAX:
      minInterval = 160;  // 200ms in 1.25ms units.
      maxInterval = 320;  // 400ms.
      latency = 8;
      timeout = 700;      // 7s in 10ms units.
      break;
    case POWER_SAVER:
      minInterval = 80;   // 100ms.
      maxInterval = 160;  // 200ms.
      latency = 4;
      timeout = 600;      // 6s.
      break;
    case POWER_BALANCED:
    default:
      minInterval = 36;   // 45ms.
      maxInterval = 96;   // 120ms.
      latency = 2;
      timeout = 500;      // 5s.
      break;
  }
}

esp_power_level_t bleTxPowerLevel() {
  switch (effectivePowerMode()) {
    case POWER_TRAVEL:
    case POWER_MAX:
      return ESP_PWR_LVL_N9;
    case POWER_SAVER:
      return ESP_PWR_LVL_N6;
    case POWER_BALANCED:
    default:
      return ESP_PWR_LVL_N3;
  }
}

void applyBlePowerPolicy() {
  NimBLEDevice::setPower(bleTxPowerLevel(), ESP_BLE_PWR_TYPE_DEFAULT);
  NimBLEDevice::setPower(bleTxPowerLevel(), ESP_BLE_PWR_TYPE_ADV);
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setMinInterval(advertisingMinInterval());
  advertising->setMaxInterval(advertisingMaxInterval());
}

void requestBleConnectionTuning() {
  bleConnParamsPending = true;
  bleTuneRequestedMs = millis();
}

void handleBleConnectionTuning() {
  if (!bleConnParamsPending || !bleConnected || bleServer == nullptr) {
    return;
  }
  if (millis() - bleTuneRequestedMs < BLE_CONN_TUNE_DELAY_MS) {
    return;
  }

  std::vector<uint16_t> peers = bleServer->getPeerDevices();
  if (peers.empty()) {
    return;
  }

  uint16_t minInterval = 0;
  uint16_t maxInterval = 0;
  uint16_t latency = 0;
  uint16_t timeout = 0;
  connectionParamsForPower(minInterval, maxInterval, latency, timeout);
  bleServer->updateConnParams(peers[0], minInterval, maxInterval, latency, timeout);
  bleConnParamsPending = false;
}

void handlePowerPolicyChange() {
  static bool initialized = false;
  static PowerMode lastMode = POWER_BALANCED;
  const PowerMode mode = effectivePowerMode();
  if (initialized && mode == lastMode) {
    return;
  }

  initialized = true;
  lastMode = mode;
  applyBrightness();
  applyBlePowerPolicy();
  requestBleConnectionTuning();
  setCpuMhzIfNeeded(displaySleeping ? sleepCpuMhz() : activeCpuMhz());
  enforceUnusedPowerRails(true);
  needsRedraw = true;
}

void setupBle() {
  NimBLEDevice::init(app.deviceName.c_str());
  applyBlePowerPolicy();
  NimBLEServer* server = NimBLEDevice::createServer();
  bleServer = server;
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(NUS_SERVICE_UUID);
  txCharacteristic = service->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );
  NimBLECharacteristic* rxCharacteristic = service->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  rxCharacteristic->setCallbacks(new RxCallbacks());
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(NUS_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinInterval(advertisingMinInterval());
  advertising->setMaxInterval(advertisingMaxInterval());
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  NimBLEDevice::startAdvertising();
}

String formatCompact(uint64_t value) {
  char buffer[18];
  if (value >= 1000000000ULL) {
    snprintf(buffer, sizeof(buffer), "%.1fB", static_cast<double>(value) / 1000000000.0);
  } else if (value >= 1000000ULL) {
    snprintf(buffer, sizeof(buffer), "%.1fM", static_cast<double>(value) / 1000000.0);
  } else if (value >= 1000ULL) {
    snprintf(buffer, sizeof(buffer), "%.1fK", static_cast<double>(value) / 1000.0);
  } else {
    snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  }
  String result(buffer);
  result.replace(".0K", "K");
  result.replace(".0M", "M");
  result.replace(".0B", "B");
  return result;
}

String tokenLabel() {
  return app.hasTokens ? formatCompact(app.tokens) : "n/a";
}

void drawTextAt(int x, int y, const String& text, uint16_t color = TFT_WHITE, uint16_t bg = TFT_BLACK) {
  canvas.setTextColor(color, bg);
  canvas.setCursor(x, y);
  canvas.print(text);
}

String fitTextPixels(String text, int maxWidth) {
  text = cleanText(text);
  if (canvas.textWidth(text.c_str()) <= maxWidth) {
    return text;
  }

  String result = text;
  while (!result.isEmpty()) {
    const int len = static_cast<int>(result.length());
    int start = len - 1;
    while (start > 0 && (static_cast<uint8_t>(result.charAt(start)) & 0xC0) == 0x80) {
      start -= 1;
    }
    result = result.substring(0, start);
    String candidate = result;
    candidate.trim();
    candidate += "...";
    if (canvas.textWidth(candidate.c_str()) <= maxWidth) {
      return candidate;
    }
  }
  return "...";
}

uint16_t speakerColor(const String& speaker) {
  if (speaker.indexOf("Tool") >= 0) {
    return rgb(214, 158, 74);
  }
  if (speaker.indexOf("User") >= 0) {
    return rgb(112, 190, 122);
  }
  if (speaker.indexOf("System") >= 0) {
    return rgb(150, 135, 210);
  }
  if (speaker.indexOf("Subagent") >= 0) {
    return rgb(197, 126, 191);
  }
  return rgb(91, 190, 216);
}

bool isHeaderLine(const String& line) {
  return line.startsWith("[") && line.indexOf("]") > 0;
}

uint16_t bodyLineColor(const String& line) {
  if (!isHeaderLine(line)) {
    return rgb(176, 176, 168);
  }
  return speakerColor(line);
}

void drawBar(int x, int y, int w, int h, int value, uint16_t color) {
  canvas.drawRect(x, y, w, h, rgb(58, 61, 64));
  canvas.fillRect(x + 1, y + 1, w - 2, h - 2, rgb(18, 20, 22));
  if (value >= 0) {
    const int fill = map(constrain(value, 0, 100), 0, 100, 0, w - 2);
    canvas.fillRect(x + 1, y + 1, fill, h - 2, color);
  }
}

void drawLimitRow(int y, const RateLimitWindowState& window, const char* fallbackLabel) {
  const String label = window.available ? window.label : fallbackLabel;
  const int pct = window.available ? window.remainingPct : app.remainingPct;
  const uint16_t color = limitColor(pct);
  useUiFont();
  drawTextAt(4, y, fitTextPixels(label, 18), rgb(168, 172, 170));
  drawTextAt(25, y, pct >= 0 ? String(pct) + "%" : "--%", color);
  drawBar(64, y + 5, 66, 8, pct, limitColor(pct));
}

void drawStatusIndicator(StatusMode mode) {
  const int x = 1;
  const int y = 3;
  const uint32_t frame = millis() / statusAnimationInterval(mode);

  auto drawCell = [&](uint8_t col, uint8_t row, uint16_t color) {
    canvas.fillRect(x + col * 4, y + row * 4, 3, 3, color);
  };

  auto drawMatrix = [&](const uint8_t* levels, uint16_t offColor, uint16_t dimColor, uint16_t brightColor) {
    for (uint8_t row = 0; row < 3; ++row) {
      for (uint8_t col = 0; col < 3; ++col) {
        const uint8_t level = levels[row * 3 + col];
        drawCell(col, row, level >= 2 ? brightColor : (level == 1 ? dimColor : offColor));
      }
    }
  };

  switch (mode) {
    case MODE_OFF: {
      static const uint8_t levels[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
      drawMatrix(levels, TFT_BLACK, TFT_BLACK, TFT_BLACK);
      break;
    }
    case MODE_IDLE: {
      static const uint8_t levels[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
      drawMatrix(levels, rgb(42, 46, 50), rgb(112, 118, 122), rgb(112, 118, 122));
      break;
    }
    case MODE_WORK: {
      static const uint8_t frames[12][9] = {
        {2, 2, 0, 2, 1, 0, 0, 0, 0},
        {1, 2, 1, 2, 1, 0, 0, 0, 0},
        {0, 2, 2, 1, 1, 1, 0, 0, 0},
        {0, 2, 2, 0, 1, 2, 0, 0, 0},
        {0, 1, 2, 0, 1, 2, 0, 0, 1},
        {0, 0, 1, 0, 1, 2, 0, 1, 2},
        {0, 0, 0, 0, 1, 2, 0, 2, 2},
        {0, 0, 0, 0, 1, 1, 1, 2, 2},
        {0, 0, 0, 1, 1, 1, 2, 2, 0},
        {0, 0, 0, 2, 1, 0, 2, 2, 0},
        {1, 0, 0, 2, 1, 0, 2, 1, 0},
        {2, 1, 0, 2, 1, 0, 1, 0, 0}
      };
      drawMatrix(frames[frame % 12], rgb(18, 68, 78), rgb(58, 132, 144), rgb(122, 214, 218));
      break;
    }
    case MODE_WAIT: {
      static const uint8_t frames[4][9] = {
        {2, 2, 2, 0, 0, 0, 0, 0, 0},
        {1, 1, 1, 2, 2, 2, 0, 0, 0},
        {0, 0, 0, 1, 1, 1, 2, 2, 2},
        {1, 1, 1, 2, 2, 2, 1, 1, 1}
      };
      drawMatrix(frames[(frame / 2) % 4], rgb(78, 52, 28), rgb(142, 96, 46), rgb(224, 166, 78));
      break;
    }
    case MODE_STALE: {
      static const uint8_t frames[4][9] = {
        {2, 0, 0, 0, 1, 0, 0, 0, 1},
        {1, 2, 0, 0, 1, 0, 0, 1, 0},
        {0, 1, 2, 0, 1, 0, 1, 0, 0},
        {0, 0, 1, 0, 1, 0, 2, 1, 0}
      };
      drawMatrix(frames[(frame / 2) % 4], rgb(44, 40, 62), rgb(86, 78, 112), rgb(154, 139, 196));
      break;
    }
    case MODE_ERR: {
      static const uint8_t frames[2][9] = {
        {2, 2, 2, 2, 0, 2, 2, 2, 2},
        {1, 1, 1, 1, 0, 1, 1, 1, 1}
      };
      drawMatrix(frames[(frame / 2) % 2], rgb(64, 20, 26), rgb(126, 38, 48), rgb(230, 78, 92));
      break;
    }
  }

}

void drawTopBar() {
  useUiFont();
  const StatusMode mode = currentStatusMode();
  const uint16_t color = statusColor(mode);
  canvas.fillRect(0, 0, canvas.width(), TOP_BAR_HEIGHT, color);
  drawStatusIndicator(mode);
  drawTextAt(16, 1, fitTextPixels(modeLabel(mode), 36), TFT_WHITE, color);

  const int pct = batteryPercent();
  char battery[5];
  if (pct >= 0) {
    snprintf(battery, sizeof(battery), "%d%%", constrain(pct, 0, 100));
  } else {
    snprintf(battery, sizeof(battery), "--%");
  }
  const uint16_t active = rgb(235, 239, 235);
  const int batteryX = canvas.width() - canvas.textWidth(battery) - 2;
  if (app.hasNew && app.scrollOffset > 0) {
    canvas.fillCircle(53, TOP_BAR_HEIGHT / 2, 3, rgb(255, 196, 78));
  }
  drawTextAt(62, 1, isUsbPowered() ? "USB" : "   ", active, color);
  drawTextAt(87, 1, bleConnected ? "BLE" : "   ", active, color);
  drawTextAt(batteryX, 1, battery, active, color);
}

void drawDashboard() {
  canvas.fillScreen(TFT_BLACK);
  useUiFont();
  drawTopBar();

  drawLimitRow(21, app.primaryLimit, "5h");
  drawLimitRow(37, app.secondaryLimit, "7d");
  useUiFont();
  drawTextAt(4, 54, fitTextPixels("TOK " + tokenLabel(), canvas.width() - 8), TFT_LIGHTGREY);

  const String currentRaw = app.statusSpeaker + ": " + app.statusText;
  useBodyFontFor(currentRaw);
  const String current = fitTextPixels(currentRaw, canvas.width() - 8);
  drawTextAt(4, 70, current, speakerColor(app.statusSpeaker));
  useUiFont();
  canvas.drawFastHLine(4, 88, canvas.width() - 8, rgb(42, 42, 42));

  const uint8_t visible = visibleBodyLines();
  const uint16_t maxOffset = maxScrollOffset();
  app.scrollOffset = min<uint16_t>(app.scrollOffset, maxOffset);
  const size_t latestStart = app.bodyCount > visible ? app.bodyCount - visible : 0;
  const size_t start = latestStart >= app.scrollOffset ? latestStart - app.scrollOffset : 0;
  uint8_t y = bodyTopY();
  for (uint8_t i = 0; i < visible; ++i) {
    const size_t lineIndex = start + i;
    if (lineIndex >= app.bodyCount) {
      break;
    }
    const String line = bodyLineAt(lineIndex);
    if (isHeaderLine(line)) {
      canvas.fillRect(0, y - 1, canvas.width(), bodyLineHeight(), rgb(17, 21, 24));
    }
    const bool header = isHeaderLine(line);
    useBodyFontFor(line);
    drawTextAt(
      4,
      y,
      header ? fitTextPixels(line, canvas.width() - 8) : line,
      bodyLineColor(line),
      header ? rgb(17, 21, 24) : TFT_BLACK
    );
    y += bodyLineHeight();
  }
  useUiFont();

}

const char* brightnessName() {
  static const char* names[] = {"Low", "Med", "High"};
  return names[constrain(app.settings.brightness, 0, 2)];
}

const char* powerName() {
  switch (app.settings.power) {
    case POWER_TRAVEL:
      return "Travel";
    case POWER_MAX:
      return "Max";
    case POWER_SAVER:
      return "Saver";
    case POWER_BALANCED:
    default:
      return "Balanced";
  }
}

const char* soundName() {
  if (app.settings.sound == SOUND_OFF) {
    return "Off";
  }
  if (app.settings.sound == SOUND_ALERTS) {
    return "Alerts";
  }
  return "Soft";
}

const char* navName() {
  return app.settings.textNav == TEXT_NAV_LINE ? "Line" : "Page";
}

String settingLabel(uint8_t index) {
  switch (index) {
    case SETTING_BRIGHTNESS:
      return "Brightness " + String(brightnessName());
    case SETTING_POWER:
      return String("Power ") + powerName() + (lowBatteryPowerSaveActive() ? ">Max" : "");
    case SETTING_SOUND:
      return "Sound " + String(soundName());
    case SETTING_TEXT_NAV:
      return "Text nav " + String(navName());
    case SETTING_AUTO_NEWEST:
      return String("Auto newest ") + (app.settings.autoNewest ? "On" : "Off");
    default:
      return "";
  }
}

void drawSettings() {
  canvas.fillScreen(TFT_BLACK);
  useUiFont();
  drawTopBar();
  drawTextAt(4, 22, "SETTINGS", rgb(210, 214, 210));

  for (uint8_t i = 0; i < SETTING_COUNT; ++i) {
    const uint8_t y = 48 + i * 20;
    const bool selected = i == app.settings.selected;
    const uint16_t fg = selected ? TFT_BLACK : TFT_LIGHTGREY;
    const uint16_t bg = selected ? rgb(118, 166, 154) : TFT_BLACK;
    if (selected) {
      canvas.fillRect(2, y - 2, canvas.width() - 4, DASHBOARD_FONT_HEIGHT + 2, bg);
    }
    drawTextAt(6, y, fitTextPixels(settingLabel(i), canvas.width() - 12), fg, bg);
  }

  const int pct = batteryPercent();
  String batteryLine = "BLE ";
  batteryLine += bleConnected ? "on" : "off";
  batteryLine += " BAT ";
  batteryLine += pct >= 0 ? String(pct) + "%" : "n/a";
  if (powerTelemetry.voltageMv > 0) {
    batteryLine += " ";
    batteryLine += String(powerTelemetry.voltageMv);
    batteryLine += "mV";
  }
  if (powerTelemetry.hasCurrent) {
    batteryLine += " ";
    batteryLine += String(powerTelemetry.currentMa);
    batteryLine += "mA";
  }
  drawTextAt(4, 166, fitTextPixels(app.deviceName, canvas.width() - 8), rgb(100, 104, 108));
  drawTextAt(4, 183, fitTextPixels(batteryLine, canvas.width() - 8), rgb(100, 104, 108));
  drawTextAt(4, 200, fitTextPixels("Heap " + formatCompact(ESP.getFreeHeap()), canvas.width() - 8), rgb(100, 104, 108));
  drawTextAt(4, canvas.height() - DASHBOARD_FONT_HEIGHT, fitTextPixels("A change  B next", canvas.width() - 8), rgb(100, 104, 108));
}

void redraw() {
  if (displaySleeping) {
    needsRedraw = false;
    return;
  }
  if (app.settings.open) {
    drawSettings();
  } else {
    drawDashboard();
  }
  canvas.pushSprite(0, 0);
  needsRedraw = false;
}

void scrollNewer() {
  const uint16_t step = app.settings.textNav == TEXT_NAV_LINE ? 1 : visibleBodyLines();
  app.scrollOffset = app.scrollOffset > step ? app.scrollOffset - step : 0;
  if (app.scrollOffset == 0) {
    app.hasNew = false;
  }
  needsRedraw = true;
}

void scrollOlder() {
  const uint16_t step = app.settings.textNav == TEXT_NAV_LINE ? 1 : visibleBodyLines();
  app.scrollOffset = min<uint16_t>(maxScrollOffset(), app.scrollOffset + step);
  needsRedraw = true;
}

void jumpNewest() {
  app.scrollOffset = 0;
  app.hasNew = false;
  needsRedraw = true;
}

void touchSettings() {
  app.settings.lastInputMs = millis();
}

void openSettings() {
  app.settings.open = true;
  touchSettings();
  needsRedraw = true;
}

void closeSettings() {
  app.settings.open = false;
  saveSettings();
  needsRedraw = true;
}

void rotateSetting() {
  touchSettings();
  switch (app.settings.selected) {
    case SETTING_BRIGHTNESS:
      app.settings.brightness = (app.settings.brightness + 1) % 3;
      applyBrightness();
      break;
    case SETTING_POWER:
      app.settings.power = static_cast<PowerMode>((app.settings.power + 1) % 4);
      if (!isUsbPowered() && app.settings.power != POWER_BALANCED && app.settings.brightness > 1) {
        app.settings.brightness = 1;
      }
      applyBrightness();
      applyBlePowerPolicy();
      requestBleConnectionTuning();
      setCpuMhzIfNeeded(displaySleeping ? sleepCpuMhz() : activeCpuMhz());
      break;
    case SETTING_SOUND:
      app.settings.sound = static_cast<SoundMode>((app.settings.sound + 1) % 3);
      requestCue(app.settings.sound == SOUND_OFF ? CUE_NONE : CUE_ACTIVITY);
      break;
    case SETTING_TEXT_NAV:
      app.settings.textNav = app.settings.textNav == TEXT_NAV_PAGE ? TEXT_NAV_LINE : TEXT_NAV_PAGE;
      clampScroll();
      break;
    case SETTING_AUTO_NEWEST:
      app.settings.autoNewest = !app.settings.autoNewest;
      break;
    default:
      break;
  }
  saveSettings();
  needsRedraw = true;
}

void nextSetting() {
  touchSettings();
  app.settings.selected = (app.settings.selected + 1) % SETTING_COUNT;
  needsRedraw = true;
}

void dispatchInput(InputEvent event) {
  if (event == INPUT_NONE) {
    return;
  }
  if (displaySleeping) {
    wakeDisplay();
    return;
  }
  autoSleepEligibleSinceMs = 0;

  if (app.settings.open) {
    if (event == INPUT_A_LONG) {
      closeSettings();
    } else if (event == INPUT_A_SINGLE) {
      rotateSetting();
    } else if (event == INPUT_B_SINGLE) {
      nextSetting();
    }
    return;
  }

  switch (event) {
    case INPUT_A_SINGLE:
      scrollNewer();
      break;
    case INPUT_B_SINGLE:
      scrollOlder();
      break;
    case INPUT_A_LONG:
      openSettings();
      break;
    case INPUT_B_LONG:
      if (app.scrollOffset > 0 && app.hasNew) {
        jumpNewest();
      } else {
        enterDisplaySleep();
      }
      break;
    default:
      break;
  }
}

void updateButtonTracker(ButtonTracker& tracker, bool pressed, InputEvent singleEvent, InputEvent longEvent, bool suppressIndividual) {
  const uint32_t now = millis();
  if (pressed && !tracker.down) {
    tracker.down = true;
    tracker.longSent = false;
    tracker.pressMs = now;
  }

  if (pressed && tracker.down && !tracker.longSent && now - tracker.pressMs >= LONG_PRESS_MS && !suppressIndividual) {
    tracker.longSent = true;
    dispatchInput(longEvent);
  }

  if (!pressed && tracker.down) {
    tracker.down = false;
    if (!tracker.longSent && !suppressIndividual) {
      dispatchInput(singleEvent);
    }
  }
}

void handleButtons() {
  const bool aPressed = M5.BtnA.isPressed();
  const bool bPressed = M5.BtnB.isPressed();
  const bool bothPressed = aPressed && bPressed;

  if (bothPressed) {
    comboActive = true;
  }

  updateButtonTracker(buttonA, aPressed, INPUT_A_SINGLE, INPUT_A_LONG, comboActive);
  updateButtonTracker(buttonB, bPressed, INPUT_B_SINGLE, INPUT_B_LONG, comboActive);

  if (!aPressed && !bPressed) {
    comboActive = false;
  }
}

void checkShakeWake() {
  if (!displaySleeping || millis() - lastShakeCheckMs < shakeCheckMs()) {
    return;
  }
  if (millis() - sleepEnteredMs < SHAKE_WAKE_ARM_DELAY_MS) {
    return;
  }
  lastShakeCheckMs = millis();

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) {
    return;
  }
  if (!accelPrimed) {
    lastAx = ax;
    lastAy = ay;
    lastAz = az;
    accelPrimed = true;
    return;
  }

  const float delta = fabsf(ax - lastAx) + fabsf(ay - lastAy) + fabsf(az - lastAz);
  lastAx = ax;
  lastAy = ay;
  lastAz = az;
  if (delta >= SHAKE_WAKE_DELTA) {
    wakeDisplay();
  }
}

void prepareTravelWakeSources() {
  if (!pmicReady) {
    return;
  }

  pm1.disableLeds();
  pm1.setLedEnLevel(false);
  pm1.setBoostEnable(false);
  pm1.boostSetPowerHold(false);
  pm1.gpioSetPowerHold(M5PM1_GPIO_NUM_3, false);

  // Keep the IMU rail available for the StickS3 PMIC wake chain and arm GPIO4,
  // which is wired to the IMU interrupt path on StickS3-class hardware.
  pm1.ldoSetPowerHold(true);
  pm1.gpioSetFunc(M5PM1_GPIO_NUM_4, M5PM1_GPIO_FUNC_WAKE);
  pm1.gpioSetWakeEdge(M5PM1_GPIO_NUM_4, M5PM1_GPIO_WAKE_RISING);
  pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_4, true);
  pm1.clearWakeSource(0x7F);
}

void enterTravelShutdown() {
  if (travelShutdownStarted) {
    return;
  }
  travelShutdownStarted = true;
  app.settings.open = false;
  saveSettings();
  shutdownSpeakerOutput();
  enforceUnusedPowerRails(true);
  prepareTravelWakeSources();
  M5.Display.sleep();
  delay(120);
  if (pmicReady) {
    pm1.shutdown();
    delay(500);
  }
  M5.Power.powerOff();
  delay(200);
  M5.Power.deepSleep(0, true);
}

void enterDeepSleep() {
  app.settings.open = false;
  saveSettings();
  shutdownSpeakerOutput();
  enforceUnusedPowerRails(true);
  M5.Display.sleep();
  delay(20);
  M5.Power.deepSleep(0, true);
}

void handleStatusAnimation() {
  static uint32_t lastAnimationMs = 0;
  const StatusMode mode = currentStatusMode();
  if (displaySleeping || !statusModeAnimates(mode)) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastAnimationMs >= statusAnimationInterval(mode)) {
    lastAnimationMs = now;
    needsRedraw = true;
  }
}

void handleSleepTimers() {
  if (displaySleeping) {
    checkShakeWake();
    const uint32_t travelMs = travelShutdownAfterMs();
    if (travelMs > 0 && !isUsbPowered() && app.running == 0 && app.waiting == 0 && !app.hasNew && millis() - sleepEnteredMs > travelMs) {
      enterTravelShutdown();
    }
    const uint32_t deepMs = deepSleepAfterMs();
    if (deepMs > 0 && !isUsbPowered() && app.running == 0 && app.waiting == 0 && !app.hasNew && millis() - sleepEnteredMs > deepMs) {
      enterDeepSleep();
    }
    return;
  }

  const bool canAutoSleep = !app.settings.open && app.scrollOffset == 0 && !app.hasNew;
  if (!canAutoSleep) {
    autoSleepEligibleSinceMs = 0;
    return;
  }

  if (autoSleepEligibleSinceMs == 0) {
    autoSleepEligibleSinceMs = millis();
    return;
  }
  if (millis() - autoSleepEligibleSinceMs > autoSleepAfterMs()) {
    enterDisplaySleep();
  }
}

uint32_t loopDelayMs() {
  if (displaySleeping) {
    return effectivePowerMode() == POWER_BALANCED ? 160 : 280;
  }
  if (app.settings.open || pendingCue != CUE_NONE || speakerOutputActive) {
    return 20;
  }
  if (currentStatusMode() == MODE_WORK) {
    return effectivePowerMode() == POWER_BALANCED ? 50 : 120;
  }
  return effectivePowerMode() == POWER_BALANCED ? 60 : 140;
}

void seedBody() {
  uint8_t added = appendActivityBlock("System", "boot", "Dashboard booted. Waiting for Codex Desktop observer.");
  applyNewLines(added);
}

}  // namespace

void setup() {
  Serial.begin(115200);

  setCpuMhzIfNeeded(activeCpuMhz());
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  cfg.led_brightness = 0;
  M5.begin(cfg);
  M5.Power.setLed(0);
  setupPmicPowerSaving();
  setupSpeakerOutput();
  M5.Display.setRotation(0);
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  useUiFont();

  prefs.begin("codexdash", false);
  samplePowerTelemetry(true);
  loadSettings();
  applyBrightness();
  setCpuMhzIfNeeded(activeCpuMhz());

  app.deviceName = "Codex-S3-" + chipSuffix();
  setupBle();
  setCurrentStatus("System", "advertise", "Advertise " + app.deviceName);
  seedBody();
  redraw();
}

void loop() {
  M5.update();
  handleButtons();
  handleSleepTimers();
  handleStatusAnimation();
  handlePowerPolicyChange();
  handleBleConnectionTuning();
  playPendingCue();
  handleSpeakerPower();

  if (app.settings.open && millis() - app.settings.lastInputMs > SETTINGS_IDLE_MS) {
    closeSettings();
  }

  if (samplePowerTelemetry(false)) {
    needsRedraw = true;
    handlePowerPolicyChange();
  }

  enforceUnusedPowerRails(false);

  if (needsRedraw) {
    redraw();
  }

  delay(loopDelayMs());
}
