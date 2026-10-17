// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file scale.cpp
 * @brief Anbindung von Bluetooth-Kaffeewaagen über NimBLE.
 */
#include "scale.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <math.h>

#include <atomic>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

enum class Cmd : uint8_t { Tare, TimerStart, TimerStop, TimerReset };

/// Ausgewerteter Inhalt eines Datenpakets.
struct Reading {
  bool hasWeight = false;
  float g = 0.0f;
  int battery = -1;  ///< -1 = Paket enthält keinen Akkuwert
};

/// Sendet Bytes an die verbundene Waage (nur aus dem Waagen-Task).
void send(const Bytes& b);

// =============================================================================
// Protokolle
// =============================================================================

/// Protokoll einer Waagen-Familie: Characteristics, Datenformat, Befehle.
class Protocol {
 public:
  virtual ~Protocol() = default;
  virtual const char* name() const = 0;
  virtual NimBLEUUID notifyUuid() const = 0;
  virtual NimBLEUUID writeUuid() const = 0;
  /// Wertet ein Notification-Paket aus. false = unbekanntes/ungültiges Paket.
  virtual bool parse(const uint8_t* d, size_t len, Reading& r) const = 0;
  virtual Bytes command(Cmd c) const = 0;
  /// Nach dem Abonnieren (z. B. Anmeldung bei Acaia).
  virtual void start() const {}
  /// Intervall für Lebenszeichen an die Waage, 0 = keine nötig.
  virtual uint32_t heartbeatMs() const { return 0; }
  virtual Bytes heartbeat() const { return {}; }
};

/**
 * Felicita Arc / Incline / Parallel: eine Characteristic 0xFFE1 für beides.
 * Paket (18 Byte): [2] '+'/'-', [3..8] sechs ASCII-Ziffern in 0,01 g,
 * [15] Akku-Rohwert 129..158. Befehle: ein ASCII-Zeichen.
 */
class Felicita : public Protocol {
 public:
  const char* name() const override { return "Felicita"; }
  NimBLEUUID notifyUuid() const override { return NimBLEUUID((uint16_t)0xFFE1); }
  NimBLEUUID writeUuid() const override { return NimBLEUUID((uint16_t)0xFFE1); }

  bool parse(const uint8_t* d, size_t len, Reading& r) const override {
    if (len < 9 || (d[2] != '+' && d[2] != '-')) return false;
    int32_t centi = 0;
    for (size_t i = 3; i < 9; ++i) {
      if (d[i] < '0' || d[i] > '9') return false;
      centi = centi * 10 + (d[i] - '0');
    }
    r.hasWeight = true;
    r.g = (d[2] == '-' ? -centi : centi) / 100.0f;
    if (len >= 16) r.battery = constrain((d[15] - kBatteryRawMin) * 100 / (kBatteryRawMax - kBatteryRawMin), 0, 100);
    return true;
  }

  Bytes command(Cmd c) const override {
    switch (c) {
      case Cmd::Tare:       return {'T'};
      case Cmd::TimerStart: return {'R'};
      case Cmd::TimerStop:  return {'S'};
      case Cmd::TimerReset: return {'C'};
    }
    return {};
  }

 private:
  static constexpr int kBatteryRawMin = 129;
  static constexpr int kBatteryRawMax = 158;
};

/**
 * Acaia (Lunar, Pearl, Pyxis, Cinco …). Nach dem Verbinden muss sich der
 * Client anmelden (IDENTIFY) und Gewichtsdaten anfordern; danach alle ~3 s
 * ein Lebenszeichen, sonst trennt die Waage.
 *  - alte Firmware (vor 2021): Characteristic 0x2A80, Gewicht ab Byte 2
 *  - neue Firmware: ISSC-UUIDs, Paket "EF DD 0C .. 05", Gewicht ab Byte 5
 * Gewicht: 16 Bit little endian, danach (+4) Nachkommastellen, (+5) Bit 1 = negativ.
 */
