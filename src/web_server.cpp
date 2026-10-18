// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file web_server.cpp
 * @brief Implementierung von Webseite und JSON-API.
 */
#include "web_server.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>

#include "app.h"
#include "battery.h"
#include "net_manager.h"
#include "scale.h"
#include "vibration.h"
#include "ui.h"
#include "web_page.h"

namespace {

WebServer server(80);

/// Neustart nach dem Speichern der WLAN-Daten (verzögert, damit die Antwort
/// noch beim Browser ankommt).
uint32_t restartAtMs = 0;

const char* qualityName(TeachQuality q) {
  switch (q) {
    case TeachQuality::Ok:      return "ok";
    case TeachQuality::Warning: return "warning";
    case TeachQuality::Error:   return "error";
  }
  return "?";
}

/// Sendet ein JsonDocument als Antwort.
void sendJson(const JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

/// Einfache Antwort {"ok": true/false, "message": "..."}.
void sendOk(bool ok = true, const char* message = nullptr) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (message) doc["message"] = message;
  sendJson(doc, ok ? 200 : 400);
}

void fillSettings(JsonObject o, const DetectorSettings& s) {
  o["similarityThreshold"] = s.similarityThreshold;
  o["periodicityThreshold"] = s.periodicityThreshold;
  o["levelThreshold"] = s.levelThreshold;
  o["onDelayMs"] = s.onDelayMs;
  o["offDelayMs"] = s.offDelayMs;
  o["minShotSeconds"] = s.minShotSeconds;
}

// --- Routen-Handler --------------------------------------------------------

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", kWebPage);
}

