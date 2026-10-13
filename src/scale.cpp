/**
 * @file scale.cpp
 * @brief Felicita-Arc-Anbindung über NimBLE.
 */
#include "scale.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <math.h>

#include <atomic>

namespace {

// --- Protokoll ----------------------------------------------------------------
const NimBLEUUID kServiceUuid((uint16_t)0xFFE0);
const NimBLEUUID kCharUuid((uint16_t)0xFFE1);
constexpr const char* kNamePrefix = "FELICITA";
constexpr uint8_t kCmdTare = 0x54;        // 'T'
constexpr uint8_t kCmdTimerStart = 0x52;  // 'R'
constexpr uint8_t kCmdTimerStop = 0x53;   // 'S'
constexpr uint8_t kCmdTimerReset = 0x43;  // 'C'
constexpr int kBatteryRawMin = 129;
constexpr int kBatteryRawMax = 158;

// --- Zeitverhalten ------------------------------------------------------------
constexpr uint32_t kScanSeconds = 4;      ///< Dauer eines Suchlaufs
constexpr uint32_t kRetryMs = 2000;       ///< Pause zwischen Versuchen
constexpr uint32_t kStaleMs = 3000;       ///< ohne Daten: Gewicht gilt als unbekannt
constexpr uint32_t kFlowWindowMs = 1000;  ///< Zeitfenster für den Durchfluss

// --- Zustand (von mehreren Tasks genutzt) -------------------------------------
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
scale::Config cfg;
std::atomic<scale::State> state{scale::State::Off};
std::atomic<uint8_t> pendingCmds{0};  ///< Bitmaske vorgemerkter Befehle

float lastWeight = NAN;
uint32_t lastPacketMs = 0;
int batteryPct = -1;
uint32_t packetCount = 0;
uint32_t invalidCount = 0;
uint8_t lastRaw[20];
size_t lastRawLen = 0;
String connectedName;

/// Ringpuffer (Zeit, Gewicht) für den Durchfluss.
struct Sample { uint32_t ms; float g; };
constexpr size_t kSamples = 24;
Sample samples[kSamples];
size_t sampleHead = 0, sampleCount = 0;

// Befehls-Bits für pendingCmds
enum : uint8_t { kBitTare = 1, kBitStart = 2, kBitStop = 4, kBitReset = 8 };

Preferences prefs;
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* chr = nullptr;

/**
 * Wertet ein Gewichtspaket aus. Tolerant: Nur Pakete mit gültigem Vorzeichen
 * und sechs Ziffern werden übernommen, alles andere wird gezählt und verworfen.
 */
void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  portENTER_CRITICAL(&mux);
  lastRawLen = len < sizeof(lastRaw) ? len : sizeof(lastRaw);
  memcpy(lastRaw, data, lastRawLen);
  portEXIT_CRITICAL(&mux);

  bool ok = len >= 9 && (data[2] == '+' || data[2] == '-');
  int32_t centi = 0;
  for (size_t i = 3; ok && i < 9; ++i) {
    if (data[i] < '0' || data[i] > '9') ok = false;
    centi = centi * 10 + (data[i] - '0');
  }
  if (!ok) {
    portENTER_CRITICAL(&mux);
    ++invalidCount;
    portEXIT_CRITICAL(&mux);
    return;
  }
  const float g = (data[2] == '-' ? -centi : centi) / 100.0f;
  const uint32_t now = millis();

  portENTER_CRITICAL(&mux);
  lastWeight = g;
  lastPacketMs = now;
  ++packetCount;
  if (len >= 16) {
    const int raw = data[15];
    batteryPct = constrain((raw - kBatteryRawMin) * 100 / (kBatteryRawMax - kBatteryRawMin), 0, 100);
  }
  samples[sampleHead] = {now, g};
  sampleHead = (sampleHead + 1) % kSamples;
  if (sampleCount < kSamples) ++sampleCount;
  portEXIT_CRITICAL(&mux);
}

/// Sendet vorgemerkte Befehle an die Waage.
void sendPending() {
  const uint8_t cmds = pendingCmds.exchange(0);
  if (!cmds || !chr) return;
  // Reihenfolge: erst tarieren / Timer zurücksetzen, dann starten bzw. stoppen
  const uint8_t order[][2] = {{kBitTare, kCmdTare}, {kBitReset, kCmdTimerReset},
                              {kBitStart, kCmdTimerStart}, {kBitStop, kCmdTimerStop}};
  for (const auto& o : order) {
    if (cmds & o[0]) {
      uint8_t b = o[1];
      chr->writeValue(&b, 1, false);
      vTaskDelay(pdMS_TO_TICKS(60));  // Waage braucht kurz Zeit zwischen Befehlen
    }
  }
}