class Acaia : public Protocol {
 public:
  explicit Acaia(bool oldFirmware) : old_(oldFirmware) {}

  const char* name() const override { return old_ ? "Acaia (alt)" : "Acaia"; }
  NimBLEUUID notifyUuid() const override {
    return old_ ? NimBLEUUID((uint16_t)0x2A80) : NimBLEUUID("49535343-1e4d-4bd9-ba61-23c647249616");
  }
  NimBLEUUID writeUuid() const override {
    return old_ ? NimBLEUUID((uint16_t)0x2A80) : NimBLEUUID("49535343-8841-43f4-a8d4-ecbe34729bb3");
  }

  bool parse(const uint8_t* d, size_t len, Reading& r) const override {
    size_t at;
    if (old_) {
      if (len != 10 && len != 14) return false;
      at = 2;
    } else {
      if ((len != 13 && len != 17) || d[4] != 0x05) return false;
      at = 5;
    }
    const int raw = d[at] | (d[at + 1] << 8);
    const uint8_t decimals = d[at + 4];
    if (decimals > 4) return false;
    r.hasWeight = true;
    r.g = raw / powf(10.0f, decimals) * ((d[at + 5] & 0x02) ? -1.0f : 1.0f);
    return true;
  }

  Bytes command(Cmd c) const override {
    switch (c) {
      case Cmd::Tare:       return {0xEF, 0xDD, 0x04, 0x00, 0x00, 0x00};
      case Cmd::TimerStart: return {0xEF, 0xDD, 0x0D, 0x00, 0x00, 0x00, 0x00};
      case Cmd::TimerStop:  return {0xEF, 0xDD, 0x0D, 0x00, 0x02, 0x00, 0x02};
      case Cmd::TimerReset: return {0xEF, 0xDD, 0x0D, 0x00, 0x01, 0x00, 0x01};
    }
    return {};
  }

  void start() const override {
    send({0xEF, 0xDD, 0x0B, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,  // IDENTIFY
          0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x9A, 0x6D});
    vTaskDelay(pdMS_TO_TICKS(100));
    send({0xEF, 0xDD, 0x0C, 0x09, 0x00, 0x01, 0x01, 0x02, 0x02, 0x05, 0x03, 0x04,  // Daten anfordern
          0x15, 0x06});
  }
  uint32_t heartbeatMs() const override { return 2750; }
  Bytes heartbeat() const override { return {0xEF, 0xDD, 0x00, 0x02, 0x00, 0x02, 0x00}; }

 private:
  bool old_;
};

/**
 * Bookoo Themis Mini / Ultra: Gewicht auf 0xFF11, Befehle auf 0xFF12.
 * Paket (20 Byte): [0]=0x03 [1]=0x0B, [6] '-' = negativ, [7..9] Gewicht in
 * 0,01 g (big endian), [13] Akku in %.
 */
class Bookoo : public Protocol {
 public:
  const char* name() const override { return "Bookoo"; }
  NimBLEUUID notifyUuid() const override { return NimBLEUUID((uint16_t)0xFF11); }
  NimBLEUUID writeUuid() const override { return NimBLEUUID((uint16_t)0xFF12); }

  bool parse(const uint8_t* d, size_t len, Reading& r) const override {
    if (len < 14 || d[0] != 0x03 || d[1] != 0x0B) return false;
    const int32_t centi = (d[7] << 16) | (d[8] << 8) | d[9];
    r.hasWeight = true;
    r.g = (d[6] == '-' ? -centi : centi) / 100.0f;
    if (d[13] <= 100) r.battery = d[13];
    return true;
  }

  Bytes command(Cmd c) const override {
    switch (c) {
      case Cmd::Tare:       return {0x03, 0x0A, 0x01, 0x00, 0x00, 0x08};
      case Cmd::TimerStart: return {0x03, 0x0A, 0x04, 0x00, 0x00, 0x0A};
      case Cmd::TimerStop:  return {0x03, 0x0A, 0x05, 0x00, 0x00, 0x0D};
      case Cmd::TimerReset: return {0x03, 0x0A, 0x06, 0x00, 0x00, 0x0C};
    }
    return {};
  }
};

