#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <M5Unified.h>

namespace {

constexpr const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

constexpr uint32_t STALE_AFTER_MS = 30000;
constexpr uint32_t REDRAW_EVERY_MS = 500;
constexpr uint32_t BLE_NOTIFY_CHUNK_DELAY_MS = 8;
constexpr size_t ENTRY_COUNT = 3;
constexpr size_t BLE_NOTIFY_CHUNK_SIZE = 20;
constexpr uint8_t PAGE_COUNT = 6;

BLECharacteristic* txCharacteristic = nullptr;
bool bleConnected = false;
bool needsRedraw = true;
uint8_t page = 0;
String rxBuffer;

struct PromptState {
  bool active = false;
  String id;
  String tool;
  String hint;
};

struct RateLimitWindowState {
  bool available = false;
  String label;
  int usedPct = -1;
  int remainingPct = -1;
  uint32_t windowMins = 0;
  uint32_t resetsAt = 0;
};

struct PlanState {
  bool available = false;
  String step;
  String status;
  uint16_t completed = 0;
  uint16_t total = 0;
};

struct GoalState {
  bool available = false;
  String objective;
  String status;
  uint32_t timeUsedSec = 0;
  uint32_t tokensUsed = 0;
  uint32_t tokenBudget = 0;
  bool hasTokenBudget = false;
};

struct AppState {
  String deviceName;
  String owner = "Simon";
  uint16_t total = 0;
  uint16_t running = 0;
  uint16_t waiting = 0;
  String msg = "Waiting for Codex";
  String entries[ENTRY_COUNT];
  uint32_t tokens = 0;
  uint32_t tokensToday = 0;
  int remainingPct = -1;
  RateLimitWindowState primaryLimit;
  RateLimitWindowState secondaryLimit;
  PlanState plan;
  GoalState goal;
  PromptState prompt;
  uint32_t lastSnapshotMs = 0;
  uint32_t approvals = 0;
  uint32_t denials = 0;
};

AppState app;

String chipSuffix() {
  const uint64_t chip = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned int>(chip & 0xFFFF));
  return String(suffix);
}

String fitText(String text, size_t limit) {
  text.replace("\n", " ");
  text.replace("\r", " ");
  text.trim();
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
  doc["n"] = 0;
  sendJson(doc);
}

void sendStatusAck() {
  JsonDocument doc;
  doc["ack"] = "status";
  doc["ok"] = true;

  JsonObject data = doc["data"].to<JsonObject>();
  data["name"] = app.deviceName;
  data["sec"] = false;

  JsonObject bat = data["bat"].to<JsonObject>();
  const int pct = batteryPercent();
  if (pct >= 0) {
    bat["pct"] = pct;
  }
  bat["usb"] = isUsbPowered();

  JsonObject sys = data["sys"].to<JsonObject>();
  sys["up"] = millis() / 1000;
  sys["heap"] = ESP.getFreeHeap();

  JsonObject stats = data["stats"].to<JsonObject>();
  stats["appr"] = app.approvals;
  stats["deny"] = app.denials;

  sendJson(doc);
}

void sendPermissionDecision(const char* decision) {
  if (!app.prompt.active || app.prompt.id.isEmpty()) {
    return;
  }

  JsonDocument doc;
  doc["cmd"] = "permission";
  doc["id"] = app.prompt.id;
  doc["decision"] = decision;
  sendJson(doc);

  if (String(decision) == "once") {
    app.approvals += 1;
    app.msg = "Approved once";
  } else {
    app.denials += 1;
    app.msg = "Denied";
  }
  app.prompt.active = false;
  needsRedraw = true;
}

void updateEntries(JsonArray entries) {
  for (size_t i = 0; i < ENTRY_COUNT; ++i) {
    app.entries[i] = "";
  }

  size_t index = 0;
  for (JsonVariant value : entries) {
    if (index >= ENTRY_COUNT) {
      break;
    }
    app.entries[index] = fitText(value.as<String>(), 30);
    index += 1;
  }
}

