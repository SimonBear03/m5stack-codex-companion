#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#if defined(CODEX_COMPANION_CARDPUTER)
#include <M5Cardputer.h>
#else
#include <M5PM1.h>
#endif
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <ctype.h>
#include <math.h>
#include <mbedtls/md.h>

namespace {

constexpr const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

#if defined(CODEX_COMPANION_CARDPUTER)
constexpr bool BOARD_HAS_IMU_ORIENTATION = false;
constexpr uint8_t BOARD_DEFAULT_ROTATION = 1;
constexpr const char* BOARD_DEVICE_PREFIX = "Codex-CP-";
constexpr const char* BOARD_INPUT_HINT = "Enter set  Arrows nav";
constexpr const char* BOARD_TYPE = "cardputer_adv";
#else
constexpr bool BOARD_HAS_IMU_ORIENTATION = true;
constexpr uint8_t BOARD_DEFAULT_ROTATION = 0;
constexpr const char* BOARD_DEVICE_PREFIX = "Codex-S3-";
constexpr const char* BOARD_INPUT_HINT = "A change  B next";
constexpr const char* BOARD_TYPE = "sticks3";
#endif

constexpr uint32_t STALE_AFTER_MS = 90000;
constexpr uint32_t BLE_NOTIFY_CHUNK_DELAY_MS = 8;
constexpr uint32_t LONG_PRESS_MS = 650;
constexpr uint32_t PAIRING_TIMEOUT_MS = 60000;
constexpr uint32_t SETTINGS_IDLE_MS = 12000;
constexpr uint32_t ACTIVITY_SOUND_COOLDOWN_MS = 1400;
constexpr uint32_t SHAKE_WAKE_ARM_DELAY_MS = 2500;
constexpr float SHAKE_WAKE_DELTA = 0.85f;
constexpr uint8_t DASHBOARD_FONT_HEIGHT = 14;
constexpr uint8_t DASHBOARD_LINE_HEIGHT = 15;
constexpr uint8_t TOP_BAR_HEIGHT = 17;
constexpr uint32_t STATUS_ANIMATION_MS = 420;
constexpr uint32_t WORK_ANIMATION_MS = 260;
constexpr uint32_t POWER_TELEMETRY_MS = 5000;
constexpr uint32_t SPEAKER_IDLE_OFF_MS = 180;
constexpr uint32_t BLE_CONN_TUNE_DELAY_MS = 800;
constexpr uint32_t BLE_ADVERTISING_ENSURE_MS = 10000;
constexpr uint32_t PERIPHERAL_ENFORCE_MS = 30000;
constexpr uint32_t ORIENTATION_CHECK_MS = 250;
constexpr uint32_t ORIENTATION_STABLE_MS = 700;
constexpr float ORIENTATION_MIN_G = 0.45f;
constexpr float ORIENTATION_AXIS_MARGIN = 0.08f;
constexpr uint8_t LOW_BATTERY_POWER_LOW_PCT = 20;
constexpr size_t BODY_LINE_COUNT = 190;
constexpr size_t RAW_ACTIVITY_COUNT = 8;
constexpr size_t SEEN_SEQ_COUNT = 24;
constexpr size_t BLE_NOTIFY_CHUNK_SIZE = 20;
constexpr size_t RX_BUFFER_LIMIT = 8192;
constexpr size_t PAIR_SECRET_HEX_LENGTH = 64;

enum InputAction : uint8_t {
  ACTION_NONE = 0,
  ACTION_SCROLL_NEWER,
  ACTION_SCROLL_OLDER,
  ACTION_SCROLL_NEWER_LINE,
  ACTION_SCROLL_OLDER_LINE,
  ACTION_SCROLL_NEWER_PAGE,
  ACTION_SCROLL_OLDER_PAGE,
  ACTION_OPEN_SETTINGS,
  ACTION_CLOSE_SETTINGS,
  ACTION_NEXT_SETTING,
  ACTION_PREVIOUS_SETTING,
  ACTION_NEXT_VALUE,
  ACTION_PREVIOUS_VALUE,
  ACTION_CONFIRM,
  ACTION_BACK,
  ACTION_GO,
  ACTION_SLEEP,
  ACTION_JUMP_OR_SLEEP
};

enum PressEvent : uint8_t {
  PRESS_NONE = 0,
  PRESS_SINGLE,
  PRESS_LONG
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

enum DetailMode : uint8_t {
  DETAIL_FULL = 0,
  DETAIL_STATUS,
  DETAIL_USAGE,
  DETAIL_COUNT
};

enum SettingIndex : uint8_t {
  SETTING_BRIGHTNESS = 0,
  SETTING_POWER,
  SETTING_DETAIL,
  SETTING_SOUND,
  SETTING_TEXT_NAV,
  SETTING_AUTO_NEWEST,
  SETTING_ROTATION,
  SETTING_COUNT
};

enum RotationMode : uint8_t {
  ROTATION_AUTO = 0,
  ROTATION_LOCK,
  ROTATION_PORTRAIT,
  ROTATION_LANDSCAPE,
  ROTATION_PORTRAIT_180,
  ROTATION_LANDSCAPE_180
};

enum PowerMode : uint8_t {
  POWER_ALWAYS = 0,
  POWER_AUTO,
  POWER_LOW,
  POWER_COUNT
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
  MODE_SYNC,
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
  PowerMode power = POWER_ALWAYS;
  DetailMode detail = DETAIL_FULL;
  SoundMode sound = SOUND_SOFT;
  TextNavMode textNav = TEXT_NAV_PAGE;
  bool autoNewest = true;
  RotationMode rotation = ROTATION_AUTO;
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
  int vbusMv = -1;
  int currentMa = 0;
  bool hasCurrent = false;
  bool usb = false;
  bool charging = false;
  bool chargeKnown = false;
  uint32_t sampledMs = 0;
};

struct SecurityState {
  String deviceId;
  String hostId;
  String secretHex;
  String sessionNonce;
  bool sessionAuthorized = false;
  bool pairingActive = false;
  bool pairingConfirmed = false;
  String pairingCode;
  String pendingHostId;
  String pendingSecretHex;
  uint32_t pairingStartedMs = 0;
};

struct AppState {
  String deviceName;
  uint16_t total = 0;
  uint16_t running = 0;
  uint16_t waiting = 0;
  String statusSpeaker = "System";
  String statusKind = "idle";
  String statusText = "Waiting for Codex";
  String activityStatus = "idle";
  String activityTitle = "Codex";
  String activitySubtitle = "Waiting for Codex";
  String activityWaitingKind;
  bool hasCodexActivity = false;
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
#if !defined(CODEX_COMPANION_CARDPUTER)
M5PM1 pm1;
#endif
PowerTelemetry powerTelemetry;
SecurityState security;
ButtonTracker buttonA;
ButtonTracker buttonB;
#if defined(CODEX_COMPANION_CARDPUTER)
ButtonTracker keyUp;
ButtonTracker keyDown;
ButtonTracker keyLeft;
ButtonTracker keyRight;
ButtonTracker keyEnter;
ButtonTracker keyBack;
ButtonTracker keyGo;
#endif
AppState app;
String rxBuffer;
bool bleConnected = false;
bool awaitingSnapshot = false;
bool needsRedraw = true;
bool comboActive = false;
Cue pendingCue = CUE_NONE;
uint32_t lastActivityCueMs = 0;
uint32_t autoSleepEligibleSinceMs = 0;
uint32_t lastShakeCheckMs = 0;
uint32_t sleepEnteredMs = 0;
uint32_t speakerOffAtMs = 0;
uint32_t bleTuneRequestedMs = 0;
uint32_t lastAdvertisingEnsureMs = 0;
uint32_t lastPeripheralEnforceMs = 0;
uint32_t lastOrientationCheckMs = 0;
uint32_t orientationCandidateSinceMs = 0;
uint8_t displayRotation = 0;
uint8_t orientationCandidate = 0;
bool displaySleeping = false;
bool displayDimmed = false;
bool accelPrimed = false;
bool pmicReady = false;
bool speakerOutputActive = false;
bool bleConnParamsPending = false;
bool bleAdvertisingRestartRequested = false;
float lastAx = 0.0f;
float lastAy = 0.0f;
float lastAz = 0.0f;

String chipSuffix() {
  const uint64_t chip = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned int>(chip & 0xFFFF));
  return String(suffix);
}

String randomHex(size_t bytes) {
  static const char* digits = "0123456789abcdef";
  String value;
  value.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t byte = static_cast<uint8_t>(esp_random() & 0xFF);
    value += digits[byte >> 4];
    value += digits[byte & 0x0F];
  }
  return value;
}

String randomPairingCode() {
  char code[7];
  snprintf(code, sizeof(code), "%06lu", static_cast<unsigned long>(100000 + (esp_random() % 900000)));
  return String(code);
}

bool isPaired() {
  return security.secretHex.length() == PAIR_SECRET_HEX_LENGTH && !security.hostId.isEmpty();
}

void resetAuthorizedSession() {
  security.sessionNonce = randomHex(16);
  security.sessionAuthorized = false;
}

bool hexToBytes(const String& hex, uint8_t* output, size_t outputLen) {
  if (hex.length() != outputLen * 2) {
    return false;
  }
  for (size_t i = 0; i < outputLen; ++i) {
    const char high = hex.charAt(i * 2);
    const char low = hex.charAt(i * 2 + 1);
    if (!isxdigit(high) || !isxdigit(low)) {
      return false;
    }
    char pair[3] = {high, low, '\0'};
    output[i] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
  }
  return true;
}

String bytesToHex(const uint8_t* bytes, size_t length) {
  static const char* digits = "0123456789abcdef";
  String value;
  value.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    value += digits[bytes[i] >> 4];
    value += digits[bytes[i] & 0x0F];
  }
  return value;
}