/**
 * Decent Scale / Half Decent Scale: Daten auf 0xFFF4, Befehle auf 0x36F5.
 * Pakete: 03 <Typ> d1..d4 XOR (7 Byte, ab Firmware 1.2 Gewicht mit 10 Byte).
 * Gewicht: Typ 0xCE (ruhig) / 0xCA (ändert sich), int16 big endian in 0,1 g.
 * Antwort auf "LED an": 03 0A 00 <Einheit> <Akku> <Firmware> XOR.
 */
class Decent : public Protocol {
 public:
  const char* name() const override { return "Decent"; }
  NimBLEUUID notifyUuid() const override { return NimBLEUUID((uint16_t)0xFFF4); }
  NimBLEUUID writeUuid() const override { return NimBLEUUID((uint16_t)0x36F5); }

  bool parse(const uint8_t* d, size_t len, Reading& r) const override {
    if (len < 7 || d[0] != 0x03) return false;
    if (d[1] == 0xCE || d[1] == 0xCA) {
      r.hasWeight = true;
      r.g = static_cast<int16_t>((d[2] << 8) | d[3]) / 10.0f;
      return true;
    }
    if (d[1] == 0x0A) {
      if (d[4] <= 100) r.battery = d[4];  // 0xFF = USB-Betrieb
      return true;
    }
    return false;
  }

  Bytes command(Cmd c) const override {
    switch (c) {
      case Cmd::Tare:       return packet(0x0F, 0x00, 0x00, 0x00, 0x00);
      case Cmd::TimerStart: return packet(0x0B, 0x03, 0x00, 0x00, 0x00);
      case Cmd::TimerStop:  return packet(0x0B, 0x00, 0x00, 0x00, 0x00);
      case Cmd::TimerReset: return packet(0x0B, 0x02, 0x00, 0x00, 0x00);
    }
    return {};
  }

  /// Anzeige einschalten (Gramm); die Antwort enthält den Akkustand.
  void start() const override { send(packet(0x0A, 0x01, 0x01, 0x00, 0x00)); }
  /// Half Decent Scale trennt ohne Lebenszeichen nach 5 s (ältere ignorieren es).
  uint32_t heartbeatMs() const override { return 4000; }
  Bytes heartbeat() const override { return packet(0x0A, 0x03, 0xFF, 0xFF, 0x00); }

 private:
  static Bytes packet(uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    Bytes p = {0x03, type, a, b, c, d, 0};
    for (size_t i = 0; i < 6; ++i) p[6] ^= p[i];
    return p;
  }
};

const Felicita kFelicita;
const Acaia kAcaiaOld(true);
const Acaia kAcaiaNew(false);
const Bookoo kBookoo;
const Decent kDecent;

/// Reihenfolge der Erkennung nach dem Verbinden (erste passende Characteristic gewinnt).
const Protocol* const kProtocols[] = {&kAcaiaOld, &kAcaiaNew, &kBookoo, &kDecent, &kFelicita};

/// Namensanfänge (Großbuchstaben) der unterstützten Waagen.
const char* const kNamePrefixes[] = {"FELICITA", "ACAIA", "LUNAR", "PEARL", "PYXIS", "CINCO",
                                     "PROCH",    "ORION", "BOOKOO", "DECENT"};

// =============================================================================
// Verbindung und Zustand
// =============================================================================