String durationLabel(uint32_t windowMins) {
  if (windowMins == 0) {
    return "limit";
  }
  if (windowMins % 1440 == 0) {
    return String(windowMins / 1440) + "d";
  }
  if (windowMins % 60 == 0) {
    return String(windowMins / 60) + "h";
  }
  return String(windowMins) + "m";
}

void updateRateLimitWindow(JsonObject source, RateLimitWindowState& target) {
  if (source.isNull()) {
    target.available = false;
    return;
  }

  target.available = true;
  if (source["window_mins"].is<uint32_t>()) {
    target.windowMins = source["window_mins"].as<uint32_t>();
  } else if (source["windowDurationMins"].is<uint32_t>()) {
    target.windowMins = source["windowDurationMins"].as<uint32_t>();
  }

  if (source["label"].is<const char*>()) {
    target.label = fitText(source["label"].as<String>(), 5);
  } else if (target.label.isEmpty()) {
    target.label = durationLabel(target.windowMins);
  }

  if (source["used_percent"].is<int>()) {
    target.usedPct = constrain(source["used_percent"].as<int>(), 0, 100);
    target.remainingPct = 100 - target.usedPct;
  } else if (source["usedPercent"].is<int>()) {
    target.usedPct = constrain(source["usedPercent"].as<int>(), 0, 100);
    target.remainingPct = 100 - target.usedPct;
  }

  if (source["remaining_percent"].is<int>()) {
    target.remainingPct = constrain(source["remaining_percent"].as<int>(), 0, 100);
  } else if (source["remainingPercent"].is<int>()) {
    target.remainingPct = constrain(source["remainingPercent"].as<int>(), 0, 100);
  }

  if (source["resets_at"].is<uint32_t>()) {
    target.resetsAt = source["resets_at"].as<uint32_t>();
  } else if (source["resetsAt"].is<uint32_t>()) {
    target.resetsAt = source["resetsAt"].as<uint32_t>();
  }
}

void updateRateLimits(JsonObject rateLimits) {
  if (rateLimits["primary"].is<JsonObject>()) {
    updateRateLimitWindow(rateLimits["primary"].as<JsonObject>(), app.primaryLimit);
  }
  if (rateLimits["secondary"].is<JsonObject>()) {
    updateRateLimitWindow(rateLimits["secondary"].as<JsonObject>(), app.secondaryLimit);
  }
}

void clearPlan() {
  app.plan.available = false;
  app.plan.step = "";
  app.plan.status = "";
  app.plan.completed = 0;
  app.plan.total = 0;
}

void updatePlan(JsonObject source) {
  if (source["available"].is<bool>() && !source["available"].as<bool>()) {
    clearPlan();
    return;
  }

  app.plan.available = true;
  if (source["step"].is<const char*>()) {
    app.plan.step = fitText(source["step"].as<String>(), 80);
  }
  if (source["status"].is<const char*>()) {
    app.plan.status = fitText(source["status"].as<String>(), 16);
  }
  if (source["completed"].is<int>()) {
    app.plan.completed = static_cast<uint16_t>(constrain(source["completed"].as<int>(), 0, 999));
  }
  if (source["total"].is<int>()) {
    app.plan.total = static_cast<uint16_t>(constrain(source["total"].as<int>(), 0, 999));
  }
}

void clearGoal() {
  app.goal.available = false;
  app.goal.objective = "";
  app.goal.status = "";
  app.goal.timeUsedSec = 0;
  app.goal.tokensUsed = 0;
  app.goal.tokenBudget = 0;
  app.goal.hasTokenBudget = false;
}