void handleStatus() {
  // Snapshot ist groß (Teach-Ergebnis, Historie) -> statisch statt auf dem Stack.
  static StatusSnapshot s;
  app.snapshot(s);

  JsonDocument doc;
  doc["running"] = s.timerRunning;
  doc["timerMs"] = s.timerMs;
  doc["pumpOn"] = s.detector.pumpOn;
  doc["uptimeMs"] = millis();
  doc["audioFrames"] = s.audioFrames;

  JsonObject live = doc["live"].to<JsonObject>();
  live["rmsDb"] = s.detector.rmsDb;
  live["grinderOn"] = s.grinderOn;
  if (s.grinderOn && s.grinderActive >= 0 && s.grinderActive < s.grinderCount) {
    live["grinderName"] = s.grinderNames[s.grinderActive];
  }
  live["grinderSim"] = s.detector.scores.grinderSimilarity;
  live["noiseSim"] = s.detector.scores.noiseSimilarity;
  live["similarity"] = s.detector.scores.similarity;
  live["periodicity"] = s.detector.scores.periodicity;
  live["rhythm"] = s.detector.scores.rhythm;
  live["periodic"] = s.detector.periodic;
  live["levelRatio"] = s.detector.scores.levelRatio;
  live["rawActive"] = s.detector.rawActive;
  live["rejected"] = s.detector.rejected;
  live["steadiness"] = fminf(s.detector.scores.steadiness, 20.0f);

  JsonObject prof = doc["profile"].to<JsonObject>();
  prof["slot"] = s.activeSlot;
  prof["name"] = s.profileName;
  prof["isDefault"] = s.profileDefault;
  prof["saveOk"] = s.lastSaveOk;
  prof["valid"] = s.profileValid;
  prof["bgDb"] = s.profileBgDb;
  prof["pumpDb"] = s.profilePumpDb;
  prof["quality"] = s.profileQuality;
  prof["templates"] = s.profileTemplates;
  prof["recordings"] = s.profileRecordings;
  prof["periodHz"] = s.profilePeriodHz;
  prof["rhythmThreshold"] = s.profileRhythmTh;
  prof["steadyThreshold"] = s.profileSteadyTh;
  prof["noiseCount"] = s.noiseCount;
  prof["maxGrinders"] = cfg::kMaxGrinders;
  JsonArray grinders = prof["grinders"].to<JsonArray>();
  for (uint8_t i = 0; i < s.grinderCount; ++i) grinders.add(s.grinderNames[i]);

  fillSettings(doc["settings"].to<JsonObject>(), s.settings);

  JsonArray hist = doc["history"].to<JsonArray>();
  for (size_t i = 0; i < s.historyCount; ++i) {
    JsonObject h = hist.add<JsonObject>();
    h["ms"] = s.history[i].durationMs;
    h["epoch"] = static_cast<uint32_t>(s.history[i].endEpoch);
    if (!isnan(s.history[i].weightG)) h["g"] = s.history[i].weightG;
    if (!isnan(s.history[i].doseG)) h["dose"] = s.history[i].doseG;
  }

  JsonObject teach = doc["teach"].to<JsonObject>();
  teach["phase"] = TeachController::phaseName(s.teachPhase);
  teach["mode"] = TeachController::modeName(s.teachMode);
  teach["progress"] = s.teachProgress;
  teach["recordedMs"] = s.teachRecordedMs;
  teach["instruction"] = s.teachInstruction;
  teach["liveDb"] = s.teachLiveDb;
  teach["bgDb"] = s.teachBgDb;
  if (s.teachPhase == TeachPhase::Done) {
    const TeachResult& r = s.teachResult;
    JsonObject res = teach["result"].to<JsonObject>();
    res["quality"] = qualityName(r.quality);
    res["score"] = r.score;
    res["snrDb"] = r.snrDb;
    res["bgStdDb"] = r.bgStdDb;
    res["clip"] = r.clipRatio;
    res["activeSeconds"] = r.activeSeconds;
    res["longestPauseMs"] = r.longestPauseMs;
    res["coverage"] = r.coverage;
    res["falseRatio"] = r.falseRatio;
    res["selfSimilarity"] = r.selfSimilarity;
    res["bgSimilarity"] = r.bgSimilarity;
    res["pumpPeriodicity"] = r.pumpPeriodicity;
    res["bgPeriodicity"] = r.bgPeriodicity;
    res["distinctBands"] = r.distinctBands;
    res["pumpRhythm"] = r.pumpRhythm;
    res["pumpSteadiness"] = r.pumpSteadiness;
    res["steadyThreshold"] = r.profile.steadyThreshold;
    res["contrastDb"] = r.contrastDb;
    res["bgRhythm"] = r.bgRhythm;
    res["rhythmThreshold"] = r.profile.rhythmThreshold;
    res["rejectedRatio"] = r.rejectedRatio;
    res["noiseHitBefore"] = r.noiseHitBefore;
    res["noiseHitAfter"] = r.noiseHitAfter;
    res["grinderMatch"] = r.grinderMatch;
    res["grinderMatchSim"] = r.grinderMatchSim;
    res["templates"] = r.profile.templateCount;
    res["periodHz"] = r.profile.periodHz();
    res["suggestedSimilarity"] = r.suggestedSimilarity;
    res["suggestedPeriodicity"] = r.suggestedPeriodicity;
    res["suggestedOffDelayMs"] = r.suggestedOffDelayMs;
    JsonArray msgs = res["messages"].to<JsonArray>();
    for (uint8_t i = 0; i < r.messageCount; ++i) msgs.add(r.messages[i]);
  }

  JsonObject netObj = doc["net"].to<JsonObject>();
  netObj["ip"] = net::ip();
  netObj["name"] = net::networkName();
  netObj["ap"] = net::apActive();

  doc["firmware"] = cfg::kFirmwareVersion;

  JsonObject sc = doc["scale"].to<JsonObject>();
  const scale::Config scfg = scale::config();
  sc["enabled"] = scfg.enabled;
  sc["autoTare"] = scfg.autoTare;
  sc["syncTimer"] = scfg.syncTimer;
  sc["grindTare"] = scfg.grindTare;
  sc["portafilterG"] = scfg.portafilterG;
  sc["doseMode"] = s.scaleDoseMode;
  if (!isnan(s.doseG)) sc["dose"] = s.doseG;
  sc["model"] = scale::model();
  sc["state"] = s.scaleState;
  sc["connected"] = s.scaleConnected;
  if (!isnan(s.scaleWeight)) sc["weight"] = s.scaleWeight;
  sc["flow"] = s.scaleFlow;
  sc["battery"] = s.scaleBattery;
  sc["name"] = scale::deviceName();
  sc["address"] = scale::deviceAddress();
  // String(...): ArduinoJson kopiert den Text. Ein const char* würde nur als
  // Zeiger gespeichert und könnte beim Serialisieren schon ungültig sein.
  sc["boundAddr"] = String(scfg.boundAddr);
  sc["boundName"] = String(scfg.boundName);
  sc["searching"] = scale::searching();
  JsonArray found = sc["found"].to<JsonArray>();
  for (const scale::Found& f : scale::found()) {  // temporäre Liste -> Texte kopieren
    JsonObject o = found.add<JsonObject>();
    o["name"] = String(f.name);
    o["addr"] = String(f.addr);
    o["rssi"] = f.rssi;
  }

  JsonObject vib = doc["vibration"].to<JsonObject>();
  vib["available"] = s.vibAvailable;
  vib["mode"] = s.vibMode;
  vib["thresholdDb"] = s.vibThresholdDb;
  vib["levelDb"] = s.vibLevelDb;
  vib["baselineDb"] = s.vibBaselineDb;
  vib["steadiness"] = fminf(s.vibSteadiness, 20.0f);
  vib["active"] = s.vibActive;

  JsonObject mic = doc["mic"].to<JsonObject>();
  mic["gain"] = s.micGain;
  mic["clip"] = s.clipRatio;

  JsonObject bat = doc["battery"].to<JsonObject>();
  bat["level"] = battery::level();
  bat["charging"] = battery::charging();

  sendJson(doc);
}

