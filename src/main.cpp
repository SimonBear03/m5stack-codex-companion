#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

namespace {

constexpr const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

constexpr uint32_t STALE_AFTER_MS = 30000;
constexpr uint32_t BLE_NOTIFY_CHUNK_DELAY_MS = 8;
constexpr uint32_t LONG_PRESS_MS = 650;
constexpr uint32_t SETTINGS_IDLE_MS = 12000;
constexpr uint32_t ACTIVITY_SOUND_COOLDOWN_MS = 1400;
constexpr size_t BODY_LINE_COUNT = 190;
constexpr size_t SEEN_SEQ_COUNT = 24;
constexpr size_t BLE_NOTIFY_CHUNK_SIZE = 20;

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

enum TextSizeMode : uint8_t {
  TEXT_COMPACT = 0,
  TEXT_READABLE
};

enum SettingIndex : uint8_t {
  SETTING_BRIGHTNESS = 0,
  SETTING_SOUND,
  SETTING_TEXT_NAV,
  SETTING_TEXT_SIZE,
  SETTING_AUTO_NEWEST,
  SETTING_COUNT
};

enum Cue : uint8_t {
  CUE_NONE = 0,
  CUE_ACTIVITY,
  CUE_CONNECTED,
  CUE_COMPLETED,
  CUE_DISCONNECTED
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
  SoundMode sound = SOUND_SOFT;
  TextNavMode textNav = TEXT_NAV_PAGE;
  TextSizeMode textSize = TEXT_COMPACT;
  bool autoNewest = true;
  uint8_t selected = 0;
  bool open = false;
  uint32_t lastInputMs = 0;
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
Preferences prefs;
M5Canvas canvas(&M5.Display);
ButtonTracker buttonA;
ButtonTracker buttonB;
AppState app;
String rxBuffer;
bool bleConnected = false;
bool needsRedraw = true;
bool comboActive = false;
Cue pendingCue = CUE_NONE;
uint32_t lastActivityCueMs = 0;

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

String fitText(String text, size_t limit) {
  text = cleanText(text);
  if (text.length() <= limit) {
    return text;
  }
  return text.substring(0, limit > 3 ? limit - 3 : limit) + "...";
}

int batteryPercent() {
  const int level = M5.Power.getBatteryLevel();
  if (level < 0 || level > 100) {
    return -1;
  }
  return level;
}

bool isUsbPowered() {
  return M5.Power.isCharging();
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return canvas.color565(r, g, b);
}

uint16_t statusColor() {
  if (!bleConnected) {
    return rgb(145, 43, 58);
  }
  if (app.lastSnapshotMs > 0 && millis() - app.lastSnapshotMs > STALE_AFTER_MS) {
    return rgb(142, 103, 42);
  }
  if (app.waiting > 0) {
    return rgb(165, 92, 45);
  }
  if (app.running > 0) {
    return rgb(30, 108, 150);
  }
  return rgb(42, 126, 82);
}

String modeLabel() {
  if (!bleConnected) {
    return "OFF";
  }
  if (app.lastSnapshotMs > 0 && millis() - app.lastSnapshotMs > STALE_AFTER_MS) {
    return "STALE";
  }
  if (app.waiting > 0) {
    return "WAIT";
  }
  if (app.running > 0) {
    return "RUN";
  }
  return "IDLE";
}

uint16_t limitColor(int remainingPct) {
  if (remainingPct < 0) {
    return TFT_DARKGREY;
  }
  if (remainingPct <= 15) {
    return rgb(170, 68, 52);
  }
  if (remainingPct <= 35) {
    return rgb(174, 128, 56);
  }
  return rgb(75, 145, 111);
}

uint8_t brightnessValue() {
  static const uint8_t values[] = {54, 130, 210};
  return values[constrain(app.settings.brightness, 0, 2)];
}

void applyBrightness() {
  M5.Display.setBrightness(brightnessValue());
}

void loadSettings() {
  app.settings.brightness = prefs.getUChar("br", 1);
  if (app.settings.brightness > 2) {
    app.settings.brightness = 1;
  }
  app.settings.sound = static_cast<SoundMode>(prefs.getUChar("snd", SOUND_SOFT));
  if (app.settings.sound > SOUND_ALERTS) {
    app.settings.sound = SOUND_SOFT;
  }
  app.settings.textNav = static_cast<TextNavMode>(prefs.getUChar("nav", TEXT_NAV_PAGE));
  if (app.settings.textNav > TEXT_NAV_LINE) {
    app.settings.textNav = TEXT_NAV_PAGE;
  }
  app.settings.textSize = static_cast<TextSizeMode>(prefs.getUChar("txt", TEXT_COMPACT));
  if (app.settings.textSize > TEXT_READABLE) {
    app.settings.textSize = TEXT_COMPACT;
  }
  app.settings.autoNewest = prefs.getBool("auto", true);
}

void saveSettings() {
  prefs.putUChar("br", app.settings.brightness);
  prefs.putUChar("snd", app.settings.sound);
  prefs.putUChar("nav", app.settings.textNav);
  prefs.putUChar("txt", app.settings.textSize);
  prefs.putBool("auto", app.settings.autoNewest);
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
  M5.Speaker.setVolume(app.settings.sound == SOUND_ALERTS ? 36 : 24);
  M5.Speaker.tone(frequency, duration, -1, true);
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
    toneSoft(1046, 18);
    return;
  }
  if (cue == CUE_CONNECTED) {
    toneSoft(740, 28);
    delay(34);
    toneSoft(988, 32);
    return;
  }
  if (cue == CUE_COMPLETED) {
    toneSoft(880, 35);
    delay(42);
    toneSoft(1175, 40);
    return;
  }
  if (cue == CUE_DISCONNECTED) {
    toneSoft(330, 70);
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
  battery["usb"] = isUsbPowered();
  JsonObject sys = data["sys"].to<JsonObject>();
  sys["up"] = millis() / 1000;
  sys["heap"] = ESP.getFreeHeap();
  JsonObject stats = data["stats"].to<JsonObject>();
  stats["appr"] = app.approvals;
  stats["deny"] = app.denials;
  JsonObject settings = data["settings"].to<JsonObject>();
  settings["brightness"] = app.settings.brightness;
  settings["sound"] = app.settings.sound;
  settings["nav"] = app.settings.textNav;
  settings["text"] = app.settings.textSize;
  settings["auto_newest"] = app.settings.autoNewest;
  sendJson(doc);
}

void sendControl(const char* action) {
  JsonDocument doc;
  doc["cmd"] = "control";
  doc["action"] = action;
  sendJson(doc);
}

uint8_t maxBodyChars() {
  return app.settings.textSize == TEXT_READABLE ? 19 : 21;
}

uint8_t bodyLineHeight() {
  return app.settings.textSize == TEXT_READABLE ? 12 : 10;
}

uint8_t bodyTopY() {
  return 82;
}

uint8_t bodyBottomY() {
  return app.settings.textSize == TEXT_READABLE ? 226 : 229;
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

uint8_t appendWrappedText(const String& firstPrefix, const String& nextPrefix, String text) {
  text = cleanText(text);
  if (text.isEmpty()) {
    return 0;
  }

  const uint8_t maxChars = maxBodyChars();
  String remaining = text;
  String prefix = firstPrefix;
  uint8_t added = 0;

  while (!remaining.isEmpty()) {
    const int available = max<int>(6, maxChars - prefix.length());
    int take = min<int>(remaining.length(), available);
    if (remaining.length() > static_cast<size_t>(available)) {
      int breakAt = -1;
      for (int i = take; i > 0; --i) {
        if (remaining.charAt(i - 1) == ' ') {
          breakAt = i - 1;
          break;
        }
      }
      if (breakAt >= 4) {
        take = breakAt;
      }
    }

    String chunk = remaining.substring(0, take);
    chunk.trim();
    if (!chunk.isEmpty()) {
      appendBodyLine(prefix + chunk);
      added += 1;
    }
    remaining = remaining.substring(take);
    remaining.trim();
    prefix = nextPrefix;
  }

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

uint8_t appendActivityBlock(const String& speaker, const String& kind, String text) {
  text = cleanText(text);
  if (text.isEmpty()) {
    return 0;
  }

  uint8_t added = 0;
  appendBodyLine(activityHeader(speaker, kind));
  added += 1;
  added += appendWrappedText("  ", "  ", text);
  appendBodyLine("");
  added += 1;
  return added;
}

void applyNewLines(uint8_t added) {
  if (added == 0) {
    return;
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

void setCurrentStatus(String speaker, String kind, String text) {
  speaker = fitText(speaker.isEmpty() ? "Codex" : speaker, 12);
  kind = fitText(kind.isEmpty() ? "status" : kind, 16);
  text = fitText(text.isEmpty() ? kind : text, 96);
  const bool completed = kind == "complete" || kind == "completed" || kind == "task_complete";
  const bool changed = speaker != app.statusSpeaker || text != app.statusText || kind != app.statusKind;

  app.statusSpeaker = speaker;
  app.statusKind = kind;
  app.statusText = text;
  if (changed && completed) {
    requestCue(CUE_COMPLETED);
  }
  needsRedraw = true;
}

void handleStatus(JsonObject status) {
  const String speaker = status["speaker"] | "Codex";
  const String kind = status["kind"] | "status";
  const String text = status["text"] | "";
  setCurrentStatus(speaker, kind, text);
}

void handleActivity(JsonArray activity) {
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
    added += appendActivityBlock(speaker, kind, text);
  }
  applyNewLines(added);
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

void updateWindow(JsonObject source, RateLimitWindowState& target, const char* fallbackLabel) {
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
}

void updateRateLimits(JsonObject rateLimits) {
  if (rateLimits["primary"].is<JsonObject>()) {
    updateWindow(rateLimits["primary"].as<JsonObject>(), app.primaryLimit, "5h");
  }
  if (rateLimits["secondary"].is<JsonObject>()) {
    updateWindow(rateLimits["secondary"].as<JsonObject>(), app.secondaryLimit, "7d");
  }
}

void handleSnapshot(JsonDocument& doc) {
  app.total = doc["total"] | app.total;
  app.running = doc["running"] | 0;
  app.waiting = doc["waiting"] | 0;

  if (doc["tokens"].is<uint64_t>()) {
    app.tokens = doc["tokens"].as<uint64_t>();
    app.hasTokens = true;
  } else if (doc["tokens"].is<uint32_t>()) {
    app.tokens = doc["tokens"].as<uint32_t>();
    app.hasTokens = true;
  }

  if (doc["rate_limit_remaining_percent"].is<int>()) {
    app.remainingPct = constrain(doc["rate_limit_remaining_percent"].as<int>(), 0, 100);
  } else if (doc["remaining_pct"].is<int>()) {
    app.remainingPct = constrain(doc["remaining_pct"].as<int>(), 0, 100);
  } else if (doc["remaining"].is<int>()) {
    app.remainingPct = constrain(doc["remaining"].as<int>(), 0, 100);
  }

  if (doc["rate_limits"].is<JsonObject>()) {
    updateRateLimits(doc["rate_limits"].as<JsonObject>());
  }

  const bool hasStructuredText = doc["status"].is<JsonObject>() || doc["activity"].is<JsonArray>();
  if (doc["status"].is<JsonObject>()) {
    handleStatus(doc["status"].as<JsonObject>());
  } else if (doc["msg"].is<const char*>()) {
    setCurrentStatus("Codex", "status", doc["msg"].as<String>());
  }

  if (doc["activity"].is<JsonArray>()) {
    handleActivity(doc["activity"].as<JsonArray>());
  } else if (!hasStructuredText) {
    handleLegacyText(doc);
  }

  app.lastSnapshotMs = millis();
  clampScroll();
  needsRedraw = true;
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
    } else if (rxBuffer.length() < 4096) {
      rxBuffer += ch;
    } else {
      rxBuffer = "";
      setCurrentStatus("System", "error", "RX overflow");
    }
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    bleConnected = true;
    setCurrentStatus("System", "connected", "BLE connected");
    requestCue(CUE_CONNECTED);
  }

  void onDisconnect(NimBLEServer*) override {
    bleConnected = false;
    setCurrentStatus("System", "disconnected", "BLE disconnected");
    requestCue(CUE_DISCONNECTED);
    delay(80);
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    processRx(characteristic->getValue());
  }
};

void setupBle() {
  NimBLEDevice::init(app.deviceName.c_str());
  NimBLEServer* server = NimBLEDevice::createServer();
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
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
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
  canvas.drawRect(x, y, w, h, rgb(45, 45, 45));
  if (value >= 0) {
    const int fill = map(constrain(value, 0, 100), 0, 100, 0, w - 2);
    canvas.fillRect(x + 1, y + 1, fill, h - 2, color);
  }
}

void drawLimitRow(int y, const RateLimitWindowState& window, const char* fallbackLabel) {
  const String label = window.available ? window.label : fallbackLabel;
  const int pct = window.available ? window.remainingPct : app.remainingPct;
  drawTextAt(4, y, fitText(label, 3), TFT_LIGHTGREY);
  drawTextAt(25, y, pct >= 0 ? String(pct) + "%" : "--%", TFT_WHITE);
  drawBar(58, y + 1, 72, 7, pct, limitColor(pct));
}

void drawTopBar() {
  const uint16_t color = statusColor();
  canvas.fillRect(0, 0, canvas.width(), 15, color);
  canvas.setTextColor(TFT_WHITE, color);
  canvas.setCursor(4, 4);
  canvas.print(modeLabel());
  if (app.hasNew) {
    const uint16_t newBg = rgb(230, 186, 69);
    canvas.fillRoundRect(40, 2, 24, 11, 2, newBg);
    canvas.setTextColor(TFT_BLACK, newBg);
    canvas.setCursor(43, 4);
    canvas.print("NEW");
  }
  const int pct = batteryPercent();
  String right = String(bleConnected ? "BLE" : "---");
  if (isUsbPowered()) {
    right += " USB";
  }
  if (pct >= 0) {
    right += " " + String(pct) + "%";
  }
  canvas.setCursor(max<int>(4, canvas.width() - static_cast<int>(right.length() * 6) - 3), 4);
  canvas.print(right);
}

void drawDashboard() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextSize(1);
  drawTopBar();

  drawLimitRow(20, app.primaryLimit, "5h");
  drawLimitRow(33, app.secondaryLimit, "7d");
  drawTextAt(4, 49, "TOK " + tokenLabel(), TFT_LIGHTGREY);

  const String current = fitText(app.statusSpeaker + ": " + app.statusText, 21);
  drawTextAt(4, 64, current, speakerColor(app.statusSpeaker));
  canvas.drawFastHLine(4, 77, canvas.width() - 8, rgb(42, 42, 42));

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
    drawTextAt(4, y, fitText(line, maxBodyChars()), bodyLineColor(line), isHeaderLine(line) ? rgb(17, 21, 24) : TFT_BLACK);
    y += bodyLineHeight();
  }

  String footer = app.settings.textNav == TEXT_NAV_PAGE ? "A down/page  B up/page" : "A down/line  B up/line";
  if (app.hasNew && app.scrollOffset > 0) {
    footer = "B hold newest";
  }
  drawTextAt(4, canvas.height() - 9, fitText(footer, 22), TFT_DARKGREY);
}

const char* brightnessName() {
  static const char* names[] = {"Low", "Med", "High"};
  return names[constrain(app.settings.brightness, 0, 2)];
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

const char* textSizeName() {
  return app.settings.textSize == TEXT_READABLE ? "Readable" : "Compact";
}

String settingLabel(uint8_t index) {
  switch (index) {
    case SETTING_BRIGHTNESS:
      return "Brightness " + String(brightnessName());
    case SETTING_SOUND:
      return "Sound " + String(soundName());
    case SETTING_TEXT_NAV:
      return "Text nav " + String(navName());
    case SETTING_TEXT_SIZE:
      return "Text size " + String(textSizeName());
    case SETTING_AUTO_NEWEST:
      return String("Auto newest ") + (app.settings.autoNewest ? "On" : "Off");
    default:
      return "";
  }
}

void drawSettings() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextSize(1);
  canvas.fillRect(0, 0, canvas.width(), 15, rgb(45, 45, 45));
  drawTextAt(4, 4, "SETTINGS", TFT_WHITE, rgb(45, 45, 45));

  for (uint8_t i = 0; i < SETTING_COUNT; ++i) {
    const uint8_t y = 25 + i * 16;
    const bool selected = i == app.settings.selected;
    const uint16_t fg = selected ? TFT_BLACK : TFT_LIGHTGREY;
    const uint16_t bg = selected ? rgb(120, 150, 145) : TFT_BLACK;
    if (selected) {
      canvas.fillRect(2, y - 2, canvas.width() - 4, 13, bg);
    }
    drawTextAt(6, y, fitText(settingLabel(i), 20), fg, bg);
  }

  const int pct = batteryPercent();
  drawTextAt(4, canvas.height() - 42, fitText(app.deviceName, 21), TFT_DARKGREY);
  drawTextAt(4, canvas.height() - 30, String("BLE ") + (bleConnected ? "on" : "off") + "  BAT " + (pct >= 0 ? String(pct) + "%" : "n/a"), TFT_DARKGREY);
  drawTextAt(4, canvas.height() - 18, "Heap " + formatCompact(ESP.getFreeHeap()), TFT_DARKGREY);
  drawTextAt(4, canvas.height() - 8, "A change  B next", TFT_DARKGREY);
}

void redraw() {
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
    case SETTING_SOUND:
      app.settings.sound = static_cast<SoundMode>((app.settings.sound + 1) % 3);
      requestCue(app.settings.sound == SOUND_OFF ? CUE_NONE : CUE_ACTIVITY);
      break;
    case SETTING_TEXT_NAV:
      app.settings.textNav = app.settings.textNav == TEXT_NAV_PAGE ? TEXT_NAV_LINE : TEXT_NAV_PAGE;
      clampScroll();
      break;
    case SETTING_TEXT_SIZE:
      app.settings.textSize = app.settings.textSize == TEXT_COMPACT ? TEXT_READABLE : TEXT_COMPACT;
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

void seedBody() {
  uint8_t added = appendActivityBlock("System", "boot", "Dashboard booted. Waiting for Codex Desktop observer.");
  applyNewLines(added);
}

}  // namespace

void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Speaker.begin();
  M5.Speaker.setVolume(24);
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setTextSize(1);

  prefs.begin("codexdash", false);
  loadSettings();
  applyBrightness();

  app.deviceName = "Codex-S3-" + chipSuffix();
  setupBle();
  setCurrentStatus("System", "advertise", "Advertise " + app.deviceName);
  seedBody();
  redraw();
}

void loop() {
  M5.update();
  handleButtons();
  playPendingCue();

  if (app.settings.open && millis() - app.settings.lastInputMs > SETTINGS_IDLE_MS) {
    closeSettings();
  }

  static uint32_t lastStatusCheckMs = 0;
  if (millis() - lastStatusCheckMs > 1000) {
    lastStatusCheckMs = millis();
    needsRedraw = true;
  }

  if (needsRedraw) {
    redraw();
  }

  delay(20);
}