void updateGoal(JsonObject source) {
  if (source["available"].is<bool>() && !source["available"].as<bool>()) {
    clearGoal();
    return;
  }

  app.goal.available = true;
  app.goal.hasTokenBudget = false;
  if (source["objective"].is<const char*>()) {
    app.goal.objective = fitText(source["objective"].as<String>(), 96);
  }
  if (source["status"].is<const char*>()) {
    app.goal.status = fitText(source["status"].as<String>(), 16);
  }
  if (source["time_used_sec"].is<uint32_t>()) {
    app.goal.timeUsedSec = source["time_used_sec"].as<uint32_t>();
  } else if (source["timeUsedSeconds"].is<uint32_t>()) {
    app.goal.timeUsedSec = source["timeUsedSeconds"].as<uint32_t>();
  }
  if (source["tokens_used"].is<uint32_t>()) {
    app.goal.tokensUsed = source["tokens_used"].as<uint32_t>();
  } else if (source["tokensUsed"].is<uint32_t>()) {
    app.goal.tokensUsed = source["tokensUsed"].as<uint32_t>();
  }
  if (source["token_budget"].is<uint32_t>()) {
    app.goal.tokenBudget = source["token_budget"].as<uint32_t>();
    app.goal.hasTokenBudget = true;
  } else if (source["tokenBudget"].is<uint32_t>()) {
    app.goal.tokenBudget = source["tokenBudget"].as<uint32_t>();
    app.goal.hasTokenBudget = true;
  }
}

void handleSnapshot(JsonDocument& doc) {
  app.total = doc["total"] | app.total;
  app.running = doc["running"] | 0;
  app.waiting = doc["waiting"] | 0;
  app.tokens = doc["tokens"] | app.tokens;
  app.tokensToday = doc["tokens_today"] | app.tokensToday;

  if (doc["msg"].is<const char*>()) {
    app.msg = fitText(doc["msg"].as<String>(), 64);
  }

  if (doc["entries"].is<JsonArray>()) {
    updateEntries(doc["entries"].as<JsonArray>());
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

  if (doc["plan"].is<JsonObject>()) {
    updatePlan(doc["plan"].as<JsonObject>());
  }

  if (doc["goal"].is<JsonObject>()) {
    updateGoal(doc["goal"].as<JsonObject>());
  }

  if (doc["prompt"].is<JsonObject>()) {
    JsonObject prompt = doc["prompt"].as<JsonObject>();
    app.prompt.id = prompt["id"].as<String>();
    app.prompt.tool = prompt["tool"].as<String>();
    app.prompt.hint = fitText(prompt["hint"].as<String>(), 80);
    app.prompt.active = !app.prompt.id.isEmpty();
  } else if (app.waiting == 0) {
    app.prompt.active = false;
  }

  app.lastSnapshotMs = millis();
  needsRedraw = true;
}

void handleCommand(JsonDocument& doc) {
  const String command = doc["cmd"].as<String>();
  if (command == "status") {
    sendStatusAck();
    return;
  }
  if (command == "owner") {
    app.owner = fitText(doc["name"].as<String>(), 20);
    sendAck("owner", true);
    needsRedraw = true;
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
    app.msg = "JSON error";
    needsRedraw = true;
    return;
  }

  if (doc["cmd"].is<const char*>()) {
    handleCommand(doc);
    return;
  }

  if (doc["time"].is<JsonArray>()) {
    app.msg = "Time synced";
    needsRedraw = true;
    return;
  }

  handleSnapshot(doc);
}

void handleIncomingBytes(const std::string& value) {
  for (char c : value) {
    if (c == '\n') {
      const String line = rxBuffer;
      rxBuffer = "";
      if (!line.isEmpty()) {
        handleLine(line);
      }
      continue;
    }
    if (c != '\r') {
      rxBuffer += c;
    }
    if (rxBuffer.length() > 4096) {
      rxBuffer = "";
      app.msg = "RX overflow";
      needsRedraw = true;
    }
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    bleConnected = true;
    app.msg = "Connected";
    needsRedraw = true;
  }

  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    app.total = 0;
    app.running = 0;
    app.waiting = 0;
    clearPlan();
    clearGoal();
    app.prompt.active = false;
    app.msg = "Disconnected";
    BLEDevice::startAdvertising();
    needsRedraw = true;
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    handleIncomingBytes(characteristic->getValue());
  }
};