/// Sucht eine Felicita-Waage und verbindet sich. true = verbunden.
bool findAndConnect() {
  state = scale::State::Scanning;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);  // Name steht oft erst in der Scan-Antwort
  scan->setInterval(100);
  scan->setWindow(99);
  NimBLEScanResults results = scan->start(kScanSeconds, false);

  NimBLEAddress address;
  String name;
  bool found = false;
  for (int i = 0; i < results.getCount() && !found; ++i) {
    NimBLEAdvertisedDevice dev = results.getDevice(i);
    String n = dev.getName().c_str();
    String upper = n;
    upper.toUpperCase();
    if (upper.startsWith(kNamePrefix)) {
      address = dev.getAddress();
      name = n;
      found = true;
    }
  }
  scan->clearResults();
  if (!found || !cfg.enabled) return false;

  state = scale::State::Connecting;
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setConnectTimeout(5);
  }
  if (!client->connect(address)) return false;

  NimBLERemoteService* svc = client->getService(kServiceUuid);
  chr = svc ? svc->getCharacteristic(kCharUuid) : nullptr;
  if (!chr || !chr->canNotify() || !chr->subscribe(true, onNotify)) {
    Serial.println("[scale] Waage gefunden, aber Datenkanal fehlt – andere Firmware/Modell?");
    chr = nullptr;
    client->disconnect();
    return false;
  }
  portENTER_CRITICAL(&mux);
  connectedName = name;
  sampleCount = 0;
  portEXIT_CRITICAL(&mux);
  Serial.printf("[scale] verbunden mit %s (%s)\n", name.c_str(), address.toString().c_str());
  return true;
}

/// Waagen-Task: Verbindung halten, Befehle senden.
void scaleTask(void*) {
  bool bleStarted = false;
  for (;;) {
    if (!cfg.enabled) {
      if (client && client->isConnected()) client->disconnect();
      chr = nullptr;
      state = scale::State::Off;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!bleStarted) {
      NimBLEDevice::init("CoffeeTimer");
      bleStarted = true;
    }
    if (client && client->isConnected() && chr) {
      state = scale::State::Connected;
      sendPending();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    chr = nullptr;
    if (!findAndConnect()) vTaskDelay(pdMS_TO_TICKS(kRetryMs));
  }
}

}  // namespace

namespace scale {

void begin() {
  prefs.begin("scale", false);
  cfg.enabled = prefs.getBool("enabled", false);
  cfg.autoTare = prefs.getBool("autoTare", true);
  cfg.syncTimer = prefs.getBool("syncTimer", false);
  // Eigener Task auf Core 0 (niedrige Priorität): Suche/Verbindungsaufbau
  // blockieren einige Sekunden und dürfen Display und Erkennung nicht stören.
  xTaskCreatePinnedToCore(scaleTask, "scale", 6144, nullptr, 1, nullptr, 0);
}

Config config() { return cfg; }

void setConfig(const Config& c) {
  cfg = c;
  prefs.putBool("enabled", c.enabled);
  prefs.putBool("autoTare", c.autoTare);
  prefs.putBool("syncTimer", c.syncTimer);
}

State state() { return ::state.load(); }

const char* stateText() {
  switch (::state.load()) {
    case State::Off:        return "aus";
    case State::Scanning:   return "sucht …";
    case State::Connecting: return "verbindet …";
    case State::Connected:  return "verbunden";
  }
  return "?";
}

bool connected() { return ::state.load() == State::Connected; }

float weight() {
  portENTER_CRITICAL(&mux);
  const bool fresh = lastPacketMs && millis() - lastPacketMs < kStaleMs;
  const float w = fresh ? lastWeight : NAN;
  portEXIT_CRITICAL(&mux);
  return connected() ? w : NAN;
}

float flowRate() {
  // Steigung zwischen dem neuesten Wert und dem Wert vor ~1 s
  portENTER_CRITICAL(&mux);
  float rate = 0.0f;
  if (sampleCount >= 2) {
    const Sample& last = samples[(sampleHead + kSamples - 1) % kSamples];
    for (size_t i = 2; i <= sampleCount; ++i) {
      const Sample& s = samples[(sampleHead + kSamples - i) % kSamples];
      if (last.ms - s.ms >= kFlowWindowMs || i == sampleCount) {
        const uint32_t dt = last.ms - s.ms;
        if (dt > 200) rate = (last.g - s.g) * 1000.0f / dt;
        break;
      }
    }
  }
  const bool fresh = lastPacketMs && millis() - lastPacketMs < kStaleMs;
  portEXIT_CRITICAL(&mux);
  return fresh && connected() ? rate : 0.0f;
}

int battery() {
  portENTER_CRITICAL(&mux);
  const int b = batteryPct;
  portEXIT_CRITICAL(&mux);
  return connected() ? b : -1;
}

String deviceName() {
  portENTER_CRITICAL(&mux);
  String n = connectedName;
  portEXIT_CRITICAL(&mux);
  return n;
}

void tare() { pendingCmds |= kBitTare; }
void timerStart() { pendingCmds |= kBitStart; }
void timerStop() { pendingCmds |= kBitStop; }
void timerReset() { pendingCmds |= kBitReset; }

void diagnostics(String& out) {
  char buf[120];
  uint8_t raw[20];
  size_t rawLen;
  uint32_t packets, invalid;
  portENTER_CRITICAL(&mux);
  rawLen = lastRawLen;
  memcpy(raw, lastRaw, rawLen);
  packets = packetCount;
  invalid = invalidCount;
  portEXIT_CRITICAL(&mux);

  snprintf(buf, sizeof(buf), "Waage: %s%s%s, Pakete %lu (ungueltig %lu), Gewicht %.2f g, Akku %d%%\n",
           stateText(), connected() ? " mit " : "", connected() ? deviceName().c_str() : "",
           (unsigned long)packets, (unsigned long)invalid, weight(), battery());
  out += buf;
  if (rawLen) {
    out += "  Letztes Paket:";
    for (size_t i = 0; i < rawLen; ++i) {
      snprintf(buf, sizeof(buf), " %02X", raw[i]);
      out += buf;
    }
    out += '\n';
  }
}

}  // namespace scale
