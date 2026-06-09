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
constexpr size_t ENTRY_COUNT = 3;

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
  txCharacteristic->setValue(line.c_str());
  txCharacteristic->notify();
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
    app.remainingPct = doc["rate_limit_remaining_percent"].as<int>();
  } else if (doc["remaining_pct"].is<int>()) {
    app.remainingPct = doc["remaining_pct"].as<int>();
  } else if (doc["remaining"].is<int>()) {
    app.remainingPct = doc["remaining"].as<int>();
  }

  if (doc["prompt"].is<JsonObject>()) {
    JsonObject prompt = doc["prompt"].as<JsonObject>();
    app.prompt.active = true;
    app.prompt.id = prompt["id"].as<String>();
    app.prompt.tool = prompt["tool"].as<String>();
    app.prompt.hint = fitText(prompt["hint"].as<String>(), 80);
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
  drawLine(24, "Usage", TFT_WHITE);
  drawLine(42, "Today: " + String(app.tokensToday), TFT_LIGHTGREY);
  drawLine(58, "Total: " + String(app.tokens), TFT_LIGHTGREY);
  if (app.remainingPct >= 0) {
    drawLine(74, "Remain: " + String(app.remainingPct) + "%", app.remainingPct < 20 ? TFT_ORANGE : TFT_GREEN);
  } else {
    drawLine(74, "Remain: host n/a", TFT_DARKGREY);
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
      drawDashboard();
      break;
    case 1:
      drawEntries();
      break;
    case 2:
      drawUsage();
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
      page = (page + 1) % 4;
      needsRedraw = true;
    }
  }

  if (M5.BtnB.wasPressed()) {
    if (app.prompt.active) {
      sendPermissionDecision("deny");
    } else {
      page = (page + 1) % 4;
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