void setupBle() {
  BLEDevice::init(app.deviceName.c_str());
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* service = server->createService(NUS_SERVICE_UUID);
  txCharacteristic = service->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
  );
  txCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic* rxCharacteristic = service->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxCharacteristic->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(NUS_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

const char* statusText() {
  if (!bleConnected) {
    return "DISCONNECTED";
  }
  if (app.lastSnapshotMs > 0 && millis() - app.lastSnapshotMs > STALE_AFTER_MS) {
    return "STALE";
  }
  if (app.prompt.active || app.waiting > 0) {
    return "WAITING";
  }
  if (app.running > 0) {
    return "RUNNING";
  }
  return "IDLE";
}

uint16_t statusColor() {
  const char* status = statusText();
  if (strcmp(status, "RUNNING") == 0) {
    return TFT_CYAN;
  }
  if (strcmp(status, "WAITING") == 0) {
    return TFT_ORANGE;
  }
  if (strcmp(status, "IDLE") == 0) {
    return TFT_GREEN;
  }
  if (strcmp(status, "STALE") == 0) {
    return TFT_YELLOW;
  }
  return TFT_RED;
}

void drawHeader() {
  M5.Display.fillRect(0, 0, M5.Display.width(), 18, statusColor());
  M5.Display.setTextColor(TFT_BLACK, statusColor());
  M5.Display.setCursor(4, 5);
  M5.Display.print(statusText());
  M5.Display.setCursor(M5.Display.width() - 28, 5);
  M5.Display.printf("P%u", page + 1);
}

void drawFooter() {
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.setCursor(4, M5.Display.height() - 10);
  M5.Display.print("A ok/next  B no/next");
}

void drawLine(int y, const String& text, uint16_t color = TFT_WHITE) {
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.setCursor(4, y);
  M5.Display.print(fitText(text, 21));
}

String elapsedLabel(uint32_t seconds) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours > 0) {
    return String(hours) + "h " + String(minutes) + "m";
  }
  if (minutes > 0) {
    return String(minutes) + "m";
  }
  return String(seconds) + "s";
}

uint16_t statusTextColor(const String& status) {
  if (status == "active" || status == "inProgress" || status == "in_progress") {
    return TFT_CYAN;
  }
  if (status == "paused" || status == "pending") {
    return TFT_YELLOW;
  }
  if (status == "blocked" || status == "usageLimited" || status == "budgetLimited") {
    return TFT_ORANGE;
  }
  if (status == "complete" || status == "completed") {
    return TFT_GREEN;
  }
  return TFT_LIGHTGREY;
}

void drawDashboard() {
  drawLine(24, "Codex " + app.owner, TFT_WHITE);
  drawLine(38, "T:" + String(app.total) + " R:" + String(app.running) + " W:" + String(app.waiting), TFT_LIGHTGREY);
  drawLine(54, app.msg, app.prompt.active ? TFT_ORANGE : TFT_WHITE);

  if (app.prompt.active) {
    drawLine(72, app.prompt.tool, TFT_ORANGE);
    drawLine(86, app.prompt.hint, TFT_WHITE);
  } else {
    drawLine(72, app.entries[0], TFT_LIGHTGREY);
    drawLine(86, app.entries[1], TFT_DARKGREY);
  }
}

void drawEntries() {
  drawLine(24, "Recent", TFT_WHITE);
  for (size_t i = 0; i < ENTRY_COUNT; ++i) {
    drawLine(42 + static_cast<int>(i) * 16, String(i + 1) + " " + app.entries[i], TFT_LIGHTGREY);
  }
}

void drawUsage() {
  drawLine(24, "Limits", TFT_WHITE);

  if (app.primaryLimit.available || app.secondaryLimit.available) {
    if (app.primaryLimit.available) {
      const uint16_t color = app.primaryLimit.remainingPct < 20 ? TFT_ORANGE : TFT_GREEN;
      drawLine(42, app.primaryLimit.label + " left: " + String(app.primaryLimit.remainingPct) + "%", color);
    }
    if (app.secondaryLimit.available) {
      const uint16_t color = app.secondaryLimit.remainingPct < 20 ? TFT_ORANGE : TFT_GREEN;
      drawLine(58, app.secondaryLimit.label + " left: " + String(app.secondaryLimit.remainingPct) + "%", color);
    }
    drawLine(74, "Tokens: " + String(app.tokens), TFT_DARKGREY);
    drawLine(90, "Host rate limits", TFT_DARKGREY);
  } else if (app.remainingPct >= 0) {
    drawLine(42, "Remain: " + String(app.remainingPct) + "%", app.remainingPct < 20 ? TFT_ORANGE : TFT_GREEN);
    drawLine(58, "Tokens: " + String(app.tokens), TFT_LIGHTGREY);
  } else {
    drawLine(42, "5h left: host n/a", TFT_DARKGREY);
    drawLine(58, "7d left: host n/a", TFT_DARKGREY);
    drawLine(74, "Tokens: " + String(app.tokens), TFT_DARKGREY);
  }
}