String hmacSha256Hex(const String& secretHex, const String& message) {
  uint8_t key[32];
  if (!hexToBytes(secretHex, key, sizeof(key))) {
    return "";
  }
  uint8_t digest[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) {
    return "";
  }
  const int rc = mbedtls_md_hmac(
    info,
    key,
    sizeof(key),
    reinterpret_cast<const uint8_t*>(message.c_str()),
    message.length(),
    digest
  );
  if (rc != 0) {
    return "";
  }
  return bytesToHex(digest, sizeof(digest));
}

String authMessage(const String& hostNonce, const String& hostId) {
  return "auth:v1:" + security.deviceId + ":" + security.sessionNonce + ":" + hostNonce + ":" + hostId;
}

bool constantTimeEquals(const String& left, const String& right) {
  if (left.length() != right.length()) {
    return false;
  }
  uint8_t diff = 0;
  for (int i = 0; i < static_cast<int>(left.length()); ++i) {
    diff |= static_cast<uint8_t>(left.charAt(i) ^ right.charAt(i));
  }
  return diff == 0;
}

void loadSecurityState() {
  security.deviceId = prefs.getString("dev_id", "");
  if (security.deviceId.isEmpty()) {
    security.deviceId = String(BOARD_TYPE) + "-" + chipSuffix() + "-" + randomHex(4);
    prefs.putString("dev_id", security.deviceId);
  }
  security.hostId = prefs.getString("host_id", "");
  security.secretHex = prefs.getString("secret", "");
  if (security.secretHex.length() != PAIR_SECRET_HEX_LENGTH) {
    security.hostId = "";
    security.secretHex = "";
  }
  resetAuthorizedSession();
}

void savePairing(const String& hostId, const String& secretHex) {
  security.hostId = hostId;
  security.secretHex = secretHex;
  prefs.putString("host_id", security.hostId);
  prefs.putString("secret", security.secretHex);
  security.sessionAuthorized = true;
}