// --- Zeitverhalten ------------------------------------------------------------
constexpr uint32_t kScanSeconds = 4;      ///< Dauer eines Suchlaufs
constexpr uint32_t kRetryMs = 2000;       ///< Pause zwischen Versuchen
constexpr uint32_t kStaleMs = 3000;       ///< ohne Daten: Gewicht gilt als unbekannt
constexpr uint32_t kSilentMs = 8000;      ///< so lange ohne Daten -> neu verbinden
constexpr uint32_t kFlowWindowMs = 1000;  ///< Zeitfenster für den Durchfluss
constexpr uint32_t kStableWindowMs = 1000;  ///< Zeitfenster für "Gewicht ruhig"
constexpr float kStableRangeG = 0.3f;       ///< max. Schwankung für "ruhig"
constexpr float kMinPortafilterG = 50.0f;   ///< leichter = kein Siebträger
/// Tara prüfen: Manche Waagen (z. B. Felicita) ignorieren Tara, solange sich das
/// Gewicht bewegt. Nach dieser Zeit wird geprüft und ggf. erneut tariert.
constexpr uint32_t kTareCheckMs = 800;
constexpr uint8_t kTareRetries = 5;
/// Ändert sich das Gewicht seit der Tara um mehr als das, wurde etwas
/// abgestellt/abgenommen (Tara angekommen). Ein laufender Bezug legt in
/// kTareCheckMs nur wenige Gramm zu.
constexpr float kTareChangedG = 5.0f;

// --- Zustand (von mehreren Tasks genutzt) -------------------------------------
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
scale::Config cfg;
std::atomic<scale::State> state{scale::State::Off};
std::atomic<uint8_t> pendingCmds{0};  ///< Bitmaske vorgemerkter Befehle
std::atomic<const Protocol*> proto{nullptr};  ///< Protokoll der verbundenen Waage
std::atomic<uint32_t> tares{0};               ///< Anzahl angeforderter Tara-Befehle

float lastWeight = NAN;   ///< Rohgewicht der Waage
float doseOffset = 0.0f;  ///< Dosier-Tara: wird zum Rohgewicht addiert
bool doseActive = false;
uint32_t lastPacketMs = 0;
int batteryPct = -1;
uint32_t packetCount = 0;
uint32_t invalidCount = 0;
uint32_t sentCount = 0;    ///< Diagnose: gesendete Befehle
uint32_t failedCount = 0;  ///< Diagnose: fehlgeschlagene Schreibvorgänge
char lastCmd[24] = "";     ///< Diagnose: zuletzt gesendete Befehlsfolge
uint8_t lastRaw[20];
size_t lastRawLen = 0;
String connectedName;

/// Ringpuffer (Zeit, Rohgewicht) für Durchfluss und Ruhe-Erkennung.
struct Sample { uint32_t ms; float g; };
constexpr size_t kSamples = 24;
Sample samples[kSamples];
size_t sampleHead = 0, sampleCount = 0;

// Befehls-Bits für pendingCmds
enum : uint8_t { kBitTare = 1, kBitStart = 2, kBitStop = 4, kBitReset = 8 };

Preferences prefs;
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* notifyChr = nullptr;
NimBLERemoteCharacteristic* writeChr = nullptr;
uint32_t connectedAtMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t tareCheckAtMs = 0;  ///< 0 = keine Tara-Prüfung offen
float weightAtTare = 0.0f;   ///< Rohgewicht beim Senden der Tara
uint8_t tareTries = 0;

/// Liegen aktuelle Gewichtsdaten vor? (Aufruf nur unter mux)
bool freshLocked() { return lastPacketMs && millis() - lastPacketMs < kStaleMs; }

void send(const Bytes& b) {
  if (!writeChr || b.empty()) return;
  // Mit Bestätigung, wenn die Waage das kann: ohne gehen schnell aufeinander
  // folgende Befehle (z. B. Timer stoppen + auf 0) bei der Felicita verloren.
  const bool ok = writeChr->writeValue(b.data(), b.size(), writeChr->canWrite());
  portENTER_CRITICAL(&mux);
  ++(ok ? sentCount : failedCount);
  portEXIT_CRITICAL(&mux);
}