void handleGetSettings() {
  JsonDocument doc;
  fillSettings(doc.to<JsonObject>(), app.settings());
  sendJson(doc);
}

void handlePostSettings() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    sendOk(false, "Ungültiges JSON");
    return;
  }
  DetectorSettings ds = app.settings();  // nicht übergebene Felder bleiben erhalten

  ds.similarityThreshold = in["similarityThreshold"] | ds.similarityThreshold;
  ds.periodicityThreshold = in["periodicityThreshold"] | ds.periodicityThreshold;
  ds.levelThreshold = in["levelThreshold"] | ds.levelThreshold;
  ds.onDelayMs = in["onDelayMs"] | ds.onDelayMs;
  ds.offDelayMs = in["offDelayMs"] | ds.offDelayMs;
  ds.minShotSeconds = in["minShotSeconds"] | ds.minShotSeconds;
  app.cmdApplySettings(ds);
  sendOk();
}

// --- Firmware-Update per Upload ------------------------------------------------

/// Fehlertext des laufenden Uploads (leer = alles in Ordnung).
String updateError;
size_t updateTotal = 0;  ///< Dateigröße laut Browser (für die Prozentanzeige)

/**
 * Wird vom WebServer für jedes empfangene Stück der Datei aufgerufen.
 * Das Update wird direkt in den freien App-Slot geschrieben – die Datei muss
 * nicht vollständig in den RAM passen.
 */
void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      updateError = "";
      updateTotal = server.header("X-File-Size").toInt();
      Serial.printf("[update] Upload '%s' (%u Bytes)\n", up.filename.c_str(), (unsigned)updateTotal);
      if (!up.filename.endsWith(".bin")) {
        updateError = "Bitte eine .bin-Firmwaredatei wählen (firmware.bin).";
      } else if (!Update.begin(updateTotal > 0 ? updateTotal : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        updateError = String("Update-Start fehlgeschlagen: ") + Update.errorString();
      }
      ui::showUpdateProgress(0, "Firmware-Update (Web)");
      break;

    case UPLOAD_FILE_WRITE:
      if (updateError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize) {
        updateError = String("Schreibfehler: ") + Update.errorString();
      }
      if (updateTotal > 0) {
        ui::showUpdateProgress(static_cast<int>(up.totalSize * 100 / updateTotal), "Firmware-Update (Web)");
      }
      break;

    case UPLOAD_FILE_END:
      // end(true): Größe aus den empfangenen Daten übernehmen, Prüfsumme testen
      // und den neuen Slot als Startpartition setzen.
      if (updateError.isEmpty() && !Update.end(true)) {
        updateError = String("Prüfung fehlgeschlagen: ") + Update.errorString();
      }
      break;

    case UPLOAD_FILE_ABORTED:
      Update.abort();
      updateError = "Upload abgebrochen";
      break;
  }
}

/// Antwort nach Abschluss des Uploads; bei Erfolg Neustart in die neue Firmware.
void handleUpdateDone() {
  if (updateError.isEmpty() && Update.isFinished()) {
    ui::showUpdateProgress(100, "Update fertig – Neustart …");
    sendOk(true, "Update erfolgreich – Gerät startet neu");
    restartAtMs = millis() + 1000;
  } else {
    if (Update.isRunning()) Update.abort();
    Serial.printf("[update] Fehler: %s\n", updateError.c_str());
    ui::showUpdateProgress(-1, "Update fehlgeschlagen");
    sendOk(false, updateError.isEmpty() ? "Update unvollständig" : updateError.c_str());
  }
}