void drawPlan() {
  drawLine(24, "Plan", TFT_WHITE);

  if (!app.plan.available) {
    drawLine(42, "Plan: host n/a", TFT_DARKGREY);
    drawLine(58, "Waiting for update", TFT_DARKGREY);
    return;
  }

  const String progress = String(app.plan.completed) + "/" + String(app.plan.total) + " " + app.plan.status;
  drawLine(42, progress, statusTextColor(app.plan.status));
  drawLine(58, app.plan.step, TFT_WHITE);
  drawLine(74, app.msg, TFT_DARKGREY);
}

void drawGoal() {
  drawLine(24, "Goal", TFT_WHITE);

  if (!app.goal.available) {
    drawLine(42, "Goal: host n/a", TFT_DARKGREY);
    drawLine(58, "No active goal", TFT_DARKGREY);
    return;
  }

  drawLine(42, app.goal.status, statusTextColor(app.goal.status));
  drawLine(58, app.goal.objective, TFT_WHITE);
  drawLine(74, "Time: " + elapsedLabel(app.goal.timeUsedSec), TFT_LIGHTGREY);
  if (app.goal.hasTokenBudget) {
    drawLine(90, "Tok: " + String(app.goal.tokensUsed) + "/" + String(app.goal.tokenBudget), TFT_DARKGREY);
  } else {
    drawLine(90, "Tok: " + String(app.goal.tokensUsed), TFT_DARKGREY);
  }
}

void drawSystem() {
  const int pct = batteryPercent();
  drawLine(24, app.deviceName, TFT_WHITE);
  drawLine(42, "BLE: " + String(bleConnected ? "on" : "off"), bleConnected ? TFT_GREEN : TFT_RED);
  drawLine(58, "Batt: " + String(pct >= 0 ? String(pct) + "%" : "n/a"), TFT_LIGHTGREY);
  drawLine(74, "USB: " + String(isUsbPowered() ? "yes" : "no"), TFT_LIGHTGREY);
  drawLine(90, "Heap: " + String(ESP.getFreeHeap()), TFT_DARKGREY);
}

void redraw() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(1);
  drawHeader();

  switch (page) {
    case 0:
      drawUsage();
      break;
    case 1:
      drawDashboard();
      break;
    case 2:
      drawPlan();
      break;
    case 3:
      drawGoal();
      break;
    case 4:
      drawEntries();
      break;
    default:
      drawSystem();
      break;
  }

  drawFooter();
  needsRedraw = false;
}

void handleButtons() {
  if (M5.BtnA.wasPressed()) {
    if (app.prompt.active) {
      sendPermissionDecision("once");
    } else {
      page = (page + 1) % PAGE_COUNT;
      needsRedraw = true;
    }
  }

  if (M5.BtnB.wasPressed()) {
    if (app.prompt.active) {
      sendPermissionDecision("deny");
    } else {
      page = (page + 1) % PAGE_COUNT;
      needsRedraw = true;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setTextSize(1);
  M5.Display.setBrightness(160);

  app.deviceName = "Codex-S3-" + chipSuffix();
  setupBle();

  app.msg = "Advertise " + app.deviceName;
  redraw();
}

void loop() {
  static uint32_t lastRedrawMs = 0;

  M5.update();
  handleButtons();

  const uint32_t now = millis();
  if (now - lastRedrawMs >= REDRAW_EVERY_MS) {
    lastRedrawMs = now;
    needsRedraw = true;
  }

  if (needsRedraw) {
    redraw();
  }

  delay(20);
}
