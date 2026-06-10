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
constexpr uint32_t PET_TICK_MS = 10000;
constexpr uint32_t PET_SAVE_MS = 60000;
constexpr uint32_t PET_ANIM_MS = 650;
constexpr uint32_t LONG_PRESS_MS = 650;
constexpr uint32_t DOUBLE_PRESS_MS = 280;
constexpr uint32_t COMBO_HOLD_MS = 850;
constexpr size_t ENTRY_COUNT = 3;
constexpr size_t OPTION_COUNT = 8;
constexpr size_t BLE_NOTIFY_CHUNK_SIZE = 20;
constexpr uint8_t PET_SCHEMA_VERSION = 1;

enum Page : uint8_t {
  PAGE_PET = 0,
  PAGE_CODEX,
  PAGE_LIMITS,
  PAGE_CARE,
  PAGE_SYSTEM,
  PAGE_COUNT
};

enum InputEvent : uint8_t {
  INPUT_NONE = 0,
  INPUT_A_SINGLE,
  INPUT_B_SINGLE,
  INPUT_A_DOUBLE,
  INPUT_B_DOUBLE,
  INPUT_A_LONG,
  INPUT_B_LONG,
  INPUT_AB_HOLD
};

NimBLECharacteristic* txCharacteristic = nullptr;
Preferences prefs;
M5Canvas canvas(&M5.Display);
bool bleConnected = false;
bool needsRedraw = true;
uint8_t page = PAGE_PET;
String rxBuffer;
String lastRenderedStatus;

struct ButtonTracker {
  bool down = false;
  bool longSent = false;
  bool pendingSingle = false;
  uint32_t pressMs = 0;
  uint32_t lastReleaseMs = 0;
};

ButtonTracker buttonA;
ButtonTracker buttonB;
bool comboSent = false;
bool comboActive = false;
uint32_t comboStartMs = 0;

struct PromptState {
  bool active = false;
  String id;
  String tool;
  String hint;
};

struct InteractionOption {
  String id;
  String label;
  bool selected = false;
};

struct InteractionState {
  bool active = false;
  bool multi = false;
  bool handoff = false;
  bool details = false;
  String id;
  String kind;
  String title;
  String body;
  String questionId;
  InteractionOption options[OPTION_COUNT];
  uint8_t optionCount = 0;
  uint8_t selected = 0;
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
  bool hasTokensUsed = false;
  bool hasTokenBudget = false;
};