/// Wertet ein Datenpaket mit dem Protokoll der verbundenen Waage aus.
void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  portENTER_CRITICAL(&mux);
  lastRawLen = len < sizeof(lastRaw) ? len : sizeof(lastRaw);
  memcpy(lastRaw, data, lastRawLen);
  portEXIT_CRITICAL(&mux);

  const Protocol* p = proto.load();
  Reading r;
  if (!p || !p->parse(data, len, r)) {
    portENTER_CRITICAL(&mux);
    ++invalidCount;
    portEXIT_CRITICAL(&mux);
    return;
  }
  const uint32_t now = millis();

  portENTER_CRITICAL(&mux);
  ++packetCount;
  if (r.battery >= 0) batteryPct = r.battery;
  if (r.hasWeight) {
    lastWeight = r.g;
    lastPacketMs = now;
    samples[sampleHead] = {now, r.g};
    sampleHead = (sampleHead + 1) % kSamples;
    if (sampleCount < kSamples) ++sampleCount;
  }
  portEXIT_CRITICAL(&mux);
}

/// Merkt das Rohgewicht beim Tarieren, um den Erfolg später zu prüfen.
void armTareCheck(uint32_t now) {
  portENTER_CRITICAL(&mux);
  weightAtTare = isnan(lastWeight) ? 0.0f : lastWeight;
  portEXIT_CRITICAL(&mux);
  tareCheckAtMs = now + kTareCheckMs;
}

/**
 * Hat die Waage die Tara übernommen? Erneut gesendet wird nur, wenn sie den
 * Befehl offensichtlich ignoriert hat: Gewicht weder nahe 0 noch deutlich
 * verändert. Wurde inzwischen z. B. eine Tasse abgestellt, bleibt sie stehen.
 */
void checkTare(const Protocol* p, uint32_t now) {
  if (!tareCheckAtMs || static_cast<int32_t>(now - tareCheckAtMs) < 0) return;
  portENTER_CRITICAL(&mux);
  const float w = freshLocked() ? lastWeight : NAN;
  portEXIT_CRITICAL(&mux);
  const bool zeroed = isnan(w) || fabsf(w) < fmaxf(3.0f, fabsf(weightAtTare) * 0.5f);
  const bool changed = !isnan(w) && fabsf(w - weightAtTare) > kTareChangedG;
  const bool done = zeroed || changed;
  if (done || tareTries >= kTareRetries) {
    if (!done) Serial.println("[scale] Tara nicht übernommen – aufgegeben");
    tareCheckAtMs = 0;
    return;
  }
  ++tareTries;
  Serial.printf("[scale] Tara nicht übernommen (%.1f g) – Versuch %u\n", w, tareTries + 1);
  send(p->command(Cmd::Tare));
  tareCheckAtMs = now + kTareCheckMs;
}

/// Sendet vorgemerkte Befehle und ggf. das Lebenszeichen an die Waage.
void sendPending() {
  const Protocol* p = proto.load();
  if (!p || !writeChr) return;
  const uint32_t now = millis();
  if (p->heartbeatMs() && now - lastHeartbeatMs >= p->heartbeatMs()) {
    send(p->heartbeat());
    lastHeartbeatMs = now;
  }
  checkTare(p, now);

  const uint8_t cmds = pendingCmds.exchange(0);
  if (!cmds) return;
  // Reihenfolge: tarieren, Timer stoppen, auf 0 setzen, starten
  const struct { uint8_t bit; Cmd cmd; } order[] = {
      {kBitTare, Cmd::Tare}, {kBitStop, Cmd::TimerStop},
      {kBitReset, Cmd::TimerReset}, {kBitStart, Cmd::TimerStart}};
  static const char* const kCmdNames[] = {"Tara", "Start", "Stopp", "Null"};
  String names;
  for (const auto& o : order) {
    if (cmds & o.bit) {
      names += names.isEmpty() ? "" : "+";
      names += kCmdNames[static_cast<int>(o.cmd)];
      if (o.cmd == Cmd::Tare) {
        tareTries = 0;
        armTareCheck(now);
      }
      send(p->command(o.cmd));
      vTaskDelay(pdMS_TO_TICKS(200));  // Waage braucht Zeit zwischen Befehlen
    }
  }
  Serial.printf("[scale] gesendet: %s\n", names.c_str());
  portENTER_CRITICAL(&mux);
  strlcpy(lastCmd, names.c_str(), sizeof(lastCmd));
  portEXIT_CRITICAL(&mux);
}