// --- Profil-Slots --------------------------------------------------------------

/// Liest {"slot": n, ...} aus dem Body; -1 bei Fehler.
int slotFromBody(JsonDocument& in) {
  if (deserializeJson(in, server.arg("plain"))) return -1;
  const int slot = in["slot"] | -1;
  return (slot >= 0 && slot < (int)cfg::kProfileSlots) ? slot : -1;
}

void handleProfiles() {
  SlotSummary list[cfg::kProfileSlots];
  uint8_t active = 0;
  app.slots(list, active);
  JsonDocument doc;
  doc["active"] = active;
  JsonArray arr = doc["slots"].to<JsonArray>();
  for (const SlotSummary& s : list) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s.name;
    o["stored"] = s.stored;
    o["isDefault"] = s.isDefault;
    o["recordings"] = s.recordings;
    o["templates"] = s.templates;
    o["noise"] = s.noiseCount;
    o["grinders"] = s.grinderCount;
    o["periodHz"] = s.periodHz;
    o["quality"] = s.quality;
  }
  sendJson(doc);
}

void handleProfileExport() {
  const int slot = server.arg("slot").toInt();
  String name, pump, grinders, noise;
  if (slot < 0 || slot >= (int)cfg::kProfileSlots || !app.exportSlot(slot, name, pump, grinders, noise)) {
    sendOk(false, "Export fehlgeschlagen");
    return;
  }
  // Version 2: jeder Teil einzeln (beim Import einzeln auswählbar)
  JsonDocument doc;
  doc["format"] = "coffeetimer-profile";
  doc["version"] = 2;
  doc["firmware"] = cfg::kFirmwareVersion;
  doc["name"] = name;
  doc["pump"] = pump;          // Pumpe + Einstellungen + Profilname
  doc["grinders"] = grinders;  // Mühlen
  doc["noise"] = noise;        // Störgeräusche
  String out;
  serializeJson(doc, out);
  // Dateiname ohne Sonderzeichen
  String file = name;
  for (size_t i = 0; i < file.length(); ++i) {
    if (!isalnum(static_cast<unsigned char>(file[i]))) file.setCharAt(i, '_');
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"coffeetimer-" + file + ".json\"");
  server.send(200, "application/json", out);
}

void handleWifi() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    sendOk(false, "Ungültiges JSON");
    return;
  }
  WifiSettings w;
  w.ssid = in["ssid"] | "";
  w.password = in["password"] | "";
  w.ssid.trim();
  settings::saveWifi(w);
  sendOk(true, w.ssid.isEmpty() ? "WLAN gelöscht – Neustart im AP-Modus"
                                : "Gespeichert – Gerät startet neu");
  restartAtMs = millis() + 1000;
}

}  // namespace