struct PetState {
  uint8_t mood = 72;
  uint8_t energy = 76;
  uint8_t hunger = 68;
  uint8_t cleanliness = 74;
  uint8_t bond = 12;
  uint8_t focus = 50;
  uint32_t interactions = 0;
  uint32_t playCount = 0;
  uint32_t createdSec = 0;
  uint32_t lastTickMs = 0;
  uint32_t lastSaveMs = 0;
  uint32_t lastAnimMs = 0;
  uint8_t animFrame = 0;
  uint8_t careIndex = 0;
  bool sleeping = false;
  bool dirty = false;
  String reaction = "Ready";
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
  bool hasTokens = false;
  bool hasTokensToday = false;
  int remainingPct = -1;
  RateLimitWindowState primaryLimit;
  RateLimitWindowState secondaryLimit;
  PlanState plan;
  GoalState goal;
  PromptState prompt;
  InteractionState interaction;
  PetState pet;
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

uint8_t clampStat(int value) {
  return static_cast<uint8_t>(constrain(value, 0, 100));
}

void markPetDirty() {
  app.pet.dirty = true;
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

void savePet(bool force = false) {
  if (!force && (!app.pet.dirty || millis() - app.pet.lastSaveMs < PET_SAVE_MS)) {
    return;
  }
  prefs.putUChar("schema", PET_SCHEMA_VERSION);
  prefs.putUChar("mood", app.pet.mood);
  prefs.putUChar("energy", app.pet.energy);
  prefs.putUChar("hunger", app.pet.hunger);
  prefs.putUChar("clean", app.pet.cleanliness);
  prefs.putUChar("bond", app.pet.bond);
  prefs.putUChar("focus", app.pet.focus);
  prefs.putUInt("interact", app.pet.interactions);
  prefs.putUInt("plays", app.pet.playCount);
  prefs.putUInt("created", app.pet.createdSec);
  prefs.putBool("sleeping", app.pet.sleeping);
  app.pet.lastSaveMs = millis();
  app.pet.dirty = false;
}

void loadPet() {
  const uint8_t schema = prefs.getUChar("schema", 0);
  if (schema != PET_SCHEMA_VERSION) {
    app.pet.createdSec = millis() / 1000;
    app.pet.reaction = "Born";
    markPetDirty();
    savePet(true);
    return;
  }
  app.pet.mood = prefs.getUChar("mood", app.pet.mood);
  app.pet.energy = prefs.getUChar("energy", app.pet.energy);
  app.pet.hunger = prefs.getUChar("hunger", app.pet.hunger);
  app.pet.cleanliness = prefs.getUChar("clean", app.pet.cleanliness);
  app.pet.bond = prefs.getUChar("bond", app.pet.bond);
  app.pet.focus = prefs.getUChar("focus", app.pet.focus);
  app.pet.interactions = prefs.getUInt("interact", 0);
  app.pet.playCount = prefs.getUInt("plays", 0);
  app.pet.createdSec = prefs.getUInt("created", millis() / 1000);
  app.pet.sleeping = prefs.getBool("sleeping", false);
  app.pet.reaction = app.pet.sleeping ? "Sleeping" : "Ready";
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

  JsonObject pet = data["pet"].to<JsonObject>();
  pet["mood"] = app.pet.mood;
  pet["energy"] = app.pet.energy;
  pet["bond"] = app.pet.bond;
  pet["focus"] = app.pet.focus;
  pet["sleeping"] = app.pet.sleeping;

  sendJson(doc);
}

void clearInteraction() {
  app.interaction.active = false;
  app.interaction.multi = false;
  app.interaction.handoff = false;
  app.interaction.details = false;
  app.interaction.id = "";
  app.interaction.kind = "";
  app.interaction.title = "";
  app.interaction.body = "";
  app.interaction.questionId = "";
  app.interaction.optionCount = 0;
  app.interaction.selected = 0;
  for (size_t i = 0; i < OPTION_COUNT; ++i) {
    app.interaction.options[i].id = "";
    app.interaction.options[i].label = "";
    app.interaction.options[i].selected = false;
  }
}

void sendInteraction(const char* action, const String& value = "") {
  if (!app.interaction.active || app.interaction.id.isEmpty()) {
    return;
  }

  JsonDocument doc;
  doc["cmd"] = "interaction";
  doc["id"] = app.interaction.id;
  doc["action"] = action;
  if (!value.isEmpty()) {
    doc["value"] = value;
  }
  sendJson(doc);
}

void finishInteraction(const String& message, bool positive, bool countDecision = false) {
  if (positive) {
    app.pet.mood = clampStat(app.pet.mood + 4);
    app.pet.bond = clampStat(app.pet.bond + 2);
    if (countDecision) {
      app.approvals += 1;
    }
  } else {
    app.pet.focus = clampStat(app.pet.focus - 3);
    if (countDecision) {
      app.denials += 1;
    }
  }
  app.msg = message;
  app.pet.reaction = message;
  clearInteraction();
  markPetDirty();
  needsRedraw = true;
}

void submitApproval(const char* decision) {
  sendInteraction("submit", decision);
  const String value(decision);
  if (value == "once") {
    finishInteraction("Approved once", true, true);
  } else if (value == "session") {
    finishInteraction("Approved session", true, true);
  } else if (value == "cancel") {
    finishInteraction("Cancelled", false, true);
  } else {
    finishInteraction("Declined", false, true);
  }
}

void submitChoice() {
  if (!app.interaction.active || app.interaction.optionCount == 0) {
    return;
  }
  if (app.interaction.handoff) {
    sendInteraction("handoff", "handoff");
    finishInteraction("Use Mac", false);
    return;
  }
  const InteractionOption& option = app.interaction.options[app.interaction.selected];
  sendInteraction("submit", option.id);
  app.pet.mood = clampStat(app.pet.mood + 2);
  app.pet.focus = clampStat(app.pet.focus + 1);
  finishInteraction("Choice sent", true);
}

void sendControl(const char* action) {
  JsonDocument doc;
  doc["cmd"] = "control";
  doc["action"] = action;
  sendJson(doc);
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
  app.goal.hasTokensUsed = false;
  app.goal.hasTokenBudget = false;
}

void updateGoal(JsonObject source) {
  if (source["available"].is<bool>() && !source["available"].as<bool>()) {
    clearGoal();
    return;
  }

  app.goal.available = true;
  app.goal.hasTokensUsed = false;
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
    app.goal.hasTokensUsed = true;
  } else if (source["tokensUsed"].is<uint32_t>()) {
    app.goal.tokensUsed = source["tokensUsed"].as<uint32_t>();
    app.goal.hasTokensUsed = true;
  }
  if (source["token_budget"].is<uint32_t>()) {
    app.goal.tokenBudget = source["token_budget"].as<uint32_t>();
    app.goal.hasTokenBudget = true;
  } else if (source["tokenBudget"].is<uint32_t>()) {
    app.goal.tokenBudget = source["tokenBudget"].as<uint32_t>();
    app.goal.hasTokenBudget = true;
  }
}

void setInteractionOption(uint8_t index, const String& id, const String& label) {
  if (index >= OPTION_COUNT) {
    return;
  }
  app.interaction.options[index].id = fitText(id, 24);
  app.interaction.options[index].label = fitText(label, 18);
  app.interaction.options[index].selected = false;
}

void setDefaultApprovalOptions() {
  app.interaction.optionCount = 4;
  setInteractionOption(0, "once", "Once");
  setInteractionOption(1, "session", "Session");
  setInteractionOption(2, "deny", "Deny");
  setInteractionOption(3, "cancel", "Cancel");
}

void updateInteraction(JsonObject source) {
  clearInteraction();
  app.interaction.id = source["id"].as<String>();
  app.interaction.kind = fitText(source["kind"].as<String>(), 16);
  app.interaction.title = fitText(source["title"].as<String>(), 24);
  app.interaction.body = fitText(source["body"].as<String>(), 96);
  app.interaction.questionId = fitText(source["question_id"].as<String>(), 24);
  app.interaction.multi = source["multi"] | false;
  app.interaction.handoff = source["handoff"] | false;
  app.interaction.selected = static_cast<uint8_t>(constrain(source["selected"] | 0, 0, OPTION_COUNT - 1));

  if (source["options"].is<JsonArray>()) {
    JsonArray options = source["options"].as<JsonArray>();
    uint8_t index = 0;
    for (JsonVariant value : options) {
      if (index >= OPTION_COUNT || !value.is<JsonObject>()) {
        break;
      }
      JsonObject option = value.as<JsonObject>();
      const String id = option["id"].is<const char*>() ? option["id"].as<String>() : option["label"].as<String>();
      const String label = option["label"].is<const char*>() ? option["label"].as<String>() : id;
      setInteractionOption(index, id, label);
      index += 1;
    }
    app.interaction.optionCount = index;
  }

  if (app.interaction.kind == "approval" && app.interaction.optionCount == 0) {
    setDefaultApprovalOptions();
  }
  if (app.interaction.optionCount > 0 && app.interaction.selected >= app.interaction.optionCount) {
    app.interaction.selected = 0;
  }
  app.interaction.active = !app.interaction.id.isEmpty();
}

void updateLegacyPrompt(JsonObject prompt) {
  app.prompt.id = prompt["id"].as<String>();
  app.prompt.tool = prompt["tool"].as<String>();
  app.prompt.hint = fitText(prompt["hint"].as<String>(), 80);
  app.prompt.active = !app.prompt.id.isEmpty();
  if (!app.prompt.active) {
    clearInteraction();
    return;
  }

  clearInteraction();
  app.interaction.active = true;
  app.interaction.id = app.prompt.id;
  app.interaction.kind = "approval";
  app.interaction.title = fitText(app.prompt.tool, 24);
  app.interaction.body = fitText(app.prompt.hint, 96);
  setDefaultApprovalOptions();
}

void handleSnapshot(JsonDocument& doc) {
  app.total = doc["total"] | app.total;
  app.running = doc["running"] | 0;
  app.waiting = doc["waiting"] | 0;
  if (doc["tokens"].is<uint32_t>()) {
    app.tokens = doc["tokens"].as<uint32_t>();
    app.hasTokens = true;
  }
  if (doc["tokens_today"].is<uint32_t>()) {
    app.tokensToday = doc["tokens_today"].as<uint32_t>();
    app.hasTokensToday = true;
  }

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

  if (doc["interaction"].is<JsonObject>()) {
    updateInteraction(doc["interaction"].as<JsonObject>());
    app.prompt.active = false;
  } else if (doc["prompt"].is<JsonObject>()) {
    updateLegacyPrompt(doc["prompt"].as<JsonObject>());
  } else if (app.waiting == 0) {
    app.prompt.active = false;
    clearInteraction();
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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    bleConnected = true;
    app.msg = "Connected";
    app.pet.reaction = "Linked";
    needsRedraw = true;
  }

  void onDisconnect(NimBLEServer*) override {
    bleConnected = false;
    app.total = 0;
    app.running = 0;
    app.waiting = 0;
    clearPlan();
    clearGoal();
    app.prompt.active = false;
    clearInteraction();
    app.msg = "Disconnected";
    app.pet.reaction = "Offline";
    NimBLEDevice::startAdvertising();
    needsRedraw = true;
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    handleIncomingBytes(characteristic->getValue());
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

const char* statusText() {
  if (!bleConnected) {
    return "OFFLINE";
  }
  if (app.lastSnapshotMs > 0 && millis() - app.lastSnapshotMs > STALE_AFTER_MS) {
    return "STALE";
  }
  if (app.interaction.active || app.waiting > 0) {
    return "WAIT";
  }
  if (app.running > 0) {
    return "WORK";
  }
  return "IDLE";
}

uint16_t statusColor() {
  const char* status = statusText();
  if (strcmp(status, "WORK") == 0) {
    return TFT_CYAN;
  }
  if (strcmp(status, "WAIT") == 0) {
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

const char* pageName() {
  switch (page) {
    case PAGE_PET:
      return "Blob";
    case PAGE_CODEX:
      return "Codex";
    case PAGE_LIMITS:
      return "Limits";
    case PAGE_CARE:
      return "Care";
    default:
      return "System";
  }
}

uint16_t limitColor(int remainingPct) {
  if (remainingPct < 0) {
    return TFT_DARKGREY;
  }
  if (remainingPct < 15) {
    return TFT_RED;
  }
  if (remainingPct < 35) {
    return TFT_ORANGE;
  }
  if (remainingPct < 60) {
    return TFT_YELLOW;
  }
  return TFT_GREEN;
}

void drawHeader() {
  canvas.fillRect(0, 0, canvas.width(), 16, statusColor());
  canvas.setTextColor(TFT_BLACK, statusColor());
  canvas.setCursor(4, 4);
  canvas.print(statusText());
  canvas.setCursor(58, 4);
  canvas.print(pageName());
  canvas.setCursor(canvas.width() - 18, 4);
  canvas.printf("%u", page + 1);
}

void drawLine(int y, const String& text, uint16_t color = TFT_WHITE) {
  canvas.setTextColor(color, TFT_BLACK);
  canvas.setCursor(4, y);
  canvas.print(fitText(text, 21));
}

void drawBar(int x, int y, int w, int h, int value, uint16_t color, const String& label) {
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.setCursor(x, y - 10);
  canvas.print(fitText(label, 7));
  canvas.drawRect(x + 26, y - 1, w, h + 2, TFT_DARKGREY);
  if (value >= 0) {
    const int fill = map(constrain(value, 0, 100), 0, 100, 0, w - 2);
    canvas.fillRect(x + 27, y, fill, h, color);
  }
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

String tokensLabel() {
  return app.hasTokens ? String(app.tokens) : "n/a";
}

String blobMoodLabel() {
  if (app.pet.sleeping) {
    return "Sleeping";
  }
  if (app.interaction.active) {
    return "Listening";
  }
  if (!bleConnected) {
    return "Offline";
  }
  if (app.running > 0) {
    return "Working";
  }
  if (app.pet.energy < 25) {
    return "Sleepy";
  }
  if (app.pet.hunger < 25) {
    return "Hungry";
  }
  if (app.pet.cleanliness < 25) {
    return "Messy";
  }
  if (app.pet.mood > 75) {
    return "Happy";
  }
  return "Ready";
}

uint16_t blobColor() {
  if (app.pet.sleeping) {
    return TFT_DARKCYAN;
  }
  if (app.interaction.active) {
    return TFT_ORANGE;
  }
  if (app.running > 0) {
    return TFT_CYAN;
  }
  if (!bleConnected) {
    return TFT_PURPLE;
  }
  if (app.pet.energy < 25 || app.pet.hunger < 25 || app.pet.cleanliness < 25) {
    return TFT_YELLOW;
  }
  return TFT_GREEN;
}

void drawBlob(int cx, int cy, uint8_t scale = 1) {
  const int bob = app.pet.sleeping ? 0 : (app.pet.animFrame % 2 == 0 ? 0 : -2);
  const uint16_t color = blobColor();
  const int r = 22 * scale;
  canvas.fillCircle(cx, cy + bob, r, color);
  canvas.fillRect(cx - r, cy + bob, r * 2, 18 * scale, color);
  canvas.fillCircle(cx - r, cy + 18 * scale + bob, 5 * scale, color);
  canvas.fillCircle(cx + r, cy + 18 * scale + bob, 5 * scale, color);

  const bool blink = !app.pet.sleeping && app.pet.animFrame % 5 == 4;
  canvas.fillCircle(cx - 8 * scale, cy - 2 * scale + bob, 3 * scale, TFT_BLACK);
  canvas.fillCircle(cx + 8 * scale, cy - 2 * scale + bob, 3 * scale, TFT_BLACK);
  if (blink) {
    canvas.drawLine(cx - 11 * scale, cy - 2 * scale + bob, cx - 5 * scale, cy - 2 * scale + bob, TFT_BLACK);
    canvas.drawLine(cx + 5 * scale, cy - 2 * scale + bob, cx + 11 * scale, cy - 2 * scale + bob, TFT_BLACK);
  }
  if (app.pet.sleeping) {
    canvas.drawLine(cx - 10 * scale, cy + 10 * scale + bob, cx + 10 * scale, cy + 10 * scale + bob, TFT_BLACK);
  } else if (app.interaction.active || app.pet.mood < 35) {
    canvas.drawCircle(cx, cy + 10 * scale + bob, 4 * scale, TFT_BLACK);
  } else {
    canvas.drawLine(cx - 8 * scale, cy + 8 * scale + bob, cx - 2 * scale, cy + 12 * scale + bob, TFT_BLACK);
    canvas.drawLine(cx - 2 * scale, cy + 12 * scale + bob, cx + 8 * scale, cy + 8 * scale + bob, TFT_BLACK);
  }
}

void drawLimitBars(int y, bool large) {
  const int width = large ? 86 : 62;
  const int height = large ? 8 : 5;
  if (app.primaryLimit.available) {
    drawBar(8, y, width, height, app.primaryLimit.remainingPct, limitColor(app.primaryLimit.remainingPct), app.primaryLimit.label);
  } else {
    drawBar(8, y, width, height, -1, TFT_DARKGREY, "5h");
  }
  if (app.secondaryLimit.available) {
    drawBar(8, y + (large ? 24 : 16), width, height, app.secondaryLimit.remainingPct, limitColor(app.secondaryLimit.remainingPct), app.secondaryLimit.label);
  } else {
    drawBar(8, y + (large ? 24 : 16), width, height, -1, TFT_DARKGREY, "7d");
  }
}

void drawPetHome() {
  drawLine(20, "Agent Blob", TFT_WHITE);
  drawBlob(canvas.width() / 2, 58);
  drawLine(90, blobMoodLabel(), TFT_LIGHTGREY);
  drawLimitBars(112, false);
}

void drawCodex() {
  drawLine(20, "Codex " + app.owner, TFT_WHITE);
  drawLine(34, "T:" + String(app.total) + " R:" + String(app.running) + " W:" + String(app.waiting), TFT_LIGHTGREY);
  drawLine(48, app.msg, app.interaction.active ? TFT_ORANGE : TFT_WHITE);
  if (app.plan.available) {
    drawLine(64, String(app.plan.completed) + "/" + String(app.plan.total) + " " + app.plan.status, statusTextColor(app.plan.status));
    drawLine(78, app.plan.step, TFT_WHITE);
  } else {
    drawLine(64, app.entries[0], TFT_LIGHTGREY);
    drawLine(78, app.entries[1], TFT_DARKGREY);
  }
  if (app.goal.available) {
    drawLine(94, "Goal: " + app.goal.status, statusTextColor(app.goal.status));
  } else {
    drawLine(94, "Tok: " + tokensLabel(), TFT_DARKGREY);
  }
}

void drawLimits() {
  drawLine(20, "Codex limits", TFT_WHITE);
  drawLimitBars(48, true);
  drawLine(98, "Tok: " + tokensLabel(), TFT_DARKGREY);
  drawLine(112, "Bars show remaining", TFT_DARKGREY);
}

void drawStatBar(int y, const String& label, uint8_t value, uint16_t color) {
  drawBar(4, y, 86, 6, value, color, label);
}

void drawCare() {
  static const char* actions[] = {"Feed", "Clean", "Nap", "Focus"};
  drawLine(20, "Care: " + String(actions[app.pet.careIndex]), TFT_WHITE);
  drawStatBar(42, "Mood", app.pet.mood, TFT_GREEN);
  drawStatBar(56, "Energy", app.pet.energy, TFT_CYAN);
  drawStatBar(70, "Food", app.pet.hunger, TFT_YELLOW);
  drawStatBar(84, "Clean", app.pet.cleanliness, TFT_BLUE);
  drawStatBar(98, "Bond", app.pet.bond, TFT_MAGENTA);
  drawStatBar(112, "Focus", app.pet.focus, TFT_ORANGE);
}

void drawSystem() {
  const int pct = batteryPercent();
  drawLine(20, app.deviceName, TFT_WHITE);
  drawLine(36, "BLE: " + String(bleConnected ? "on" : "off"), bleConnected ? TFT_GREEN : TFT_RED);
  drawLine(52, "Batt: " + String(pct >= 0 ? String(pct) + "%" : "n/a"), TFT_LIGHTGREY);
  drawLine(68, "USB: " + String(isUsbPowered() ? "yes" : "no"), TFT_LIGHTGREY);
  drawLine(84, "Heap: " + String(ESP.getFreeHeap()), TFT_DARKGREY);
  drawLine(100, "Pet saves: NVS", TFT_DARKGREY);
}

void drawFooter() {
  canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
  canvas.setCursor(4, canvas.height() - 8);
  if (app.interaction.active) {
    if (app.interaction.kind == "approval") {
      canvas.print("A once A+ sess B deny");
    } else if (app.interaction.handoff) {
      canvas.print("A handoff B cancel");
    } else {
      canvas.print("A select B next");
    }
    return;
  }
  if (page == PAGE_PET) {
    canvas.print("A pet  B next");
  } else if (page == PAGE_CARE) {
    canvas.print("A do  B action");
  } else {
    canvas.print("A refresh  B next");
  }
}

void drawInteractionOverlay() {
  if (!app.interaction.active) {
    return;
  }

  canvas.fillRect(2, 18, canvas.width() - 4, 94, TFT_BLACK);
  canvas.drawRect(2, 18, canvas.width() - 4, 94, app.interaction.handoff ? TFT_YELLOW : TFT_ORANGE);
  drawBlob(24, 47);
  drawLine(22, app.interaction.title, TFT_ORANGE);
  drawLine(36, app.interaction.details ? app.interaction.body : fitText(app.interaction.body, 42), TFT_WHITE);

  if (app.interaction.kind == "approval") {
    drawLine(58, "A Once", TFT_GREEN);
    drawLine(72, "A long Session", TFT_CYAN);
    drawLine(86, "B Deny", TFT_ORANGE);
    drawLine(100, "B long Cancel", TFT_RED);
    return;
  }

  if (app.interaction.handoff) {
    drawLine(62, "Needs Mac", TFT_YELLOW);
    drawLine(78, "A: handoff", TFT_LIGHTGREY);
    drawLine(92, "B: cancel", TFT_LIGHTGREY);
    return;
  }

  for (uint8_t i = 0; i < min<uint8_t>(app.interaction.optionCount, 3); ++i) {
    const uint8_t optionIndex = (app.interaction.selected + i) % app.interaction.optionCount;
    const uint16_t color = i == 0 ? TFT_GREEN : TFT_LIGHTGREY;
    const String prefix = i == 0 ? "> " : "  ";
    drawLine(62 + i * 14, prefix + app.interaction.options[optionIndex].label, color);
  }
}

void redraw() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextSize(1);
  drawHeader();

  switch (page) {
    case PAGE_PET:
      drawPetHome();
      break;
    case PAGE_CODEX:
      drawCodex();
      break;
    case PAGE_LIMITS:
      drawLimits();
      break;
    case PAGE_CARE:
      drawCare();
      break;
    default:
      drawSystem();
      break;
  }

  drawInteractionOverlay();
  drawFooter();
  lastRenderedStatus = statusText();
  canvas.pushSprite(0, 0);
  needsRedraw = false;
}

void nextPage() {
  page = (page + 1) % PAGE_COUNT;
  needsRedraw = true;
}

void prevPage() {
  page = page == 0 ? PAGE_COUNT - 1 : page - 1;
  needsRedraw = true;
}

void petTouch() {
  app.pet.interactions += 1;
  app.pet.mood = clampStat(app.pet.mood + 4);
  app.pet.bond = clampStat(app.pet.bond + 1);
  app.pet.energy = clampStat(app.pet.energy - 1);
  app.pet.reaction = "Bloop";
  markPetDirty();
  needsRedraw = true;
}

void petPlay() {
  app.pet.interactions += 1;
  app.pet.playCount += 1;
  app.pet.mood = clampStat(app.pet.mood + 8);
  app.pet.bond = clampStat(app.pet.bond + 2);
  app.pet.energy = clampStat(app.pet.energy - 6);
  app.pet.hunger = clampStat(app.pet.hunger - 3);
  app.pet.cleanliness = clampStat(app.pet.cleanliness - 2);
  app.pet.reaction = "Bounce";
  markPetDirty();
  needsRedraw = true;
}

void toggleSleep() {
  app.pet.sleeping = !app.pet.sleeping;
  app.pet.reaction = app.pet.sleeping ? "Sleeping" : "Awake";
  markPetDirty();
  needsRedraw = true;
}

void applyCareAction() {
  static const char* actions[] = {"Fed", "Clean", "Nap", "Focus"};
  switch (app.pet.careIndex) {
    case 0:
      app.pet.hunger = clampStat(app.pet.hunger + 18);
      app.pet.mood = clampStat(app.pet.mood + 2);
      break;
    case 1:
      app.pet.cleanliness = clampStat(app.pet.cleanliness + 20);
      app.pet.mood = clampStat(app.pet.mood + 2);
      break;
    case 2:
      app.pet.energy = clampStat(app.pet.energy + 18);
      app.pet.sleeping = true;
      break;
    default:
      app.pet.focus = clampStat(app.pet.focus + 15);
      app.pet.energy = clampStat(app.pet.energy - 3);
      break;
  }
  app.pet.interactions += 1;
  app.pet.reaction = actions[app.pet.careIndex];
  markPetDirty();
  needsRedraw = true;
}

void refreshStatus() {
  JsonDocument doc;
  doc["cmd"] = "status";
  sendJson(doc);
  app.msg = "Refresh sent";
  needsRedraw = true;
}

void dispatchInteraction(InputEvent event) {
  if (!app.interaction.active) {
    return;
  }

  if (event == INPUT_AB_HOLD) {
    sendControl("interrupt");
    if (app.interaction.kind == "approval") {
      submitApproval("cancel");
    } else {
      sendInteraction("cancel", "cancel");
      finishInteraction("Cancelled", false);
    }
    return;
  }

  if (app.interaction.kind == "approval") {
    switch (event) {
      case INPUT_A_SINGLE:
        submitApproval("once");
        break;
      case INPUT_A_LONG:
        submitApproval("session");
        break;
      case INPUT_B_SINGLE:
        submitApproval("deny");
        break;
      case INPUT_B_LONG:
        submitApproval("cancel");
        break;
      case INPUT_A_DOUBLE:
      case INPUT_B_DOUBLE:
        app.interaction.details = !app.interaction.details;
        needsRedraw = true;
        break;
      default:
        break;
    }
    return;
  }

  switch (event) {
    case INPUT_A_SINGLE:
      if (app.interaction.handoff) {
        sendInteraction("handoff", "handoff");
        finishInteraction("Use Mac", false);
      } else if (app.interaction.optionCount > 0) {
        app.interaction.options[app.interaction.selected].selected = !app.interaction.options[app.interaction.selected].selected;
        app.msg = "Selected " + app.interaction.options[app.interaction.selected].label;
        needsRedraw = true;
      }
      break;
    case INPUT_A_DOUBLE:
    case INPUT_A_LONG:
      submitChoice();
      break;
    case INPUT_B_SINGLE:
      if (app.interaction.optionCount > 0) {
        app.interaction.selected = (app.interaction.selected + 1) % app.interaction.optionCount;
        needsRedraw = true;
      }
      break;
    case INPUT_B_DOUBLE:
      if (app.interaction.optionCount > 0) {
        app.interaction.selected = app.interaction.selected == 0 ? app.interaction.optionCount - 1 : app.interaction.selected - 1;
        needsRedraw = true;
      }
      break;
    case INPUT_B_LONG:
      sendInteraction("cancel", "cancel");
      finishInteraction("Cancelled", false);
      break;
    default:
      break;
  }
}

void dispatchInput(InputEvent event) {
  if (event == INPUT_NONE) {
    return;
  }
  if (app.interaction.active) {
    dispatchInteraction(event);
    return;
  }
  if (event == INPUT_AB_HOLD) {
    if (bleConnected) {
      sendControl("interrupt");
      app.msg = "Interrupt sent";
    } else {
      page = PAGE_SYSTEM;
    }
    needsRedraw = true;
    return;
  }

  switch (page) {
    case PAGE_PET:
      if (event == INPUT_A_SINGLE) {
        petTouch();
      } else if (event == INPUT_A_DOUBLE) {
        petPlay();
      } else if (event == INPUT_A_LONG) {
        page = PAGE_CARE;
        needsRedraw = true;
      } else if (event == INPUT_B_SINGLE) {
        nextPage();
      } else if (event == INPUT_B_DOUBLE) {
        prevPage();
      } else if (event == INPUT_B_LONG) {
        toggleSleep();
      }
      break;
    case PAGE_CARE:
      if (event == INPUT_A_SINGLE || event == INPUT_A_LONG) {
        applyCareAction();
      } else if (event == INPUT_A_DOUBLE) {
        petPlay();
      } else if (event == INPUT_B_SINGLE) {
        app.pet.careIndex = (app.pet.careIndex + 1) % 4;
        needsRedraw = true;
      } else if (event == INPUT_B_DOUBLE) {
        app.pet.careIndex = app.pet.careIndex == 0 ? 3 : app.pet.careIndex - 1;
        needsRedraw = true;
      } else if (event == INPUT_B_LONG) {
        page = PAGE_PET;
        needsRedraw = true;
      }
      break;
    case PAGE_CODEX:
    case PAGE_LIMITS:
    case PAGE_SYSTEM:
      if (event == INPUT_A_SINGLE || event == INPUT_A_LONG) {
        refreshStatus();
      } else if (event == INPUT_A_DOUBLE) {
        page = PAGE_CODEX;
        needsRedraw = true;
      } else if (event == INPUT_B_SINGLE) {
        nextPage();
      } else if (event == INPUT_B_DOUBLE) {
        prevPage();
      } else if (event == INPUT_B_LONG) {
        page = PAGE_PET;
        needsRedraw = true;
      }
      break;
  }
}

void updateButtonTracker(
  ButtonTracker& tracker,
  bool pressed,
  InputEvent singleEvent,
  InputEvent doubleEvent,
  InputEvent longEvent,
  bool suppressIndividual
) {
  const uint32_t now = millis();
  if (pressed && !tracker.down) {
    tracker.down = true;
    tracker.longSent = false;
    tracker.pressMs = now;
  }

  if (suppressIndividual) {
    tracker.pendingSingle = false;
  }

  if (pressed && tracker.down && !tracker.longSent && now - tracker.pressMs >= LONG_PRESS_MS && !comboSent && !suppressIndividual) {
    tracker.longSent = true;
    tracker.pendingSingle = false;
    dispatchInput(longEvent);
  }

  if (!pressed && tracker.down) {
    tracker.down = false;
    if (!tracker.longSent && !comboSent && !suppressIndividual) {
      if (tracker.pendingSingle && now - tracker.lastReleaseMs <= DOUBLE_PRESS_MS) {
        tracker.pendingSingle = false;
        dispatchInput(doubleEvent);
      } else {
        tracker.pendingSingle = true;
        tracker.lastReleaseMs = now;
      }
    }
  }

  if (tracker.pendingSingle && now - tracker.lastReleaseMs > DOUBLE_PRESS_MS && !suppressIndividual) {
    tracker.pendingSingle = false;
    dispatchInput(singleEvent);
  }
}

void handleButtons() {
  const bool aPressed = M5.BtnA.isPressed();
  const bool bPressed = M5.BtnB.isPressed();
  const bool bothPressed = aPressed && bPressed;
  const uint32_t now = millis();

  if (bothPressed) {
    if (!comboActive) {
      comboActive = true;
      comboStartMs = now;
      buttonA.pendingSingle = false;
      buttonB.pendingSingle = false;
    }
    if (!comboSent && now - comboStartMs >= COMBO_HOLD_MS) {
      comboSent = true;
      buttonA.pendingSingle = false;
      buttonB.pendingSingle = false;
      buttonA.longSent = true;
      buttonB.longSent = true;
      dispatchInput(INPUT_AB_HOLD);
    }
  }

  updateButtonTracker(buttonA, aPressed, INPUT_A_SINGLE, INPUT_A_DOUBLE, INPUT_A_LONG, comboActive || comboSent);
  updateButtonTracker(buttonB, bPressed, INPUT_B_SINGLE, INPUT_B_DOUBLE, INPUT_B_LONG, comboActive || comboSent);

  if (!aPressed && !bPressed) {
    comboSent = false;
    comboActive = false;
    comboStartMs = 0;
  }
}

void updatePet() {
  const uint32_t now = millis();
  if (app.pet.lastTickMs == 0) {
    app.pet.lastTickMs = now;
    return;
  }
  if (now - app.pet.lastTickMs < PET_TICK_MS) {
    return;
  }

  const uint32_t ticks = (now - app.pet.lastTickMs) / PET_TICK_MS;
  app.pet.lastTickMs += ticks * PET_TICK_MS;
  for (uint32_t i = 0; i < ticks; ++i) {
    if (app.pet.sleeping) {
      app.pet.energy = clampStat(app.pet.energy + 2);
      app.pet.mood = clampStat(app.pet.mood + 1);
      if (app.pet.energy > 92) {
        app.pet.sleeping = false;
      }
    } else {
      app.pet.energy = clampStat(app.pet.energy - 1);
      app.pet.hunger = clampStat(app.pet.hunger - 1);
      if (i % 3 == 0) {
        app.pet.cleanliness = clampStat(app.pet.cleanliness - 1);
      }
    }
    if (app.running > 0) {
      app.pet.focus = clampStat(app.pet.focus + 1);
      app.pet.energy = clampStat(app.pet.energy - 1);
    } else if (app.pet.focus > 45) {
      app.pet.focus = clampStat(app.pet.focus - 1);
    }
    if (app.pet.energy < 20 || app.pet.hunger < 20 || app.pet.cleanliness < 20) {
      app.pet.mood = clampStat(app.pet.mood - 1);
    }
  }
  markPetDirty();
  needsRedraw = true;
}

void updateAnimation() {
  if (millis() - app.pet.lastAnimMs < PET_ANIM_MS) {
    return;
  }
  app.pet.lastAnimMs = millis();
  app.pet.animFrame = (app.pet.animFrame + 1) % 8;
  if (page == PAGE_PET || app.interaction.active) {
    needsRedraw = true;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(160);
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setTextSize(1);

  prefs.begin("agentblob", false);
  loadPet();

  app.deviceName = "Codex-S3-" + chipSuffix();
  setupBle();

  app.msg = "Advertise " + app.deviceName;
  redraw();
}

void loop() {
  M5.update();
  handleButtons();
  updatePet();
  updateAnimation();
  savePet();

  if (lastRenderedStatus != statusText()) {
    needsRedraw = true;
  }

  if (needsRedraw) {
    redraw();
  }

  delay(20);
}