bool isScaleName(String name) {
  name.toUpperCase();
  for (const char* prefix : kNamePrefixes) {
    if (name.startsWith(prefix)) return true;
  }
  return false;
}

/// Sucht eine Characteristic in allen Services der verbundenen Waage.
NimBLERemoteCharacteristic* findChar(const NimBLEUUID& uuid) {
  auto* services = client->getServices(false);
  if (!services) return nullptr;
  for (NimBLERemoteService* svc : *services) {
    if (NimBLERemoteCharacteristic* c = svc->getCharacteristic(uuid)) return c;
  }
  return nullptr;
}

void disconnect() {
  proto = nullptr;
  notifyChr = writeChr = nullptr;
  if (client && client->isConnected()) client->disconnect();
}

/// Sucht eine unterstützte Waage und verbindet sich. true = verbunden.
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
    const String n = dev.getName().c_str();
    if (isScaleName(n)) {
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

  // Protokoll an den vorhandenen Characteristics erkennen.
  client->getServices(true);
  const Protocol* p = nullptr;
  for (const Protocol* cand : kProtocols) {
    NimBLERemoteCharacteristic* n = findChar(cand->notifyUuid());
    NimBLERemoteCharacteristic* w = findChar(cand->writeUuid());
    if (n && w && (n->canNotify() || n->canIndicate())) {
      p = cand;
      notifyChr = n;
      writeChr = w;
      break;
    }
  }
  if (!p || !notifyChr->subscribe(notifyChr->canNotify(), onNotify)) {
    Serial.printf("[scale] %s: unbekanntes Protokoll / kein Datenkanal\n", name.c_str());
    disconnect();
    return false;
  }
  portENTER_CRITICAL(&mux);
  connectedName = name;
  sampleCount = 0;
  batteryPct = -1;
  lastPacketMs = 0;
  portEXIT_CRITICAL(&mux);
  proto = p;
  p->start();
  connectedAtMs = lastHeartbeatMs = millis();
  Serial.printf("[scale] verbunden mit %s (%s, Protokoll %s)\n", name.c_str(),
                address.toString().c_str(), p->name());
  return true;
}