namespace web {

void begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handlePostSettings);

  server.on("/api/teach/start", HTTP_POST, [] {
    // Body {"mode": "pump" | "noise" | "grinder" | "background"}
    JsonDocument in;
    deserializeJson(in, server.arg("plain"));
    app.cmdStartTeach(TeachController::modeFromName(in["mode"] | "pump"));
    sendOk();
  });
  // Profil-Slots
  server.on("/api/profiles", HTTP_GET, handleProfiles);
  server.on("/api/profiles/export", HTTP_GET, handleProfileExport);
  // Klartext eines Profils (zum Ansehen im Browser)
  server.on("/api/profiles/text", HTTP_GET, [] {
    String text;
    if (!app.slotText(server.arg("slot").toInt(), text)) return sendOk(false, "Ungültiger Profil-Platz");
    server.send(200, "text/plain; charset=utf-8", text);
  });
  server.on("/api/profiles/select", HTTP_POST, [] {
    JsonDocument in;
    const int slot = slotFromBody(in);
    if (slot < 0) return sendOk(false, "Ungültiger Slot");
    app.cmdSelectSlot(slot);
    sendOk(true, "Profil gewechselt");
  });
  server.on("/api/profiles/rename", HTTP_POST, [] {
    JsonDocument in;
    const int slot = slotFromBody(in);
    const bool ok = slot >= 0 && app.cmdRenameSlot(slot, in["name"] | "");
    sendOk(ok, ok ? "Umbenannt" : "Umbenennen fehlgeschlagen");
  });
  server.on("/api/profiles/delete", HTTP_POST, [] {
    JsonDocument in;
    const int slot = slotFromBody(in);
    const bool ok = slot >= 0 && app.cmdDeleteSlot(slot);
    sendOk(ok, ok ? "Profil gelöscht – Standardprofil aktiv" : "Löschen fehlgeschlagen");
  });
  server.on("/api/profiles/import", HTTP_POST, [] {
    // Body {"slot": n, "parts": ["pump","grinders","noise"], "pump": "...", "grinders": "...",
    //       "noise": "..."}  bzw. bei Exporten älterer Firmware "data": "..." (alle Teile)
    JsonDocument in;
    const int slot = slotFromBody(in);
    uint8_t parts = 0;
    for (JsonVariant p : in["parts"].as<JsonArray>()) {
      const String name = p.as<String>();
      if (name == "pump") parts |= settings::kPartPump;
      if (name == "grinders") parts |= settings::kPartGrinders;
      if (name == "noise") parts |= settings::kPartNoise;
    }
    if (!parts) return sendOk(false, "Nichts zum Importieren ausgewählt");
    const bool ok = slot >= 0 && app.cmdImportSlot(slot, parts, in["pump"] | "", in["grinders"] | "",
                                                   in["noise"] | "", in["data"] | "");
    sendOk(ok, ok ? "Import abgeschlossen" : "Import fehlgeschlagen (Datei ungültig oder andere Version)");
  });
  // Einzelnes Störgeräusch / Klangmuster löschen, Body {"index": n}
  server.on("/api/noise/delete", HTTP_POST, [] {
    JsonDocument in;
    deserializeJson(in, server.arg("plain"));
    const bool ok = app.cmdDeleteNoise(in["index"] | 255);
    sendOk(ok, ok ? "Störgeräusch gelöscht" : "Löschen fehlgeschlagen");
  });
  server.on("/api/templates/delete", HTTP_POST, [] {
    JsonDocument in;
    deserializeJson(in, server.arg("plain"));
    const bool ok = app.cmdDeleteTemplate(in["index"] | 255);
    sendOk(ok, ok ? "Klangmuster gelöscht" : "Nicht möglich (mind. 1 Muster bleibt)");
  });
  // Waage: Einstellungen und Tara
  server.on("/api/scale", HTTP_POST, [] {
    JsonDocument in;
    if (deserializeJson(in, server.arg("plain"))) return sendOk(false, "Ungültiges JSON");
    scale::Config c = scale::config();
    c.enabled = in["enabled"] | c.enabled;
    c.autoTare = in["autoTare"] | c.autoTare;
    c.syncTimer = in["syncTimer"] | c.syncTimer;
    c.grindTare = in["grindTare"] | c.grindTare;
    c.portafilterG = in["portafilterG"] | c.portafilterG;
    scale::setConfig(c);
    sendOk(true, c.enabled ? "Waage gespeichert – suche Waage …" : "Waage deaktiviert");
  });
  server.on("/api/scale/portafilter", HTTP_POST, [] {
    if (!scale::connected()) return sendOk(false, "Waage nicht verbunden");
    if (!scale::learnPortafilter()) return sendOk(false, "Leeren Siebträger auflegen und ruhig liegen lassen");
    char msg[48];
    snprintf(msg, sizeof(msg), "Siebträger: %.1f g", scale::config().portafilterG);
    sendOk(true, msg);
  });
  // Mühlen: löschen {"index": n} bzw. umbenennen {"index": n, "name": "..."}
  server.on("/api/grinders/delete", HTTP_POST, [] {
    JsonDocument in;
    deserializeJson(in, server.arg("plain"));
    const bool ok = app.cmdDeleteGrinder(in["index"] | 255);
    sendOk(ok, ok ? "Mühle gelöscht" : "Löschen fehlgeschlagen");
  });
  server.on("/api/grinders/rename", HTTP_POST, [] {
    JsonDocument in;
    deserializeJson(in, server.arg("plain"));
    const bool ok = app.cmdRenameGrinder(in["index"] | 255, in["name"] | "");
    sendOk(ok, ok ? "Umbenannt" : "Umbenennen fehlgeschlagen");
  });
  // Waagen suchen / koppeln: Body {"addr": "...", "name": "..."}
  server.on("/api/scale/search", HTTP_POST, [] {
    if (!scale::config().enabled) return sendOk(false, "Waage zuerst einschalten");
    scale::startSearch();
    sendOk(true, "Suche läuft (ca. 5 s) …");
  });
  server.on("/api/scale/bind", HTTP_POST, [] {
    JsonDocument in;
    deserializeJson(in, server.arg("plain"));
    const char* addr = in["addr"] | "";
    if (strlen(addr) < 11) return sendOk(false, "Ungültige Adresse");
    scale::bind(addr, in["name"] | "");
    sendOk(true, "Waage gekoppelt – es wird nur noch mit ihr verbunden");
  });
  server.on("/api/scale/unbind", HTTP_POST, [] {
    scale::unbind();
    sendOk(true, "Kopplung gelöst – erste passende Waage wird verwendet");
  });
  server.on("/api/scale/timer-reset", HTTP_POST, [] {
    if (!scale::connected()) return sendOk(false, "Waage nicht verbunden");
    scale::timerStop();
    scale::timerReset();
    sendOk(true, "Waagen-Zeit auf 0");
  });
  server.on("/api/scale/tare", HTTP_POST, [] {
    if (!scale::connected()) return sendOk(false, "Waage nicht verbunden");
    scale::tare();
    sendOk(true, "Tariert");
  });
  // Erkennungsquelle: Body {"mode": 0|1|2, "thresholdDb": x}
  server.on("/api/vibration", HTTP_POST, [] {
    JsonDocument in;
    if (deserializeJson(in, server.arg("plain"))) return sendOk(false, "Ungültiges JSON");
    vibration::Config c = vibration::config();
    c.mode = static_cast<vibration::Mode>(in["mode"] | static_cast<int>(c.mode));
    c.thresholdDb = in["thresholdDb"] | c.thresholdDb;
    vibration::setConfig(c);
    sendOk(true, (String("Erkennung: ") + vibration::modeName(vibration::config().mode)).c_str());
  });
  server.on("/api/mic", HTTP_POST, [] {
    JsonDocument in;
    if (deserializeJson(in, server.arg("plain"))) return sendOk(false, "Ungültiges JSON");
    app.cmdSetMicGain(in["gain"] | cfg::kMicGainDefault);
    sendOk(true, "Mikrofon-Verstärkung gesetzt");
  });

  server.on("/api/diag", HTTP_GET, [] {
    String text;
    app.diagnosticsText(text);
    server.send(200, "text/plain; charset=utf-8", text);
  });
  server.on("/api/noise/clear", HTTP_POST, [] { app.cmdClearNoise(); sendOk(true, "Störgeräusche gelöscht"); });
  server.on("/api/teach/cancel", HTTP_POST, [] { app.cmdCancelTeach(); sendOk(); });
  server.on("/api/teach/finish", HTTP_POST, [] { app.cmdFinishTeach(); sendOk(); });
  server.on("/api/teach/save", HTTP_POST, [] {
    // Body {"mode": "replace" | "add", "keepSettings": bool}
    JsonDocument in;
    deserializeJson(in, server.arg("plain"));
    const bool add = strcmp(in["mode"] | "replace", "add") == 0;
    String note;
    const bool ok = app.cmdSaveTeach(add, &note, in["keepSettings"] | false);
    sendOk(ok, ok ? note.c_str() : "Ergebnis fehlerhaft – nicht speicherbar");
  });
  server.on("/api/timer/reset", HTTP_POST, [] { app.cmdResetTimer(); sendOk(); });
  server.on("/api/history/clear", HTTP_POST, [] { app.cmdClearHistory(); sendOk(); });
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  // Dateigröße wird vom Browser als Header mitgeschickt (für die Fortschrittsanzeige).
  static const char* headers[] = {"X-File-Size"};
  server.collectHeaders(headers, 1);

  server.onNotFound([] { server.send(404, "text/plain", "Nicht gefunden"); });
  server.begin();
}

void loop() {
  server.handleClient();
  if (restartAtMs && static_cast<int32_t>(millis() - restartAtMs) >= 0) {
    ESP.restart();
  }
}

}  // namespace web