void clearPairing() {
  security.hostId = "";
  security.secretHex = "";
  security.sessionAuthorized = false;
  security.pairingActive = false;
  security.pairingConfirmed = false;
  security.pendingHostId = "";
  security.pendingSecretHex = "";
  prefs.remove("host_id");
  prefs.remove("secret");
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

int batteryPercentFromVoltage(int mv) {
  if (mv <= 0) {
    return -1;
  }
  if (mv >= 4150) {
    return 100;
  }
  if (mv <= 3300) {
    return 0;
  }
  return constrain(static_cast<int>((mv - 3300) * 100 / 850), 0, 100);
}

int readBatteryVoltageMv() {
  const int16_t voltage = M5.Power.getBatteryVoltage();
  if (voltage > 0) {
    return voltage;
  }
#if !defined(CODEX_COMPANION_CARDPUTER)
  if (pmicReady) {
    uint16_t pmicVoltage = 0;
    if (pm1.readVbat(&pmicVoltage) == M5PM1_OK && pmicVoltage > 0) {
      return pmicVoltage;
    }
  }
#endif
  return -1;
}

int readVbusVoltageMv() {
  const int16_t voltage = M5.Power.getVBUSVoltage();
  if (voltage > 0) {
    return voltage;
  }
#if !defined(CODEX_COMPANION_CARDPUTER)
  if (pmicReady) {
    uint16_t pmicVoltage = 0;
    if (pm1.readVin(&pmicVoltage) == M5PM1_OK && pmicVoltage > 0) {
      return pmicVoltage;
    }
  }
#endif
  return -1;
}

int readBatteryPercent() {
  const int level = M5.Power.getBatteryLevel();
  if (level < 0 || level > 100) {
    return batteryPercentFromVoltage(readBatteryVoltageMv());
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
  const bool oldCharging = powerTelemetry.charging;
  const bool oldChargeKnown = powerTelemetry.chargeKnown;
  const int oldVoltage = powerTelemetry.voltageMv;
  const int oldVbus = powerTelemetry.vbusMv;

  powerTelemetry.pct = readBatteryPercent();
  powerTelemetry.voltageMv = readBatteryVoltageMv();
  powerTelemetry.vbusMv = readVbusVoltageMv();
  const int32_t current = M5.Power.getBatteryCurrent();
  powerTelemetry.hasCurrent = current != 0 && current > -2000 && current < 2000;
  powerTelemetry.currentMa = powerTelemetry.hasCurrent ? static_cast<int>(current) : 0;
  const auto chargeState = M5.Power.isCharging();
  const bool currentSaysCharging = powerTelemetry.hasCurrent && powerTelemetry.currentMa > 15;
  powerTelemetry.chargeKnown = chargeState != m5::Power_Class::charge_unknown || powerTelemetry.hasCurrent;
  powerTelemetry.charging = chargeState == m5::Power_Class::is_charging || currentSaysCharging;
  powerTelemetry.usb = powerTelemetry.charging || powerTelemetry.vbusMv > 4000;
  powerTelemetry.sampledMs = now;

  return oldPct != powerTelemetry.pct || oldUsb != powerTelemetry.usb || oldCharging != powerTelemetry.charging ||
         oldChargeKnown != powerTelemetry.chargeKnown || abs(oldVoltage - powerTelemetry.voltageMv) >= 50 ||
         abs(oldVbus - powerTelemetry.vbusMv) >= 50;
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

bool isBatteryCharging() {
  if (powerTelemetry.sampledMs == 0) {
    samplePowerTelemetry(true);
  }
  return powerTelemetry.chargeKnown && powerTelemetry.charging;
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
  if (app.hasCodexActivity) {
    if (app.activityStatus == "failed") {
      return MODE_ERR;
    }
    if (!bleConnected) {
      return MODE_OFF;
    }
    if (awaitingSnapshot) {
      return MODE_SYNC;
    }
    if (app.lastSnapshotMs > 0 && millis() - app.lastSnapshotMs > STALE_AFTER_MS) {
      return MODE_STALE;
    }
    if (app.activityStatus == "waiting") {
      return MODE_WAIT;
    }
    if (app.activityStatus == "running") {
      return MODE_WORK;
    }
    return MODE_IDLE;
  }
  if (app.statusKind == "error") {
    return MODE_ERR;
  }
  if (!bleConnected) {
    return MODE_OFF;
  }
  if (awaitingSnapshot) {
    return MODE_SYNC;
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
  return !isUsbPowered() && pct >= 0 && pct <= LOW_BATTERY_POWER_LOW_PCT && app.settings.power == POWER_AUTO;
}

PowerMode effectivePowerMode() {
  if (lowBatteryPowerSaveActive()) {
    return POWER_LOW;
  }
  return app.settings.power;
}

uint16_t statusColor(StatusMode mode) {
  switch (mode) {
    case MODE_ERR:
      return rgb(122, 36, 48);
    case MODE_OFF:
      return rgb(8, 9, 10);
    case MODE_SYNC:
      return rgb(34, 63, 78);
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
      return "DISC";
    case MODE_SYNC:
      return "SYNC";
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
  return mode == MODE_SYNC || mode == MODE_WORK || mode == MODE_WAIT || mode == MODE_STALE || mode == MODE_ERR;
}

uint32_t powerProfileFactor() {
  switch (effectivePowerMode()) {
    case POWER_LOW:
      return 2;
    case POWER_AUTO:
      return 1;
    case POWER_ALWAYS:
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
  return effectivePowerMode() == POWER_ALWAYS ? 0 : 10000;
}

uint32_t activeCpuMhz() {
  switch (effectivePowerMode()) {
    case POWER_LOW:
      return 80;
    case POWER_AUTO:
      return 120;
    case POWER_ALWAYS:
    default:
      return 160;
  }
}

uint32_t sleepCpuMhz() {
  return 80;
}

uint32_t shakeCheckMs() {
  switch (effectivePowerMode()) {
    case POWER_LOW:
      return 500;
    case POWER_AUTO:
      return 320;
    case POWER_ALWAYS:
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

void enforceIndicatorLedsOff(bool force = false) {
  const uint32_t now = millis();
  if (!force && lastPeripheralEnforceMs > 0 && now - lastPeripheralEnforceMs < PERIPHERAL_ENFORCE_MS) {
    return;
  }
  lastPeripheralEnforceMs = now;

  M5.Power.setLed(0);
  if (!pmicReady) {
    return;
  }
#if !defined(CODEX_COMPANION_CARDPUTER)
  pm1.disableLeds();
  pm1.setLedEnLevel(false);
#endif
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

void setupPmicPowerManagement() {
#if defined(CODEX_COMPANION_CARDPUTER)
  pmicReady = false;
  M5.Power.setBatteryCharge(true);
#else
  pmicReady = pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) == M5PM1_OK;
  M5.Power.setBatteryCharge(true);
  if (!pmicReady) {
    return;
  }
  pm1.setLedEnLevel(false);
  pm1.disableLeds();
  pm1.setChargeEnable(true);
  pm1.setI2cSleepTime(0);
  pm1.setAutoWakeEnable(false);
  enforceIndicatorLedsOff(true);
#endif
}

void loadSettings() {
  app.settings.brightness = prefs.isKey("br") ? prefs.getUChar("br", 1) : (isUsbPowered() ? 1 : 0);
  if (app.settings.brightness > 2) {
    app.settings.brightness = isUsbPowered() ? 1 : 0;
  }
  const uint8_t savedPower = prefs.isKey("pwr") ? prefs.getUChar("pwr", POWER_ALWAYS) : (isUsbPowered() ? POWER_ALWAYS : POWER_AUTO);
  app.settings.power = savedPower < POWER_COUNT ? static_cast<PowerMode>(savedPower) : (isUsbPowered() ? POWER_ALWAYS : POWER_AUTO);
  if (app.settings.power >= POWER_COUNT) {
    app.settings.power = POWER_ALWAYS;
  }
  app.settings.sound = static_cast<SoundMode>(prefs.getUChar("snd", SOUND_SOFT));
  if (app.settings.sound > SOUND_ALERTS) {
    app.settings.sound = SOUND_SOFT;
  }
  app.settings.detail = static_cast<DetailMode>(prefs.getUChar("det", DETAIL_FULL));
  if (app.settings.detail >= DETAIL_COUNT) {
    app.settings.detail = DETAIL_FULL;
  }
  app.settings.textNav = static_cast<TextNavMode>(prefs.getUChar("nav", TEXT_NAV_PAGE));
  if (app.settings.textNav > TEXT_NAV_LINE) {
    app.settings.textNav = TEXT_NAV_PAGE;
  }
  app.settings.autoNewest = prefs.getBool("auto", true);
  const RotationMode defaultRotationMode = BOARD_HAS_IMU_ORIENTATION ? ROTATION_AUTO : ROTATION_LANDSCAPE;
  app.settings.rotation = static_cast<RotationMode>(prefs.getUChar("rot", defaultRotationMode));
  if (app.settings.rotation > ROTATION_LANDSCAPE_180) {
    app.settings.rotation = defaultRotationMode;
  }
  if (!BOARD_HAS_IMU_ORIENTATION && app.settings.rotation == ROTATION_AUTO) {
    app.settings.rotation = defaultRotationMode;
  }
  displayRotation = prefs.getUChar("drot", BOARD_DEFAULT_ROTATION) & 0x03;
  orientationCandidate = displayRotation;
}

void saveSettings() {
  prefs.putUChar("br", app.settings.brightness);
  prefs.putUChar("pwr", app.settings.power);
  prefs.putUChar("det", app.settings.detail);
  prefs.putUChar("snd", app.settings.sound);
  prefs.putUChar("nav", app.settings.textNav);
  prefs.putBool("auto", app.settings.autoNewest);
  prefs.putUChar("rot", app.settings.rotation);
  prefs.putUChar("drot", displayRotation & 0x03);
}

void wakeDisplay() {
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
  displaySleeping = true;
  displayDimmed = false;
  pendingCue = CUE_NONE;
  shutdownSpeakerOutput();
  M5.Power.setLed(0);
  if (pmicReady) {
#if !defined(CODEX_COMPANION_CARDPUTER)
    pm1.disableLeds();
#endif
  }
  M5.Display.sleep();
  setCpuMhzIfNeeded(bleConnected ? activeCpuMhz() : sleepCpuMhz());
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

void sendHelloAck() {
  JsonDocument doc;
  doc["ack"] = "hello";
  doc["ok"] = true;
  JsonObject data = doc["data"].to<JsonObject>();
  data["device_id"] = security.deviceId;
  data["board"] = BOARD_TYPE;
  data["name"] = app.deviceName;
  data["paired"] = isPaired();
  data["nonce"] = security.sessionNonce;
  sendJson(doc);
}

void sendStatusAck() {
  samplePowerTelemetry(true);
  JsonDocument doc;
  doc["ack"] = "status";
  doc["ok"] = true;
  JsonObject data = doc["data"].to<JsonObject>();
  data["device_id"] = security.deviceId;
  data["board"] = BOARD_TYPE;
  data["name"] = app.deviceName;
  data["sec"] = isPaired();
  data["auth"] = security.sessionAuthorized;
  JsonObject battery = data["bat"].to<JsonObject>();
  const int pct = batteryPercent();
  if (pct >= 0) {
    battery["pct"] = pct;
  }
  if (powerTelemetry.voltageMv > 0) {
    battery["mv"] = powerTelemetry.voltageMv;
  }
  if (powerTelemetry.vbusMv > 0) {
    battery["vbus_mv"] = powerTelemetry.vbusMv;
  }
  if (powerTelemetry.hasCurrent) {
    battery["ma"] = powerTelemetry.currentMa;
  }
  battery["usb"] = isUsbPowered();
  battery["charge_known"] = powerTelemetry.chargeKnown;
  if (powerTelemetry.chargeKnown) {
    battery["charging"] = powerTelemetry.charging;
  }
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
  const bool lowBatteryLow = lowBatteryPowerSaveActive();
  settings["low_battery_low"] = lowBatteryLow;
  settings["low_battery_max"] = lowBatteryLow;
  settings["sound"] = app.settings.sound;
  settings["detail"] = app.settings.detail;
  settings["nav"] = app.settings.textNav;
  settings["auto_newest"] = app.settings.autoNewest;
  settings["rotation_mode"] = app.settings.rotation;
  settings["display_rotation"] = displayRotation;
  settings["auto_dim_ms"] = autoDimAfterMs();
  settings["auto_sleep_ms"] = autoSleepAfterMs();
  settings["deep_sleep_ms"] = 0;
  settings["travel_shutdown_ms"] = 0;
  sendJson(doc);
}

void sendControl(const char* action) {
  JsonDocument doc;
  doc["cmd"] = "control";
  doc["action"] = action;
  sendJson(doc);
}

bool isLandscape();

uint8_t bodyLineHeight() {
  return DASHBOARD_LINE_HEIGHT;
}

uint8_t bodyTopY() {
  return isLandscape() ? 60 : 92;
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

bool isLandscape() {
  return (displayRotation & 0x01) != 0;
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

void createDashboardSprite() {
  canvas.deleteSprite();
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  useUiFont();
}

uint8_t fixedRotationForMode(RotationMode mode) {
  switch (mode) {
    case ROTATION_PORTRAIT:
      return 0;
    case ROTATION_LANDSCAPE:
      return 1;
    case ROTATION_PORTRAIT_180:
      return 2;
    case ROTATION_LANDSCAPE_180:
      return 3;
    case ROTATION_AUTO:
    case ROTATION_LOCK:
    default:
      return displayRotation & 0x03;
  }
}

void applyDisplayRotation(uint8_t rotation, bool persist = true) {
  rotation &= 0x03;
  const bool rotationChanged = rotation != displayRotation;
  const bool missingSprite = canvas.width() != M5.Display.width() || canvas.height() != M5.Display.height();
  if (!rotationChanged && !missingSprite) {
    return;
  }

  displayRotation = rotation;
  orientationCandidate = rotation;
  orientationCandidateSinceMs = 0;
  M5.Display.setRotation(displayRotation);
  createDashboardSprite();
  rebuildBodyLines();
  if (persist) {
    prefs.putUChar("drot", displayRotation);
  }
  needsRedraw = true;
}

void applyRotationMode() {
  if (app.settings.rotation == ROTATION_AUTO || app.settings.rotation == ROTATION_LOCK) {
    applyDisplayRotation(displayRotation, true);
    return;
  }
  applyDisplayRotation(fixedRotationForMode(app.settings.rotation), true);
}

bool rotationFromAccel(float ax, float ay, uint8_t& rotation) {
  const float absX = fabsf(ax);
  const float absY = fabsf(ay);
  if (max(absX, absY) < ORIENTATION_MIN_G) {
    return false;
  }
  if (absX > absY + ORIENTATION_AXIS_MARGIN) {
    rotation = ax > 0.0f ? 2 : 0;
    return true;
  }
  if (absY > absX + ORIENTATION_AXIS_MARGIN) {
    rotation = ay > 0.0f ? 1 : 3;
    return true;
  }
  return false;
}

void initializeRotation() {
  uint8_t nextRotation = displayRotation;
  if (BOARD_HAS_IMU_ORIENTATION && app.settings.rotation == ROTATION_AUTO) {
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    if (M5.Imu.getAccel(&ax, &ay, &az)) {
      rotationFromAccel(ax, ay, nextRotation);
    }
  } else if (app.settings.rotation != ROTATION_LOCK) {
    nextRotation = fixedRotationForMode(app.settings.rotation);
  }
  applyDisplayRotation(nextRotation, true);
}

void handleAutoRotation() {
  if (
    !BOARD_HAS_IMU_ORIENTATION
    || displaySleeping
    || app.settings.rotation != ROTATION_AUTO
    || millis() - lastOrientationCheckMs < ORIENTATION_CHECK_MS
  ) {
    return;
  }
  lastOrientationCheckMs = millis();

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) {
    return;
  }

  uint8_t detected = displayRotation;
  if (!rotationFromAccel(ax, ay, detected)) {
    orientationCandidateSinceMs = 0;
    orientationCandidate = displayRotation;
    return;
  }

  if (detected != orientationCandidate) {
    orientationCandidate = detected;
    orientationCandidateSinceMs = millis();
    return;
  }

  if (detected != displayRotation && orientationCandidateSinceMs > 0 && millis() - orientationCandidateSinceMs >= ORIENTATION_STABLE_MS) {
    applyDisplayRotation(detected, true);
  }
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
  const bool completed = kind == "complete" || kind == "completed" || kind == "task_complete" || kind == "review";
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
      || kind == "review"
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

bool handleCodexActivity(JsonObject activity) {
  String status = activity["status"] | "idle";
  String title = activity["title"] | "Codex";
  String subtitle = activity["subtitle"] | "";
  String waitingKind = activity["waiting_kind"] | "";
  status.toLowerCase();
  waitingKind.toLowerCase();
  if (
    status != "idle"
    && status != "running"
    && status != "waiting"
    && status != "failed"
    && status != "review"
  ) {
    status = "idle";
  }
  if (subtitle.isEmpty()) {
    subtitle = status;
  }

  const bool changed =
    !app.hasCodexActivity
    || status != app.activityStatus
    || title != app.activityTitle
    || subtitle != app.activitySubtitle
    || waitingKind != app.activityWaitingKind;

  app.hasCodexActivity = true;
  app.activityStatus = status;
  app.activityTitle = title;
  app.activitySubtitle = subtitle;
  app.activityWaitingKind = waitingKind;
  const String currentKind = status == "failed" ? "error" : status;
  return setCurrentStatus(title, currentKind, subtitle) || changed;
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
    if (app.settings.detail != DETAIL_FULL) {
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

bool handleLegacyText(JsonDocument& doc) {
  const String signature = legacySnapshotSignature(doc);
  if (signature.isEmpty() || signature == app.legacySignature) {
    return false;
  }
  app.legacySignature = signature;

  bool changed = false;
  if (doc["msg"].is<const char*>()) {
    changed = setCurrentStatus("Codex", "status", doc["msg"].as<String>()) || changed;
  }

  uint8_t added = 0;
  if (app.settings.detail != DETAIL_FULL) {
    return changed;
  }
  if (doc["entries"].is<JsonArray>()) {
    JsonArray entries = doc["entries"].as<JsonArray>();
    for (int index = static_cast<int>(entries.size()) - 1; index >= 0; --index) {
      added += appendActivityBlock("Codex", "legacy", entries[index].as<String>());
    }
  } else if (doc["msg"].is<const char*>()) {
    added += appendActivityBlock("Codex", "legacy", doc["msg"].as<String>());
  }
  applyNewLines(added);
  return changed || added > 0;
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
  if (awaitingSnapshot) {
    awaitingSnapshot = false;
    changed = true;
  }
  const uint16_t nextTotal = doc["total"] | app.total;
  const uint16_t nextRunning = doc["running"] | 0;
  const uint16_t nextWaiting = doc["waiting"] | 0;
  changed = changed || nextTotal != app.total || nextRunning != app.running || nextWaiting != app.waiting;
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

  const bool hasCodexActivity = doc["codex_activity"].is<JsonObject>();
  const bool hasStructuredText = hasCodexActivity || doc["status"].is<JsonObject>() || doc["activity"].is<JsonArray>();
  if (hasCodexActivity) {
    changed = handleCodexActivity(doc["codex_activity"].as<JsonObject>()) || changed;
  } else {
    changed = app.hasCodexActivity || changed;
    app.hasCodexActivity = false;
  }

  if (!hasCodexActivity && doc["status"].is<JsonObject>()) {
    changed = handleStatus(doc["status"].as<JsonObject>()) || changed;
  } else if (!hasCodexActivity && doc["msg"].is<const char*>()) {
    changed = setCurrentStatus("Codex", "status", doc["msg"].as<String>()) || changed;
  }

  if (doc["activity"].is<JsonArray>()) {
    changed = handleActivity(doc["activity"].as<JsonArray>()) > 0 || changed;
  } else if (!hasStructuredText) {
    changed = handleLegacyText(doc) || changed;
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

void cancelPairing();

void handleCommand(JsonDocument& doc) {
  const String command = doc["cmd"].as<String>();
  if (command == "hello") {
    sendHelloAck();
    return;
  }
  if (command == "pair_begin") {
    if (isPaired()) {
      sendAck("pair_begin", false, "already_paired");
      return;
    }
    const String hostId = doc["host_id"].as<String>();
    const String secretHex = doc["secret"].as<String>();
    if (hostId.isEmpty() || secretHex.length() != PAIR_SECRET_HEX_LENGTH) {
      sendAck("pair_begin", false, "invalid_pairing_request");
      return;
    }
    security.pendingHostId = hostId;
    security.pendingSecretHex = secretHex;
    security.pairingCode = randomPairingCode();
    security.pairingStartedMs = millis();
    security.pairingActive = true;
    security.pairingConfirmed = false;
    wakeDisplay();
    app.settings.open = false;
    setCurrentStatus("System", "pair", "Pair " + security.pairingCode);
    needsRedraw = true;

    JsonDocument ack;
    ack["ack"] = "pair_begin";
    ack["ok"] = true;
    JsonObject data = ack["data"].to<JsonObject>();
    data["code"] = security.pairingCode;
    data["device_id"] = security.deviceId;
    data["board"] = BOARD_TYPE;
    data["name"] = app.deviceName;
    sendJson(ack);
    return;
  }
  if (command == "pair_commit") {
    if (!security.pairingActive) {
      sendAck("pair_commit", false, "pairing_not_active");
      return;
    }
    if (millis() - security.pairingStartedMs > PAIRING_TIMEOUT_MS) {
      cancelPairing();
      sendAck("pair_commit", false, "pairing_expired");
      return;
    }
    const String code = doc["code"].as<String>();
    if (code != security.pairingCode) {
      sendAck("pair_commit", false, "invalid_code");
      return;
    }
    if (!security.pairingConfirmed) {
      sendAck("pair_commit", false, "confirm_on_device");
      return;
    }
    savePairing(security.pendingHostId, security.pendingSecretHex);
    security.pairingActive = false;
    security.pairingConfirmed = false;
    security.pendingHostId = "";
    security.pendingSecretHex = "";
    security.pairingCode = "";
    setCurrentStatus("System", "pair", "Paired");
    needsRedraw = true;

    JsonDocument ack;
    ack["ack"] = "pair_commit";
    ack["ok"] = true;
    JsonObject data = ack["data"].to<JsonObject>();
    data["device_id"] = security.deviceId;
    data["board"] = BOARD_TYPE;
    data["name"] = app.deviceName;
    sendJson(ack);
    return;
  }
  if (command == "auth") {
    if (!isPaired()) {
      sendAck("auth", false, "not_paired");
      return;
    }
    const String hostId = doc["host_id"].as<String>();
    const String hostNonce = doc["nonce"].as<String>();
    const String mac = doc["mac"].as<String>();
    if (hostId != security.hostId || hostNonce.isEmpty() || mac.isEmpty()) {
      sendAck("auth", false, "invalid_auth");
      return;
    }
    const String expected = hmacSha256Hex(security.secretHex, authMessage(hostNonce, hostId));
    if (expected.isEmpty() || !constantTimeEquals(expected, mac)) {
      sendAck("auth", false, "auth_failed");
      return;
    }
    security.sessionAuthorized = true;
    sendAck("auth", true);
    return;
  }
  if (isPaired() && !security.sessionAuthorized) {
    sendAck(command.c_str(), false, "unauthorized");
    return;
  }
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
    clearPairing();
    sendAck("unpair", true);
    setCurrentStatus("System", "unpair", "Unpaired");
    needsRedraw = true;
    return;
  }

  sendAck(command.c_str(), false, "unsupported");
}

void handleLine(const String& line) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, line);
  if (error) {
    Serial.printf("Ignoring malformed BLE JSON line: %s\n", error.c_str());
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

  if (isPaired() && !security.sessionAuthorized) {
    Serial.println("Ignoring unauthenticated snapshot");
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
      Serial.println("Cleared BLE RX buffer after overflow");
    }
  }
}

void applyBlePowerPolicy();
void requestBleConnectionTuning();

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server) override {
    bleServer = server;
    const bool wasSleeping = displaySleeping;
    bleConnected = true;
    resetAuthorizedSession();
    awaitingSnapshot = true;
    rxBuffer = "";
    lastAdvertisingEnsureMs = millis();
    requestBleConnectionTuning();
    setCurrentStatus("System", "sync", "BLE syncing");
    if (!wasSleeping) {
      requestCue(CUE_CONNECTED);
    }
  }

  void onDisconnect(NimBLEServer*) override {
    const bool wasSleeping = displaySleeping;
    bleConnected = false;
    resetAuthorizedSession();
    awaitingSnapshot = false;
    bleConnParamsPending = false;
    bleAdvertisingRestartRequested = true;
    rxBuffer = "";
    setCurrentStatus("System", "disconnected", "BLE disconnected");
    if (!wasSleeping) {
      requestCue(CUE_DISCONNECTED);
    }
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    processRx(characteristic->getValue());
  }
};

uint16_t advertisingMinInterval() {
  switch (effectivePowerMode()) {
    case POWER_LOW:
      return 2400;  // 1.5s in 0.625ms units.
    case POWER_AUTO:
      return 1600;  // 1.0s.
    case POWER_ALWAYS:
    default:
      return 800;   // 0.5s.
  }
}

uint16_t advertisingMaxInterval() {
  switch (effectivePowerMode()) {
    case POWER_LOW:
      return 3200;  // 2.0s.
    case POWER_AUTO:
      return 2400;  // 1.5s.
    case POWER_ALWAYS:
    default:
      return 1200;  // 0.75s.
  }
}

void connectionParamsForPower(uint16_t& minInterval, uint16_t& maxInterval, uint16_t& latency, uint16_t& timeout) {
  switch (effectivePowerMode()) {
    case POWER_LOW:
      minInterval = 160;  // 200ms in 1.25ms units.
      maxInterval = 400;  // 500ms.
      latency = 8;
      timeout = 1600;     // 16s in 10ms units.
      break;
    case POWER_AUTO:
      minInterval = 96;   // 120ms.
      maxInterval = 192;  // 240ms.
      latency = 4;
      timeout = 1200;     // 12s.
      break;
    case POWER_ALWAYS:
    default:
      minInterval = 48;   // 60ms.
      maxInterval = 96;   // 120ms.
      latency = 2;
      timeout = 1200;     // 12s.
      break;
  }
}

esp_power_level_t bleTxPowerLevel() {
  switch (effectivePowerMode()) {
    case POWER_LOW:
      return ESP_PWR_LVL_N6;
    case POWER_AUTO:
      return ESP_PWR_LVL_N0;
    case POWER_ALWAYS:
    default:
      return ESP_PWR_LVL_P3;
  }
}

void applyBlePowerPolicy() {
  NimBLEDevice::setPower(bleTxPowerLevel(), ESP_BLE_PWR_TYPE_DEFAULT);
  NimBLEDevice::setPower(bleTxPowerLevel(), ESP_BLE_PWR_TYPE_ADV);
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setMinInterval(advertisingMinInterval());
  advertising->setMaxInterval(advertisingMaxInterval());
  uint16_t minInterval = 0;
  uint16_t maxInterval = 0;
  uint16_t latency = 0;
  uint16_t timeout = 0;
  connectionParamsForPower(minInterval, maxInterval, latency, timeout);
  advertising->setMinPreferred(minInterval);
  advertising->setMaxPreferred(maxInterval);
}

void requestBleConnectionTuning() {
  bleConnParamsPending = true;
  bleTuneRequestedMs = millis();
}

void ensureBleAdvertising() {
  if (bleConnected || millis() - lastAdvertisingEnsureMs < BLE_ADVERTISING_ENSURE_MS) {
    return;
  }
  lastAdvertisingEnsureMs = millis();
  applyBlePowerPolicy();
  NimBLEDevice::startAdvertising();
}

void handleBleAdvertisingRestart() {
  if (!bleAdvertisingRestartRequested || bleConnected) {
    return;
  }
  bleAdvertisingRestartRequested = false;
  applyBlePowerPolicy();
  lastAdvertisingEnsureMs = millis();
  NimBLEDevice::startAdvertising();
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
  static PowerMode lastMode = POWER_ALWAYS;
  const PowerMode mode = effectivePowerMode();
  if (initialized && mode == lastMode) {
    return;
  }

  initialized = true;
  lastMode = mode;
  applyBrightness();
  applyBlePowerPolicy();
  requestBleConnectionTuning();
  setCpuMhzIfNeeded(displaySleeping && !bleConnected ? sleepCpuMhz() : activeCpuMhz());
  enforceIndicatorLedsOff(true);
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
  uint16_t minInterval = 0;
  uint16_t maxInterval = 0;
  uint16_t latency = 0;
  uint16_t timeout = 0;
  connectionParamsForPower(minInterval, maxInterval, latency, timeout);
  advertising->setMinPreferred(minInterval);
  advertising->setMaxPreferred(maxInterval);
  lastAdvertisingEnsureMs = millis();
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
  drawBar(30, y + 5, 68, 8, pct, color);
  drawTextAt(104, y, pct >= 0 ? String(pct) + "%" : "--%", color);
}

void drawLimitInline(int x, int y, const RateLimitWindowState& window, const char* fallbackLabel) {
  const String label = window.available ? window.label : fallbackLabel;
  const int pct = window.available ? window.remainingPct : app.remainingPct;
  const uint16_t color = limitColor(pct);
  useUiFont();
  drawTextAt(x, y, fitTextPixels(label, 18), rgb(168, 172, 170));
  drawBar(x + 22, y + 5, 28, 8, pct, color);
  drawTextAt(x + 54, y, pct >= 0 ? String(pct) + "%" : "--%", color);
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
    case MODE_SYNC: {
      static const uint8_t frames[4][9] = {
        {2, 2, 2, 1, 0, 0, 0, 0, 0},
        {0, 1, 2, 0, 0, 2, 0, 1, 2},
        {0, 0, 0, 0, 0, 1, 2, 2, 2},
        {2, 1, 0, 2, 0, 0, 2, 1, 0}
      };
      drawMatrix(frames[(frame / 2) % 4], rgb(18, 42, 52), rgb(72, 116, 134), rgb(144, 198, 214));
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

  const int pct = batteryPercent();
  char battery[5];
  if (pct >= 0) {
    snprintf(battery, sizeof(battery), "%d%%", constrain(pct, 0, 100));
  } else {
    snprintf(battery, sizeof(battery), "--%");
  }
  const uint16_t active = rgb(235, 239, 235);
  const int batteryX = canvas.width() - 27;
  const int bleX = canvas.width() - 56;
  const int usbX = canvas.width() - 86;
  drawTextAt(16, 1, fitTextPixels(modeLabel(mode), max(24, usbX - 20)), TFT_WHITE, color);
  if (app.hasNew && app.scrollOffset > 0) {
    canvas.fillRect(0, TOP_BAR_HEIGHT - 2, canvas.width(), 2, rgb(255, 196, 78));
  }
  drawTextAt(usbX, 1, isBatteryCharging() ? "CHG" : (isUsbPowered() ? "USB" : "   "), active, color);
  drawTextAt(bleX, 1, bleConnected ? "BLE" : "   ", active, color);
  drawTextAt(batteryX, 1, battery, active, color);
}

void drawBodyText() {
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

void drawDashboard() {
  canvas.fillScreen(TFT_BLACK);
  useUiFont();
  drawTopBar();

  if (isLandscape()) {
    drawLimitInline(4, 21, app.primaryLimit, "5h");
    drawLimitInline(88, 21, app.secondaryLimit, "7d");
    drawTextAt(176, 21, fitTextPixels(tokenLabel(), canvas.width() - 180), TFT_LIGHTGREY);

    const String currentRaw = app.statusSpeaker + ": " + app.statusText;
    useBodyFontFor(currentRaw);
    const String current = fitTextPixels(currentRaw, canvas.width() - 8);
    drawTextAt(4, 39, current, speakerColor(app.statusSpeaker));
    useUiFont();
    canvas.drawFastHLine(4, 56, canvas.width() - 8, rgb(42, 42, 42));
  } else {
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
  }

  if (app.settings.detail == DETAIL_FULL) {
    drawBodyText();
  }

}

const char* brightnessName() {
  static const char* names[] = {"Low", "Med", "High"};
  return names[constrain(app.settings.brightness, 0, 2)];
}

const char* powerName() {
  switch (app.settings.power) {
    case POWER_LOW:
      return "Low";
    case POWER_AUTO:
      return "Auto";
    case POWER_ALWAYS:
    default:
      return "Always";
  }
}

const char* detailName() {
  switch (app.settings.detail) {
    case DETAIL_USAGE:
      return "Usage";
    case DETAIL_STATUS:
      return "Status";
    case DETAIL_FULL:
    default:
      return "Full";
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

const char* rotationName() {
  switch (app.settings.rotation) {
    case ROTATION_LOCK:
      return "Lock";
    case ROTATION_PORTRAIT:
      return "P";
    case ROTATION_LANDSCAPE:
      return "L";
    case ROTATION_PORTRAIT_180:
      return "P180";
    case ROTATION_LANDSCAPE_180:
      return "L180";
    case ROTATION_AUTO:
    default:
      return "Auto";
  }
}

String settingLabel(uint8_t index) {
  switch (index) {
    case SETTING_BRIGHTNESS:
      return "Brightness " + String(brightnessName());
    case SETTING_POWER:
      return String("Power ") + powerName() + (lowBatteryPowerSaveActive() ? ">Low" : "");
    case SETTING_DETAIL:
      return "Detail " + String(detailName());
    case SETTING_SOUND:
      return "Sound " + String(soundName());
    case SETTING_TEXT_NAV:
      return "Text nav " + String(navName());
    case SETTING_AUTO_NEWEST:
      return String("Auto newest ") + (app.settings.autoNewest ? "On" : "Off");
    case SETTING_ROTATION:
      return "Rotation " + String(rotationName());
    default:
      return "";
  }
}

void drawSettings() {
  canvas.fillScreen(TFT_BLACK);
  useUiFont();
  drawTopBar();
  drawTextAt(4, 22, "SETTINGS", rgb(210, 214, 210));

  const bool landscape = isLandscape();
  const uint8_t rowStart = landscape ? 36 : 48;
  const uint8_t rowHeight = landscape ? 15 : 20;
  for (uint8_t i = 0; i < SETTING_COUNT; ++i) {
    const uint8_t y = rowStart + i * rowHeight;
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
  batteryLine += isBatteryCharging() ? " CHG " : (isUsbPowered() ? " USB " : " BAT ");
  batteryLine += pct >= 0 ? String(pct) + "%" : "n/a";
  if (powerTelemetry.voltageMv > 0) {
    batteryLine += " ";
    batteryLine += String(powerTelemetry.voltageMv);
    batteryLine += "mV";
  }
  if (powerTelemetry.vbusMv > 0) {
    batteryLine += " VIN";
    batteryLine += String(powerTelemetry.vbusMv);
  }
  if (powerTelemetry.hasCurrent) {
    batteryLine += " ";
    batteryLine += String(powerTelemetry.currentMa);
    batteryLine += "mA";
  }
  if (landscape) {
    drawTextAt(100, 22, fitTextPixels(batteryLine, canvas.width() - 104), rgb(100, 104, 108));
  } else {
    drawTextAt(4, 176, fitTextPixels(app.deviceName, canvas.width() - 8), rgb(100, 104, 108));
    drawTextAt(4, 193, fitTextPixels(batteryLine, canvas.width() - 8), rgb(100, 104, 108));
    drawTextAt(4, 210, fitTextPixels("Heap " + formatCompact(ESP.getFreeHeap()), canvas.width() - 8), rgb(100, 104, 108));
    drawTextAt(4, canvas.height() - DASHBOARD_FONT_HEIGHT, fitTextPixels(BOARD_INPUT_HINT, canvas.width() - 8), rgb(100, 104, 108));
  }
}

void drawPairing() {
  canvas.fillScreen(TFT_BLACK);
  useUiFont();
  drawTopBar();
  const uint16_t accent = security.pairingConfirmed ? rgb(114, 166, 109) : rgb(224, 166, 78);
  const String title = security.pairingConfirmed ? "PAIR CONFIRMED" : "PAIR " + security.pairingCode;
  drawTextAt(8, 34, fitTextPixels(title, canvas.width() - 16), accent);
  drawTextAt(8, 56, fitTextPixels("Confirm on this device", canvas.width() - 16), TFT_LIGHTGREY);
#if defined(CODEX_COMPANION_CARDPUTER)
  drawTextAt(8, 76, fitTextPixels("Press GO/G0", canvas.width() - 16), rgb(100, 104, 108));
#else
  drawTextAt(8, 76, fitTextPixels("Press Button A", canvas.width() - 16), rgb(100, 104, 108));
#endif
}

void redraw() {
  if (displaySleeping) {
    needsRedraw = false;
    return;
  }
  if (security.pairingActive) {
    drawPairing();
  } else if (app.settings.open) {
    drawSettings();
  } else {
    drawDashboard();
  }
  canvas.pushSprite(0, 0);
  needsRedraw = false;
}

void scrollNewerBy(uint16_t step) {
  if (app.settings.detail != DETAIL_FULL) {
    app.scrollOffset = 0;
    app.hasNew = false;
    needsRedraw = true;
    return;
  }
  step = max<uint16_t>(1, step);
  app.scrollOffset = app.scrollOffset > step ? app.scrollOffset - step : 0;
  if (app.scrollOffset == 0) {
    app.hasNew = false;
  }
  needsRedraw = true;
}

void scrollOlderBy(uint16_t step) {
  if (app.settings.detail != DETAIL_FULL) {
    app.scrollOffset = 0;
    app.hasNew = false;
    needsRedraw = true;
    return;
  }
  step = max<uint16_t>(1, step);
  app.scrollOffset = min<uint16_t>(maxScrollOffset(), app.scrollOffset + step);
  needsRedraw = true;
}

void scrollNewer() {
  scrollNewerBy(app.settings.textNav == TEXT_NAV_LINE ? 1 : visibleBodyLines());
}

void scrollOlder() {
  scrollOlderBy(app.settings.textNav == TEXT_NAV_LINE ? 1 : visibleBodyLines());
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

uint8_t cycledIndex(uint8_t value, uint8_t count, int8_t delta) {
  if (count == 0) {
    return 0;
  }
  int next = static_cast<int>(value) + delta;
  while (next < 0) {
    next += count;
  }
  return static_cast<uint8_t>(next % count);
}

RotationMode cycledRotationMode(int8_t delta) {
  uint8_t current = app.settings.rotation;
  for (uint8_t attempts = 0; attempts < 6; ++attempts) {
    current = cycledIndex(current, 6, delta);
    if (BOARD_HAS_IMU_ORIENTATION || current != ROTATION_AUTO) {
      return static_cast<RotationMode>(current);
    }
  }
  return app.settings.rotation;
}

void rotateSetting(int8_t delta = 1) {
  if (delta == 0) {
    return;
  }
  touchSettings();
  switch (app.settings.selected) {
    case SETTING_BRIGHTNESS:
      app.settings.brightness = cycledIndex(app.settings.brightness, 3, delta);
      applyBrightness();
      break;
    case SETTING_POWER:
      app.settings.power = static_cast<PowerMode>(cycledIndex(app.settings.power, POWER_COUNT, delta));
      if (!isUsbPowered() && app.settings.power != POWER_ALWAYS && app.settings.brightness > 1) {
        app.settings.brightness = 1;
      }
      applyBrightness();
      applyBlePowerPolicy();
      requestBleConnectionTuning();
      setCpuMhzIfNeeded(displaySleeping && !bleConnected ? sleepCpuMhz() : activeCpuMhz());
      break;
    case SETTING_DETAIL:
      app.settings.detail = static_cast<DetailMode>(cycledIndex(app.settings.detail, DETAIL_COUNT, delta));
      app.scrollOffset = 0;
      app.hasNew = false;
      break;
    case SETTING_SOUND:
      app.settings.sound = static_cast<SoundMode>(cycledIndex(app.settings.sound, 3, delta));
      requestCue(app.settings.sound == SOUND_OFF ? CUE_NONE : CUE_ACTIVITY);
      break;
    case SETTING_TEXT_NAV:
      app.settings.textNav = app.settings.textNav == TEXT_NAV_PAGE ? TEXT_NAV_LINE : TEXT_NAV_PAGE;
      clampScroll();
      break;
    case SETTING_AUTO_NEWEST:
      app.settings.autoNewest = !app.settings.autoNewest;
      break;
    case SETTING_ROTATION:
      app.settings.rotation = cycledRotationMode(delta);
      applyRotationMode();
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

void previousSetting() {
  touchSettings();
  app.settings.selected = cycledIndex(app.settings.selected, SETTING_COUNT, -1);
  needsRedraw = true;
}

void jumpNewestOrSleep() {
  if (app.scrollOffset > 0 && app.hasNew) {
    jumpNewest();
  } else {
    enterDisplaySleep();
  }
}

void confirmPairingOnDevice() {
  if (!security.pairingActive) {
    return;
  }
  security.pairingConfirmed = true;
  setCurrentStatus("System", "pair", "Pair confirmed");
  needsRedraw = true;
}

void cancelPairing() {
  security.pairingActive = false;
  security.pairingConfirmed = false;
  security.pairingCode = "";
  security.pendingHostId = "";
  security.pendingSecretHex = "";
  setCurrentStatus("System", "pair", "Pair cancelled");
  needsRedraw = true;
}

void dispatchInput(InputAction action) {
  if (action == ACTION_NONE) {
    return;
  }
  if (displaySleeping) {
    wakeDisplay();
    return;
  }
  autoSleepEligibleSinceMs = 0;

  if (security.pairingActive) {
    if (action == ACTION_SCROLL_NEWER || action == ACTION_GO || action == ACTION_CONFIRM) {
      confirmPairingOnDevice();
    } else if (action == ACTION_BACK || action == ACTION_SLEEP) {
      cancelPairing();
    }
    return;
  }

  if (app.settings.open) {
    switch (action) {
      case ACTION_CLOSE_SETTINGS:
      case ACTION_OPEN_SETTINGS:
      case ACTION_BACK:
        closeSettings();
        break;
      case ACTION_NEXT_SETTING:
      case ACTION_SCROLL_NEWER_LINE:
        nextSetting();
        break;
      case ACTION_PREVIOUS_SETTING:
      case ACTION_SCROLL_OLDER_LINE:
        previousSetting();
        break;
      case ACTION_NEXT_VALUE:
      case ACTION_SCROLL_NEWER_PAGE:
      case ACTION_CONFIRM:
      case ACTION_GO:
        rotateSetting(1);
        break;
      case ACTION_PREVIOUS_VALUE:
      case ACTION_SCROLL_OLDER_PAGE:
        rotateSetting(-1);
        break;
      case ACTION_SLEEP:
        enterDisplaySleep();
        break;
      default:
        break;
    }
    return;
  }

  switch (action) {
    case ACTION_SCROLL_NEWER:
      scrollNewer();
      break;
    case ACTION_SCROLL_OLDER:
      scrollOlder();
      break;
    case ACTION_SCROLL_NEWER_LINE:
      scrollNewerBy(1);
      break;
    case ACTION_SCROLL_OLDER_LINE:
      scrollOlderBy(1);
      break;
    case ACTION_SCROLL_NEWER_PAGE:
      scrollNewerBy(visibleBodyLines());
      break;
    case ACTION_SCROLL_OLDER_PAGE:
      scrollOlderBy(visibleBodyLines());
      break;
    case ACTION_OPEN_SETTINGS:
    case ACTION_CONFIRM:
      openSettings();
      break;
    case ACTION_BACK:
    case ACTION_GO:
      if (app.scrollOffset > 0) {
        jumpNewest();
      }
      break;
    case ACTION_SLEEP:
      enterDisplaySleep();
      break;
    case ACTION_JUMP_OR_SLEEP:
      jumpNewestOrSleep();
      break;
    default:
      break;
  }
}

PressEvent updateButtonTracker(ButtonTracker& tracker, bool pressed, bool allowLong, bool suppressIndividual) {
  const uint32_t now = millis();
  if (pressed && !tracker.down) {
    tracker.down = true;
    tracker.longSent = false;
    tracker.pressMs = now;
  }

  if (pressed && tracker.down && allowLong && !tracker.longSent && now - tracker.pressMs >= LONG_PRESS_MS && !suppressIndividual) {
    tracker.longSent = true;
    return PRESS_LONG;
  }

  if (!pressed && tracker.down) {
    tracker.down = false;
    if (!tracker.longSent && !suppressIndividual) {
      return PRESS_SINGLE;
    }
  }
  return PRESS_NONE;
}

void dispatchPress(PressEvent event, InputAction singleAction, InputAction longAction = ACTION_NONE) {
  if (event == PRESS_SINGLE) {
    dispatchInput(singleAction);
  } else if (event == PRESS_LONG) {
    dispatchInput(longAction);
  }
}

void handleStickButtons(bool aPressed, bool bPressed) {
  const bool bothPressed = aPressed && bPressed;

  if (bothPressed) {
    comboActive = true;
  }

  const PressEvent aEvent = updateButtonTracker(buttonA, aPressed, true, comboActive);
  const PressEvent bEvent = updateButtonTracker(buttonB, bPressed, true, comboActive);

  if (aEvent == PRESS_SINGLE) {
    dispatchInput(app.settings.open ? ACTION_NEXT_VALUE : ACTION_SCROLL_NEWER);
  } else if (aEvent == PRESS_LONG) {
    dispatchInput(app.settings.open ? ACTION_CLOSE_SETTINGS : ACTION_OPEN_SETTINGS);
  }

  if (bEvent == PRESS_SINGLE) {
    dispatchInput(app.settings.open ? ACTION_NEXT_SETTING : ACTION_SCROLL_OLDER);
  } else if (bEvent == PRESS_LONG && !app.settings.open) {
    dispatchInput(ACTION_JUMP_OR_SLEEP);
  }

  if (!aPressed && !bPressed) {
    comboActive = false;
  }
}

#if defined(CODEX_COMPANION_CARDPUTER)
void handleCardputerButtons() {
  auto& keys = M5Cardputer.Keyboard.keysState();
  const bool upPressed = keys.up || M5Cardputer.Keyboard.isKeyPressed(';');
  const bool downPressed = keys.down || M5Cardputer.Keyboard.isKeyPressed('.');
  const bool leftPressed = keys.left || M5Cardputer.Keyboard.isKeyPressed(',');
  const bool rightPressed = keys.right || M5Cardputer.Keyboard.isKeyPressed('/');
  const bool backPressed = keys.backspace || keys.esc || M5Cardputer.Keyboard.isKeyPressed('`');
  dispatchPress(updateButtonTracker(keyUp, upPressed, false, false), ACTION_SCROLL_OLDER_LINE);
  dispatchPress(updateButtonTracker(keyDown, downPressed, false, false), ACTION_SCROLL_NEWER_LINE);
  dispatchPress(updateButtonTracker(keyLeft, leftPressed, false, false), ACTION_SCROLL_OLDER_PAGE);
  dispatchPress(updateButtonTracker(keyRight, rightPressed, false, false), ACTION_SCROLL_NEWER_PAGE);
  dispatchPress(updateButtonTracker(keyEnter, keys.enter, false, false), ACTION_CONFIRM);
  dispatchPress(updateButtonTracker(keyBack, backPressed, false, false), ACTION_BACK);
  dispatchPress(updateButtonTracker(keyGo, M5Cardputer.BtnA.isPressed(), true, false), ACTION_GO, ACTION_SLEEP);
}
#endif

void beginBoard() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  cfg.led_brightness = 0;
#if defined(CODEX_COMPANION_CARDPUTER)
  M5Cardputer.begin(cfg, true);
#else
  M5.begin(cfg);
#endif
}

void updateBoard() {
#if defined(CODEX_COMPANION_CARDPUTER)
  M5Cardputer.update();
#else
  M5.update();
#endif
}

void handleButtons() {
#if defined(CODEX_COMPANION_CARDPUTER)
  handleCardputerButtons();
#else
  const bool aPressed = M5.BtnA.isPressed();
  const bool bPressed = M5.BtnB.isPressed();
  handleStickButtons(aPressed, bPressed);
#endif
}

void checkShakeWake() {
  if (!displaySleeping || !BOARD_HAS_IMU_ORIENTATION || millis() - lastShakeCheckMs < shakeCheckMs()) {
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
  const uint32_t sleepMs = autoSleepAfterMs();
  if (sleepMs > 0 && millis() - autoSleepEligibleSinceMs > sleepMs) {
    enterDisplaySleep();
  }
}

void handlePairingTimeout() {
  if (!security.pairingActive || millis() - security.pairingStartedMs <= PAIRING_TIMEOUT_MS) {
    return;
  }
  cancelPairing();
}

uint32_t loopDelayMs() {
  const PowerMode mode = effectivePowerMode();
  if (displaySleeping) {
    return mode == POWER_ALWAYS ? 160 : (mode == POWER_AUTO ? 240 : 300);
  }
  if (app.settings.open || pendingCue != CUE_NONE || speakerOutputActive) {
    return 20;
  }
  if (currentStatusMode() == MODE_WORK) {
    return mode == POWER_ALWAYS ? 50 : (mode == POWER_AUTO ? 120 : 180);
  }
  return mode == POWER_ALWAYS ? 60 : (mode == POWER_AUTO ? 140 : 220);
}

void seedBody() {
  uint8_t added = appendActivityBlock("System", "boot", "Dashboard booted. Waiting for Codex Desktop observer.");
  applyNewLines(added);
}

}  // namespace

void setup() {
  Serial.begin(115200);

  setCpuMhzIfNeeded(activeCpuMhz());
  beginBoard();
  M5.Power.setLed(0);
  setupPmicPowerManagement();
  setupSpeakerOutput();

  prefs.begin("codexdash", false);
  loadSecurityState();
  samplePowerTelemetry(true);
  loadSettings();
  initializeRotation();
  applyBrightness();
  setCpuMhzIfNeeded(activeCpuMhz());

  app.deviceName = String(BOARD_DEVICE_PREFIX) + chipSuffix();
  setupBle();
  setCurrentStatus("System", "advertise", "Advertise " + app.deviceName);
  seedBody();
  redraw();
}

void loop() {
  updateBoard();
  handleButtons();
  handlePairingTimeout();
  handleAutoRotation();
  handleSleepTimers();
  handleStatusAnimation();
  handlePowerPolicyChange();
  handleBleAdvertisingRestart();
  ensureBleAdvertising();
  handleBleConnectionTuning();
  playPendingCue();
  handleSpeakerPower();

  if (app.settings.open && millis() - app.settings.lastInputMs > SETTINGS_IDLE_MS) {
    closeSettings();
  }

  const bool wasUsb = powerTelemetry.usb;
  const bool wasCharging = powerTelemetry.charging;
  const bool powerChanged = samplePowerTelemetry(false);
  if (powerChanged) {
    if (displaySleeping && (wasUsb != powerTelemetry.usb || wasCharging != powerTelemetry.charging)) {
      wakeDisplay();
    }
    needsRedraw = true;
    handlePowerPolicyChange();
  }

  enforceIndicatorLedsOff(false);

  if (needsRedraw) {
    redraw();
  }

  delay(loopDelayMs());
}