/// Waagen-Task: Verbindung halten, Befehle senden.
void scaleTask(void*) {
  bool bleStarted = false;
  for (;;) {
    if (!cfg.enabled) {
      disconnect();
      state = scale::State::Off;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!bleStarted) {
      NimBLEDevice::init("CoffeeTimer");
      bleStarted = true;
    }
    if (client && client->isConnected() && proto.load()) {
      // Verbunden, aber keine Daten mehr (z. B. Anmeldung verloren) -> neu verbinden
      portENTER_CRITICAL(&mux);
      const uint32_t since = lastPacketMs ? lastPacketMs : connectedAtMs;
      portEXIT_CRITICAL(&mux);
      if (millis() - since > kSilentMs) {
        Serial.println("[scale] keine Daten – verbinde neu");
        disconnect();
        continue;
      }
      state = scale::State::Connected;
      sendPending();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    disconnect();
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
  cfg.grindTare = prefs.getBool("grindTare", false);
  cfg.portafilterG = prefs.getFloat("pfG", 0.0f);
  // Eigener Task auf Core 0 (niedrige Priorität): Suche/Verbindungsaufbau
  // blockieren einige Sekunden und dürfen Display und Erkennung nicht stören.
  xTaskCreatePinnedToCore(scaleTask, "scale", 6144, nullptr, 1, nullptr, 0);
}

Config config() { return cfg; }

void setConfig(const Config& c) {
  cfg = c;
  cfg.portafilterG = constrain(c.portafilterG, 0.0f, 2000.0f);
  prefs.putBool("enabled", cfg.enabled);
  prefs.putBool("autoTare", cfg.autoTare);
  prefs.putBool("syncTimer", cfg.syncTimer);
  prefs.putBool("grindTare", cfg.grindTare);
  prefs.putFloat("pfG", cfg.portafilterG);
}

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
  const float w = freshLocked() ? lastWeight + doseOffset : NAN;
  portEXIT_CRITICAL(&mux);
  return connected() ? w : NAN;
}

bool stable() {
  portENTER_CRITICAL(&mux);
  bool ok = freshLocked() && sampleCount >= 4;
  if (ok) {
    const Sample& last = samples[(sampleHead + kSamples - 1) % kSamples];
    float lo = last.g, hi = last.g;
    uint32_t span = 0;
    for (size_t i = 2; i <= sampleCount; ++i) {
      const Sample& s = samples[(sampleHead + kSamples - i) % kSamples];
      if (last.ms - s.ms > kStableWindowMs) break;
      lo = fminf(lo, s.g);
      hi = fmaxf(hi, s.g);
      span = last.ms - s.ms;
    }
    ok = span >= kStableWindowMs * 3 / 4 && hi - lo <= kStableRangeG;
  }
  portEXIT_CRITICAL(&mux);
  return ok && connected();
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
  const bool fresh = freshLocked();
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

const char* model() {
  const Protocol* p = proto.load();
  return p && connected() ? p->name() : "";
}

void tare() {
  clearDoseOffset();
  ++tares;
  pendingCmds |= kBitTare;
}

void tareForDose() {
  portENTER_CRITICAL(&mux);
  doseOffset = -cfg.portafilterG;
  doseActive = true;
  portEXIT_CRITICAL(&mux);
  ++tares;
  pendingCmds |= kBitTare;
}

void clearDoseOffset() {
  portENTER_CRITICAL(&mux);
  doseOffset = 0.0f;
  doseActive = false;
  portEXIT_CRITICAL(&mux);
}

bool doseMode() {
  portENTER_CRITICAL(&mux);
  const bool d = doseActive;
  portEXIT_CRITICAL(&mux);
  return d;
}

uint32_t tareCount() { return tares.load(); }

bool learnPortafilter() {
  if (!stable()) return false;
  portENTER_CRITICAL(&mux);
  const float raw = lastWeight;
  portEXIT_CRITICAL(&mux);
  if (isnan(raw) || raw < kMinPortafilterG) return false;
  Config c = cfg;
  c.portafilterG = roundf(raw * 10.0f) / 10.0f;
  setConfig(c);
  Serial.printf("[scale] Siebträger: %.1f g\n", c.portafilterG);
  return true;
}

void timerStart() { pendingCmds |= kBitStart; }
void timerStop() { pendingCmds |= kBitStop; }
void timerReset() { pendingCmds |= kBitReset; }

void diagnostics(String& out) {
  char buf[160];
  uint8_t raw[20];
  size_t rawLen;
  uint32_t packets, invalid;
  portENTER_CRITICAL(&mux);
  rawLen = lastRawLen;
  memcpy(raw, lastRaw, rawLen);
  packets = packetCount;
  invalid = invalidCount;
  portEXIT_CRITICAL(&mux);

  snprintf(buf, sizeof(buf),
           "Waage: %s%s%s%s%s, Pakete %lu (ungueltig %lu), Gewicht %.2f g, Akku %d%%, "
           "Siebtraeger %.1f g%s\n",
           stateText(), connected() ? " mit " : "", connected() ? deviceName().c_str() : "",
           connected() ? " / " : "", model(), (unsigned long)packets, (unsigned long)invalid,
           weight(), battery(), cfg.portafilterG, doseMode() ? " (Dosier-Tara aktiv)" : "");
  out += buf;
  portENTER_CRITICAL(&mux);
  snprintf(buf, sizeof(buf), "  Befehle: gesendet %lu, fehlgeschlagen %lu, zuletzt %s\n",
           (unsigned long)sentCount, (unsigned long)failedCount, lastCmd[0] ? lastCmd : "-");
  portEXIT_CRITICAL(&mux);
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
