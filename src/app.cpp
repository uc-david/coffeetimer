// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file app.cpp
 * @brief Implementierung der zentralen Anwendungslogik.
 */
#include "app.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <math.h>

#include <new>

#include "scale.h"
#include "vibration.h"

App& app() {
  // Das Objekt ist ~80 kB groß. Im internen RAM fehlt dieser Platz sonst
  // WLAN/Bluetooth (Webseite fällt aus). PSRAM steht erst ab setup() bereit
  // (initArduino), daher erst beim ersten Aufruf anlegen statt als Global.
  static App* const instance = [] {
    void* mem = heap_caps_malloc(sizeof(App), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = ::operator new(sizeof(App));  // ohne PSRAM: intern
    return new (mem) App();
  }();
  return *instance;
}

namespace {
// Großzügig: die Teach-Auswertung läuft ebenfalls in diesem Task.
constexpr uint32_t kAudioTaskStack = 16384;
constexpr UBaseType_t kAudioTaskPriority = 3;
constexpr BaseType_t kAudioTaskCore = 0;  // Core 1 gehört loop() (Display/Web)
}  // namespace

void App::begin() {
  mutex_ = xSemaphoreCreateMutex();
  // Zeiger bleiben gültig, Inhalt wechselt mit dem Slot
  detector_.setGrinders(&grinders_);
  detector_.setClassifier(&classifier_);

  activeSlot_ = settings::loadActiveSlot();
  micGain_ = settings::loadMicGain();
  requestedGain_ = micGain_;
  {
    Lock lock(mutex_);
    loadSlotLocked(activeSlot_);
    refreshSummariesLocked();
  }

  extractor_.begin();
  teach_.begin();

  // Beim CoreS3 teilen sich Lautsprecher und Mikrofon den I2S-Bus:
  // Lautsprecher abschalten, bevor das Mikrofon gestartet wird.
  M5.Speaker.end();
  auto micCfg = M5.Mic.config();
  micCfg.sample_rate = cfg::kSampleRate;
  micCfg.noise_filter_level = 0;  // keine Glättung -> unverfälschtes Spektrum
  micCfg.magnification = micGain_;
  M5.Mic.config(micCfg);
  M5.Mic.begin();

  xTaskCreatePinnedToCore(audioTaskEntry, "audio", kAudioTaskStack, this,
                          kAudioTaskPriority, nullptr, kAudioTaskCore);
}

void App::loop() {
  // Schneller Vorab-Check ohne Mutex; Details unter dem Mutex in flushSettings()
  if (settingsDirty_ && millis() - settingsChangedMs_ >= cfg::kSettingsSaveDelayMs) flushSettings();
}

void App::flushSettings() {
  Lock lock(mutex_);
  if (settingsDirty_) persistLocked(settings::kPartPump);  // setzt settingsDirty_ zurück
}

void App::audioTaskEntry(void* arg) {
  static_cast<App*>(arg)->audioTask();
}

void App::audioTask() {
  // Drei rotierende Puffer: record() reiht einen Puffer ein und blockiert, bis
  // in der internen Warteschlange (2 Plätze) Platz ist. Damit ist der Puffer,
  // der zwei Aufrufe zuvor eingereiht wurde, sicher vollständig gefüllt –
  // die Aufnahme läuft lückenlos weiter, während wir ihn auswerten.
  static int16_t buffers[3][cfg::kFrameSize];
  size_t idx = 0;
  uint32_t warmup = 0;

  for (;;) {
    // Geänderte Mikrofon-Verstärkung übernehmen (Mikrofon kurz neu starten).
    const uint8_t wanted = requestedGain_.load();
    if (wanted != micGain_) {
      applyMicGain(wanted);
      warmup = 0;
    }

    if (!M5.Mic.record(buffers[idx], cfg::kFrameSize, cfg::kSampleRate)) {
      vTaskDelay(pdMS_TO_TICKS(10));  // Mikrofon nicht bereit
      continue;
    }
    idx = (idx + 1) % 3;
    const int16_t* ready = buffers[idx];  // ältester Puffer = fertig

    // Erste Frames verwerfen (leere Puffer, Einschwingen des Codecs).
    if (warmup < cfg::kWarmupFrames) {
      ++warmup;
      continue;
    }

    AudioFrame frame;
    extractor_.process(ready, frame);  // FFT außerhalb des Mutex
    frame.timestampMs = millis();

    Lock lock(mutex_);
    handleFrame(frame);
  }
}

void App::applyMicGain(uint8_t gain) {
  M5.Mic.end();
  auto micCfg = M5.Mic.config();
  micCfg.magnification = gain;
  M5.Mic.config(micCfg);
  M5.Mic.begin();
  extractor_.begin();  // Glättung zurücksetzen (Pegelsprung)
  Lock lock(mutex_);
  micGain_ = gain;
  detector_.reset();
  Serial.printf("[mic] Verstärkung %u\n", gain);
}

void App::handleFrame(AudioFrame& f) {
  ++audioFrames_;
  clipPeak_ = fmaxf(f.clipRatio, clipPeak_ * 0.97f);  // Spitzenwert, klingt in ~1 s ab

  // Frames bleiben unverändert (aktuelle Verstärkung). Jedes Geräusch – Pumpe,
  // Mühle, Störgeräusch – rechnet selbst auf die Verstärkung seiner Aufnahme um.
  detector_.setMicGain(micGain_);

  // Während des Teach-Modus (inkl. Ergebnisanzeige) ruht die Erkennung,
  // damit das Lernen nicht versehentlich Bezüge auslöst.
  if (teach_.engaged()) {
    teach_.process(f);
    return;
  }

  const vibration::Status vib = vibration::status();
  const auto source = vib.available ? static_cast<PumpDetector::Source>(vibration::config().mode)
                                    : PumpDetector::Source::Mic;
  detector_.setVibration(source, vib.active, vib.sinceMs);
  const PumpDetector::Event ev = detector_.process(f);
  const DetectorStatus& st = detector_.status();
  if (st.rejected) ++rejectedFrames_;
  handleGrinder(f);
  if (ev == PumpDetector::Event::Started) current_ = DetectionEvent{};
  if (st.pumpOn || ev == PumpDetector::Event::Stopped) accumulateEvent(st.scores);

  switch (ev) {
    case PumpDetector::Event::Started: {
      timer_.start(detector_.onsetMs());
      weightUntilMs_ = 0;
      // Waage: Dosier-Tara beenden, tarieren und ggf. ihren Timer mitstarten
      scale::clearDoseOffset();
      const scale::Config sc = scale::config();
      if (scale::connected()) {
        if (sc.autoTare) scale::tare();
        if (sc.syncTimer) {
          scale::timerReset();
          scale::timerStart();
        }
      }
      Serial.printf("[detect] Pumpe EIN (ki=%.2f lvl=%.2f)\n", st.scores.clsPump, st.scores.levelRatio);
      break;
    }
    case PumpDetector::Event::Stopped: {
      // Diagnose-Eintrag abschließen
      current_.durationMs = detector_.lastActiveMs() - detector_.onsetMs();
      const time_t now = time(nullptr);
      current_.endEpoch = now > cfg::kValidEpoch ? now : 0;
      if (current_.frames > 0) {
        FrameScores& m = current_.mean;
        const float n = current_.frames;
        m.levelRatio /= n;
        m.steadiness /= n;
        m.clsPump /= n;
      }
      if (eventCount_ == kMaxEvents) {
        memmove(&events_[0], &events_[1], sizeof(DetectionEvent) * (kMaxEvents - 1));
        --eventCount_;
      }
      events_[eventCount_++] = current_;

      const uint32_t minMs = static_cast<uint32_t>(settings_.minShotSeconds * 1000.0f);
      if (timer_.stop(detector_.lastActiveMs(), minMs)) {
        Serial.printf("[detect] Pumpe AUS nach %.1f s\n", timer_.displayMs(millis()) / 1000.0f);
        if (scale::connected() && scale::config().syncTimer) scale::timerStop();
        // Endgewicht: jetzt übernehmen und während des Nachtropfens nachführen
        if (timer_.lastStopRecorded() && !isnan(scale::weight())) {
          timer_.setLatestWeight(scale::weight());
          weightUntilMs_ = f.timestampMs + cfg::kWeightSettleMs;
          weightTares_ = scale::tareCount();
        }
        // Vorher gewogenes Kaffeemehl diesem Bezug zuordnen (einmalig)
        if (timer_.lastStopRecorded() && !isnan(doseG_)) {
          if (f.timestampMs - doseAtMs_ < cfg::kDoseValidMs) timer_.setLatestDose(doseG_);
          doseG_ = NAN;
        }
      }
      break;
    }
    case PumpDetector::Event::None:
      // Nachtropfen: Gewicht des letzten Bezugs noch einige Sekunden aktualisieren –
      // nicht mehr nach einer Tara (RESET, TARA, Mühle), sonst stünde dort 0 g.
      if (weightUntilMs_) {
        const float w = scale::weight();
        const bool tared = scale::tareCount() != weightTares_;
        if (!tared && static_cast<int32_t>(f.timestampMs - weightUntilMs_) < 0 && !isnan(w)) {
          timer_.setLatestWeight(w);
        } else {
          weightUntilMs_ = 0;
        }
      }
      break;
  }
}

void App::handleGrinder(const AudioFrame& f) {
  const bool on = detector_.status().grinderOn;
  if (on && !grinderWasOn_) {
    const int8_t gi = detector_.status().grinderIndex;
    Serial.printf("[detect] %s läuft\n", gi >= 0 && gi < grinders_.count ? grinders_.names[gi] : "Mühle");
    ++grinderStarts_;
    // Nachmahlen (Dosier-Tara läuft noch) nicht neu tarieren – die Waage ist
    // dann ggf. schon mit der ersten Portion belegt worden.
    const bool recent = scale::doseMode() && f.timestampMs - doseTareMs_ < cfg::kDoseValidMs;
    const scale::Config sc = scale::config();
    if (sc.grindTare && scale::connected() && !recent) {
      scale::tareForDose();  // Anzeige: -Siebträger, mit gefülltem Siebträger = Dosis
      doseG_ = NAN;
      doseTareMs_ = f.timestampMs;
    }
    // Neuer Bezug wird vorbereitet: Bezugszeit (Anzeige, Historie bleibt) und
    // Timer der Waage auf 0 – sie stehen sonst noch auf dem letzten Bezug.
    if (!timer_.running()) {
      timer_.reset();
      if (sc.syncTimer && scale::connected()) scale::timerReset();
    }
  }
  grinderWasOn_ = on;

  // Dosis: letzter ruhiger, positiver Wert während der Dosier-Tara
  if (scale::doseMode() && scale::stable()) {
    const float w = scale::weight();
    if (!isnan(w) && w >= cfg::kMinDoseG) {
      doseG_ = w;
      doseAtMs_ = f.timestampMs;
    }
  }
}

void App::accumulateEvent(const FrameScores& in) {
  // "Noch nicht messbar" (99 dB) würde den Mittelwert verfälschen -> begrenzen.
  FrameScores s = in;
  s.steadiness = fminf(s.steadiness, 20.0f);
  DetectionEvent& e = current_;
  if (e.frames == 0) {
    e.min = s;
  } else {
    e.min.levelRatio = fminf(e.min.levelRatio, s.levelRatio);
    e.min.steadiness = fmaxf(e.min.steadiness, s.steadiness);  // "Min" = ungünstigster Wert
    e.min.clsPump = fminf(e.min.clsPump, s.clsPump);
  }
  e.mean.levelRatio += s.levelRatio;
  e.mean.steadiness += s.steadiness;
  e.mean.clsPump += s.clsPump;
  ++e.frames;
}

void App::countFrames(const classifier::TrainingSet& t, size_t (&perKind)[4]) {
  for (size_t& n : perKind) n = 0;
  for (const classifier::Record& r : t.records) perKind[static_cast<int>(r.kind)] += r.count();
}

void App::diagnosticsText(String& out) {
  Lock lock(mutex_);
  char buf[200];
  auto add = [&](const char* fmt, auto... args) {
    snprintf(buf, sizeof(buf), fmt, args...);
    out += buf;
    out += '\n';
  };
  auto scores = [&](const char* label, const FrameScores& s) {
    add("  %-6s Pegel=%.2f Schwankung=%.1fdB KI-Pumpe=%.0f%%", label, s.levelRatio, fminf(s.steadiness, 20.0f),
        s.clsPump * 100.0f);
  };

  out = "=== Coffee Timer Diagnose ===\n";
  add("Slot %u \"%s\"%s, Mikrofon-Verstaerkung %u (Profil gelernt mit %.0f), Uebersteuerung %.2f%%",
      activeSlot_ + 1, slotName_, profile_.isDefault() ? " (Standardprofil)" : "", micGain_,
      profile_.micGain, clipPeak_ * 100.0f);
  add("Firmware %s, Laufzeit %lu min, Frames %lu, als Stoergeraeusch verworfen: %lu",
      cfg::kFirmwareVersion, (unsigned long)(millis() / 60000), (unsigned long)audioFrames_,
      (unsigned long)rejectedFrames_);
  add("Speicher frei: intern %u kB (min %u kB), PSRAM %u kB",
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

  const PumpProfile& p = profile_;
  add("Profil: gueltig=%d Aufnahmen=%u SchwankungMax=%.1fdB Hintergrund %.1f dB, Pumpe(leise) %.1f dB, "
      "Qualitaet %.0f, Stoergeraeusche %u",
      p.valid, p.recordings, p.steadyThreshold, p.bgRmsDb, p.pumpRmsDb, p.quality, noise_.count);
  const DetectorSettings& s = settings_;
  add("Schwellen: Pegel=%.2f KI=%.2f (halten %.2f) Ein=%ums Aus=%ums MinBezug=%.0fs", s.levelThreshold,
      cfg::kClsMinProb, cfg::kClsHoldProb, s.onDelayMs, s.offDelayMs, s.minShotSeconds);
  if (!lastPumpTeach_.isEmpty()) out += "Teach Pumpe: " + lastPumpTeach_ + "\n";
  if (!lastNoiseTeach_.isEmpty()) out += "Teach Stoer: " + lastNoiseTeach_ + "\n";

  scale::diagnostics(out);
  {
    const vibration::Status v = vibration::status();
    add("Vibration: Modus %s, Sensor %s, Pegel %.1f dB, Ruhepegel %.1f dB, Schwelle +%.1f dB, "
        "Schwankung %.1f dB, aktiv=%d", vibration::modeName(vibration::config().mode),
        v.available ? "ok" : "fehlt", v.levelDb, v.baselineDb, vibration::config().thresholdDb,
        fminf(v.steadiness, 20.0f), v.active);
  }
  {
    String names;
    for (uint8_t i = 0; i < grinders_.count; ++i) {
      const NoiseProfile::Entry& e = grinders_.entries[i];
      char lvl[48];
      snprintf(lvl, sizeof(lvl), " (Pegel %.1f / Hintergrund %.1f dB)", e.levelDb, e.bgRmsDb);
      names += (i ? ", " : "") + String(grinders_.names[i]) + lvl;
    }
    add("Muehlen: %u [%s], erkannt %lu x, Dosier-Tara %s, Dosis %.1f g", grinders_.count,
        names.c_str(), (unsigned long)grinderStarts_, scale::doseMode() ? "aktiv" : "aus", doseG_);
  }

  {
    size_t perKind[4];
    countFrames(training_, perKind);
    add("KI-Erkennung: %s, %u Klassen, Trefferquote %.0f%%, Aufnahmen %u, Klangbilder: Pumpe %u, Muehlen %u, "
        "Stoer %u, Ruhe %u", classifier_.ready() ? "trainiert" : "nicht trainiert (Standardprofil)",
        (unsigned)classifier_.classCount(), classifier_.trainingAccuracy() * 100.0f,
        (unsigned)training_.records.size(), (unsigned)perKind[0], (unsigned)perKind[1], (unsigned)perKind[2],
        (unsigned)perKind[3]);
    const FrameScores& sc = detector_.status().scores;
    if (sc.clsActive) {
      add("  live: %s %u (%.0f%%), Pumpe %.0f%%", classifier::kindName(sc.clsKind), sc.clsIndex + 1,
          sc.clsProb * 100.0f, sc.clsPump * 100.0f);
    }
  }

  out += "\nJetzt:\n";
  scores("live", detector_.status().scores);
  add("  Pumpe=%d roh=%d verworfen=%d Pegel=%.1f dBFS, Hintergrund %.1f dBFS%s (umgerechnet auf "
      "Profil-Verstaerkung)", detector_.status().pumpOn, detector_.status().rawActive,
      detector_.status().rejected, detector_.status().rmsDb, detector_.status().bgDb,
      profile_.isDefault() ? " automatisch" : "");

  add("\nLetzte Erkennungen (neueste zuerst): %u", (unsigned)eventCount_);
  for (size_t i = eventCount_; i-- > 0;) {
    const DetectionEvent& e = events_[i];
    char when[16] = "-";
    if (e.endEpoch) {
      struct tm t;
      localtime_r(&e.endEpoch, &t);
      strftime(when, sizeof(when), "%H:%M:%S", &t);
    }
    add("#%u %s  Dauer %.1f s  (%u Frames)", (unsigned)(eventCount_ - i), when,
        e.durationMs / 1000.0f, e.frames);
    scores("Mittel", e.mean);
    scores("Min", e.min);
  }
}

void App::snapshot(StatusSnapshot& out) {
  Lock lock(mutex_);
  const uint32_t now = millis();

  out.activeSlot = activeSlot_;
  memcpy(out.profileName, slotName_, sizeof(out.profileName));
  out.profileDefault = profile_.isDefault();
  out.lastSaveOk = lastSaveOk_;
  out.micGain = micGain_;
  out.scaleEnabled = scale::config().enabled;
  out.scaleConnected = scale::connected();
  out.scaleState = scale::stateText();
  out.scaleWeight = scale::weight();
  out.scaleFlow = scale::flowRate();
  out.scaleBattery = scale::battery();
  out.scaleDoseMode = scale::doseMode();
  out.doseG = doseG_;
  out.grinderCount = grinders_.count;
  memcpy(out.grinderNames, grinders_.names, sizeof(out.grinderNames));
  out.grinderActive = detector_.status().grinderIndex;
  out.grinderOn = detector_.status().grinderOn;
  out.clsReady = classifier_.ready();
  out.clsPumpKnown = classifier_.hasKind(classifier::Kind::Pump);
  out.clsClasses = classifier_.classCount();
  out.clsFrames = classifier_.frameCount();
  out.clsAccuracy = classifier_.trainingAccuracy();
  out.clipRatio = clipPeak_;
  {
    const vibration::Status v = vibration::status();
    const vibration::Config vc = vibration::config();
    out.vibMode = static_cast<uint8_t>(vc.mode);
    out.vibAvailable = v.available;
    out.vibActive = v.active;
    out.vibLevelDb = v.levelDb;
    out.vibBaselineDb = v.baselineDb;
    out.vibThresholdDb = vc.thresholdDb;
    out.vibSteadiness = v.steadiness;
  }

  out.profileValid = profile_.valid;
  out.profileBgDb = profile_.bgRmsDb;
  out.profilePumpDb = profile_.pumpRmsDb;
  out.profileQuality = profile_.quality;
  out.profileRecordings = profile_.recordings;
  out.noiseCount = noise_.count;
  out.profileSteadyTh = profile_.isDefault() ? cfg::kDefaultSteadyDb : profile_.steadyThreshold;

  out.detector = detector_.status();
  out.settings = settings_;

  out.timerRunning = timer_.running();
  out.timerMs = timer_.displayMs(now);
  out.historyCount = timer_.historyCount();
  for (size_t i = 0; i < out.historyCount; ++i) out.history[i] = timer_.history(i);

  out.teachPhase = teach_.phase();
  out.teachMode = teach_.mode();
  out.teachProgress = teach_.progress();
  out.teachRecordedMs = teach_.recordedMs();
  out.teachInstruction = teach_.instruction();
  out.teachLiveDb = teach_.liveRmsDb();
  out.teachBgDb = teach_.backgroundRmsDb();
  if (teach_.phase() == TeachPhase::Done) out.teachResult = teach_.result();

  out.audioFrames = audioFrames_;
}

DetectorSettings App::settings() {
  Lock lock(mutex_);
  return settings_;
}

void App::cmdStartTeach(TeachMode mode) {
  Lock lock(mutex_);
  // Standardprofil: Bezugsgröße ist der automatisch gemessene Raumpegel,
  // nicht der feste Startwert (vor reset() auslesen).
  PumpProfile ref = profile_;
  if (ref.isDefault()) {
    ref.bgRmsDb = detector_.status().bgDb;
    ref.pumpRmsDb = ref.bgRmsDb + cfg::kDefaultSpanDb;
    ref.steadyThreshold = cfg::kDefaultSteadyDb;
  }
  timer_.reset();
  detector_.reset();
  teach_.start(millis(), mode, settings_, ref, &grinders_, noise_.count, &classifier_, micGain_);
  Serial.println("[teach] gestartet");
}

void App::cmdCancelTeach() {
  Lock lock(mutex_);
  teach_.cancel();
  detector_.reset();
  Serial.println("[teach] abgebrochen");
}

void App::cmdFinishTeach() {
  Lock lock(mutex_);
  teach_.finish();
}

bool App::cmdSaveTeach(bool add, String* note) {
  Lock lock(mutex_);
  if (teach_.phase() != TeachPhase::Done || teach_.result().quality == TeachQuality::Error) {
    return false;
  }
  const TeachResult& r = teach_.result();
  String msg;
  uint8_t parts = 0;

  // Kennwerte für den Diagnosebericht merken.
  char info[220];
  if (isNoiseMode(r.mode)) {
    snprintf(info, sizeof(info), "%s: bisher als Pumpe %.0f%% (%.1f s) bg %.1f dB pegel %.1f dB %s",
             r.mode == TeachMode::Grinder ? "Muehle" : "Stoer", r.clsAsPump * 100.0f, r.activeSeconds,
             r.noise.bgRmsDb, r.noise.levelDb, add ? "(ergaenzt)" : "");
    lastNoiseTeach_ = info;
  } else {
    snprintf(info, sizeof(info), "snr=%.1f bgStd=%.1f schwankung(p95)=%.1fdB pause=%.1fs ki-pumpe=%.0f%% %.1fs %s",
             r.snrDb, r.bgStdDb, r.pumpSteadiness, r.longestPauseMs / 1000.0f, r.clsAsPump * 100.0f,
             r.activeSeconds, add ? "(ergaenzt)" : "");
    lastPumpTeach_ = info;
  }

  if (r.mode == TeachMode::Grinder) {
    // Mühle: neue Mühle anlegen oder einer vorhandenen hinzufügen (die von der
    // KI erkannte bzw. die erste, wenn die Liste voll ist)
    GrinderProfile& g = grinders_;
    const bool full = g.count >= cfg::kMaxGrinders;
    if ((add || full) && g.count > 0) {
      const uint8_t target = r.grinderMatch >= 0 && r.grinderMatch < g.count ? r.grinderMatch : 0;
      PumpDetector::mergeNoise(g.entries[target], r.noise);
      learnFromTeachLocked(classifier::Kind::Grinder, target, false);
      msg = String(g.names[target]) + " ergänzt" + (full && !add ? " – Liste voll" : "");
    } else {
      const uint8_t i = g.count++;
      g.entries[i] = r.noise;
      snprintf(g.names[i], GrinderProfile::kNameLen, "Mühle %u", (unsigned)(i + 1));
      learnFromTeachLocked(classifier::Kind::Grinder, i, false);
      msg = String(g.names[i]) + " gespeichert";
    }
    parts = settings::kPartGrinders;
  } else if (r.mode == TeachMode::Noise) {
    if (add && noise_.count > 0) {
      // Weitere Aufnahme desselben Geräts: in das von der KI erkannte Störgeräusch übernehmen
      const uint8_t target =
          r.noiseMatch >= 0 && r.noiseMatch < noise_.count ? r.noiseMatch : noise_.count - 1;
      PumpDetector::mergeNoise(noise_.entries[target], r.noise);
      learnFromTeachLocked(classifier::Kind::Noise, target, false);
      msg = "Störgeräusch " + String(target + 1) + " ergänzt";
    } else {
      // Anhängen; ist die Liste voll, fällt das älteste heraus (wurde vorher angekündigt).
      if (noise_.count >= cfg::kMaxNoises) {
        memmove(&noise_.entries[0], &noise_.entries[1], sizeof(NoiseProfile::Entry) * (cfg::kMaxNoises - 1));
        --noise_.count;
        training_.removeEntry(classifier::Kind::Noise, 0);  // Trainingsdaten mitverschieben
      }
      noise_.entries[noise_.count++] = r.noise;
      learnFromTeachLocked(classifier::Kind::Noise, noise_.count - 1, false);
      msg = "Störgeräusch gespeichert (" + String(noise_.count) + " gelernt)";
    }
    parts = settings::kPartNoise;
  } else {
    const uint16_t offBefore = settings_.offDelayMs;
    if (add && profile_.valid && !profile_.isDefault()) {
      // Ergänzen: Pegel so zusammenführen, dass beide Aufnahmen erkannt werden.
      // Die neue Aufnahme vorher auf die Mikrofon-Verstärkung des Profils umrechnen.
      PumpProfile incoming = r.profile;
      const float offset = PumpDetector::gainOffsetDb(profile_.micGain, r.profile.micGain);
      incoming.bgRmsDb += offset;
      incoming.pumpRmsDb += offset;
      PumpDetector::mergeProfile(profile_, incoming);
      learnFromTeachLocked(classifier::Kind::Pump, 0, false);
      msg = "Profil ergänzt";
    } else {
      // Ersetzen: Pegel aus der Aufnahme, bisherige Pumpen-Aufnahmen der KI ersetzen.
      profile_ = r.profile;
      learnFromTeachLocked(classifier::Kind::Pump, 0, true);
      msg = "Profil gespeichert";
    }
    // Ausschaltverzögerung nur verlängern (Pause in der Aufnahme) – und melden.
    if (r.suggestedOffDelayMs > settings_.offDelayMs) settings_.offDelayMs = r.suggestedOffDelayMs;
    settings::clamp(settings_);
    if (settings_.offDelayMs != offBefore) {
      char buf[64];
      snprintf(buf, sizeof(buf), ". Ausschaltverz. %.1f → %.1f s", offBefore / 1000.0f, settings_.offDelayMs / 1000.0f);
      msg += buf;
    }
    detector_.setProfile(profile_);
    detector_.setSettings(settings_);
    parts = settings::kPartPump;
  }

  if (!persistLocked(parts)) msg = "FEHLER: konnte nicht gespeichert werden!";
  // Ergebnis der neu trainierten KI anhängen
  if (classifier_.ready()) {
    char buf[64];
    snprintf(buf, sizeof(buf), " – KI neu trainiert (Trefferquote %.0f %%)", classifier_.trainingAccuracy() * 100.0f);
    msg += buf;
  }
  detector_.reset();
  teach_.cancel();
  Serial.printf("[teach] %s\n", msg.c_str());
  if (note) *note = msg;
  return true;
}

bool App::cmdDeleteGrinder(uint8_t index) {
  Lock lock(mutex_);
  GrinderProfile& g = grinders_;
  if (index >= g.count) return false;
  memmove(&g.entries[index], &g.entries[index + 1], sizeof(g.entries[0]) * (g.count - index - 1));
  memmove(g.names[index], g.names[index + 1], sizeof(g.names[0]) * (g.count - index - 1));
  --g.count;
  training_.removeEntry(classifier::Kind::Grinder, index);
  retrainLocked(true);
  detector_.reset();
  Serial.printf("[profil] Mühle %u gelöscht\n", index + 1);
  return persistLocked(settings::kPartGrinders);
}

bool App::cmdRenameGrinder(uint8_t index, const String& name) {
  Lock lock(mutex_);
  String n = name;
  n.trim();
  if (index >= grinders_.count || n.isEmpty()) return false;
  strlcpy(grinders_.names[index], n.c_str(), GrinderProfile::kNameLen);
  return persistLocked(settings::kPartGrinders);
}

void App::cmdClearNoise() {
  Lock lock(mutex_);
  noise_.count = 0;
  training_.removeKind(classifier::Kind::Noise);
  retrainLocked(true);
  persistLocked(settings::kPartNoise);
  Serial.println("[teach] Störgeräusche gelöscht");
}

bool App::cmdDeleteNoise(uint8_t index) {
  Lock lock(mutex_);
  if (index >= noise_.count) return false;
  memmove(&noise_.entries[index], &noise_.entries[index + 1],
          sizeof(NoiseProfile::Entry) * (noise_.count - index - 1));
  --noise_.count;
  training_.removeEntry(classifier::Kind::Noise, index);
  retrainLocked(true);
  Serial.printf("[profil] Störgeräusch %u gelöscht\n", index + 1);
  return persistLocked(settings::kPartNoise);
}

void App::cmdResetTimer(bool withScale) {
  Lock lock(mutex_);
  timer_.reset();
  // Alles auf 0: auch Gewicht (Tara, beendet die Dosier-Tara) und Timer der Waage
  if (withScale && scale::connected()) {
    scale::tare();
    scale::timerStop();  // falls er noch läuft
    scale::timerReset();
  }
}

void App::cmdClearHistory() {
  Lock lock(mutex_);
  timer_.clearHistory();
}

void App::cmdApplySettings(DetectorSettings s) {
  settings::clamp(s);
  Lock lock(mutex_);
  settings_ = s;
  detector_.setSettings(s);
  settingsDirty_ = true;  // gespeichert wird verzögert in loop()
  settingsChangedMs_ = millis();
}

// ---------------------------------------------------------------------------
// Profil-Slots
// ---------------------------------------------------------------------------

namespace {
/// Arbeitspuffer für Slot-Operationen (~20 kB, nur unter Mutex benutzt),
/// einmalig im PSRAM statt im knappen internen RAM.
ProfileSlot& slotBuf() {
  static ProfileSlot* buf = [] {
    void* mem = heap_caps_malloc(sizeof(ProfileSlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return new (mem ? mem : malloc(sizeof(ProfileSlot))) ProfileSlot();
  }();
  return *buf;
}
}  // namespace

void App::summarize(const ProfileSlot& s, bool stored, SlotSummary& out) {
  memcpy(out.name, s.name, sizeof(out.name));
  out.stored = stored;
  out.isDefault = s.pump.isDefault();
  out.recordings = s.pump.recordings;
  out.noiseCount = s.noise.count;
  out.grinderCount = s.grinders.count;
  out.quality = s.pump.quality;
}

void App::loadSlotLocked(uint8_t slot) {
  activeSlot_ = slot < cfg::kProfileSlots ? slot : 0;
  settingsDirty_ = false;  // ungespeicherte Änderungen des alten Stands verwerfen
  settings::loadSlot(activeSlot_, slotBuf());  // leer -> Standardprofil
  profile_ = slotBuf().pump;
  noise_ = slotBuf().noise;
  settings_ = slotBuf().settings;
  grinders_ = slotBuf().grinders;
  memcpy(slotName_, slotBuf().name, sizeof(slotName_));
  detector_.setProfile(profile_);
  detector_.setSettings(settings_);
  settings::loadTraining(activeSlot_, training_);
  retrainLocked(false);
  Serial.printf("[profil] Slot %u aktiv: %s%s\n", activeSlot_ + 1, slotName_,
                profile_.isDefault() ? " (Standardprofil)" : "");
}

bool App::persistLocked(uint8_t parts) {
  if (parts & settings::kPartPump) settingsDirty_ = false;  // Einstellungen liegen in der Pumpen-Datei
  memcpy(slotBuf().name, slotName_, sizeof(slotBuf().name));
  slotBuf().pump = profile_;
  slotBuf().noise = noise_;
  slotBuf().settings = settings_;
  slotBuf().grinders = grinders_;
  lastSaveOk_ = settings::saveSlot(activeSlot_, slotBuf(), parts);  // nur die geänderten Dateien
  summarize(slotBuf(), lastSaveOk_, summaries_[activeSlot_]);
  if (!lastSaveOk_) Serial.println("[profil] FEHLER: Speichern fehlgeschlagen");
  return lastSaveOk_;
}

void App::refreshSummariesLocked() {
  for (uint8_t i = 0; i < cfg::kProfileSlots; ++i) {
    const bool stored = settings::loadSlot(i, slotBuf());
    summarize(slotBuf(), stored, summaries_[i]);
  }
}

void App::slots(SlotSummary (&out)[cfg::kProfileSlots], uint8_t& active) {
  Lock lock(mutex_);
  for (size_t i = 0; i < cfg::kProfileSlots; ++i) out[i] = summaries_[i];
  active = activeSlot_;
}

void App::cmdSelectSlot(uint8_t slot) {
  if (slot >= cfg::kProfileSlots) return;
  Lock lock(mutex_);
  if (settingsDirty_) persistLocked(settings::kPartPump);  // offene Änderungen gehören zum bisherigen Slot
  teach_.cancel();
  timer_.reset();
  loadSlotLocked(slot);
  settings::saveActiveSlot(activeSlot_);
}

bool App::cmdRenameSlot(uint8_t slot, const String& name) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  String n = name;
  n.trim();
  if (n.isEmpty()) return false;
  if (slot == activeSlot_) {
    strlcpy(slotName_, n.c_str(), sizeof(slotName_));
    return persistLocked(settings::kPartPump);
  }
  settings::loadSlot(slot, slotBuf());
  strlcpy(slotBuf().name, n.c_str(), sizeof(slotBuf().name));
  const bool ok = settings::saveSlot(slot, slotBuf(), settings::kPartPump);  // Name liegt in der Pumpen-Datei
  summarize(slotBuf(), ok, summaries_[slot]);
  return ok;
}

bool App::cmdDeleteSlot(uint8_t slot) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  const bool ok = settings::deleteSlot(slot);
  settings::defaultSlot(slot, slotBuf());
  summarize(slotBuf(), false, summaries_[slot]);
  if (slot == activeSlot_) {
    teach_.cancel();
    loadSlotLocked(slot);
  }
  return ok;
}

ProfileSlot& App::slotLocked(uint8_t slot) {
  ProfileSlot& s = slotBuf();
  if (slot == activeSlot_) {  // aktiver Slot: aktueller Stand (auch ungespeicherte Einstellungen)
    memcpy(s.name, slotName_, sizeof(s.name));
    s.pump = profile_;
    s.noise = noise_;
    s.settings = settings_;
    s.grinders = grinders_;
  } else {
    settings::loadSlot(slot, s);
  }
  return s;
}

bool App::exportSlot(uint8_t slot, String& name, String& pump, String& grinders, String& noise, String& training) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  const ProfileSlot& s = slotLocked(slot);
  name = s.name;
  // Trainingsdaten: aktiver Slot aus dem Speicher, sonst aus der Datei
  classifier::TrainingSet other;
  if (slot != activeSlot_) settings::loadTraining(slot, other);
  return settings::encodePart(s, settings::kPartPump, pump) &&
         settings::encodePart(s, settings::kPartGrinders, grinders) &&
         settings::encodePart(s, settings::kPartNoise, noise) &&
         settings::encodeTraining(slot == activeSlot_ ? training_ : other, training);
}

bool App::slotText(uint8_t slot, String& out) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  const ProfileSlot& s = slotLocked(slot);
  // Trainingsdaten: aktiver Slot aus dem Speicher, sonst aus der Datei
  classifier::TrainingSet other;
  if (slot != activeSlot_) settings::loadTraining(slot, other);
  const classifier::TrainingSet& t = slot == activeSlot_ ? training_ : other;
  char buf[220];
  auto add = [&](const char* fmt, auto... args) {
    snprintf(buf, sizeof(buf), fmt, args...);
    out += buf;
    out += '\n';
  };
  // Aufnahmen und Klangbilder der KI je Eintrag
  auto training = [&](classifier::Kind kind, uint8_t index) {
    size_t records = 0, frames = 0;
    for (const classifier::Record& r : t.records) {
      if (r.kind == kind && (kind == classifier::Kind::Pump || kind == classifier::Kind::Background || r.index == index)) {
        ++records;
        frames += r.count();
      }
    }
    char b[64];
    snprintf(b, sizeof(b), "KI: %u Aufnahme(n), %u Klangbilder", (unsigned)records, (unsigned)frames);
    return String(b);
  };
  auto sound = [&](const char* title, const NoiseProfile::Entry& e, classifier::Kind kind, uint8_t index) {
    add("%s: eigener Hintergrund %.1f dBFS, Pegel %.1f dBFS (Abstand %.1f dB), Mikrofon-Verstärkung %.0f – %s",
        title, e.bgRmsDb, e.levelDb, e.levelDb - e.bgRmsDb, e.micGain, training(kind, index).c_str());
  };

  out = "";
  add("Profil %u: %s%s", slot + 1, s.name, slot == activeSlot_ ? "  (aktiv)" : "");
  add("Firmware %s", cfg::kFirmwareVersion);

  const PumpProfile& p = s.pump;
  add("\n== Pumpe ==");
  if (p.isDefault()) {
    add("Standardprofil (noch kein Teach) – erkannt wird jedes gleichmäßige Geräusch über dem Raumpegel.");
  } else {
    add("Aufnahmen %u, Qualität %.0f/100, Mikrofon-Verstärkung %.0f", p.recordings, p.quality, p.micGain);
    add("Hintergrund %.1f dBFS, Pumpe (leiser Teil) %.1f dBFS, Abstand %.1f dB", p.bgRmsDb, p.pumpRmsDb,
        p.pumpRmsDb - p.bgRmsDb);
    add("Max. Pegelschwankung %.1f dB", p.steadyThreshold);
  }
  add("%s", training(classifier::Kind::Pump, 0).c_str());

  const DetectorSettings& st = s.settings;
  add("\n== Einstellungen ==");
  add("Pegel-Schwelle %.2f, KI-Wahrscheinlichkeit Start %.0f %% / Halten %.0f %%", st.levelThreshold,
      cfg::kClsMinProb * 100.0f, cfg::kClsHoldProb * 100.0f);
  add("Einschaltverzögerung %u ms, Ausschaltverzögerung %u ms, Mindestdauer %.0f s", st.onDelayMs,
      st.offDelayMs, st.minShotSeconds);

  add("\n== Mühlen: %u ==", s.grinders.count);
  for (uint8_t i = 0; i < s.grinders.count; ++i) {
    sound((String("\"") + s.grinders.names[i] + "\"").c_str(), s.grinders.entries[i], classifier::Kind::Grinder, i);
  }
  add("\n== Störgeräusche: %u ==", s.noise.count);
  for (uint8_t i = 0; i < s.noise.count; ++i) {
    sound(("Störgeräusch " + String(i + 1)).c_str(), s.noise.entries[i], classifier::Kind::Noise, i);
  }
  add("\n== Ruhe ==");
  add("%s (aus den Hintergrund-Phasen der Teachs)", training(classifier::Kind::Background, 0).c_str());
  return true;
}

bool App::cmdImportSlot(uint8_t slot, uint8_t parts, const String& pump, const String& grinders,
                        const String& noise, const String& training) {
  parts &= settings::kPartAll;
  if (slot >= cfg::kProfileSlots || !parts) return false;
  Lock lock(mutex_);
  // Quelle in einen eigenen (PSRAM-)Puffer lesen
  static ProfileSlot* src = [] {
    void* mem = heap_caps_malloc(sizeof(ProfileSlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return new (mem ? mem : malloc(sizeof(ProfileSlot))) ProfileSlot();
  }();
  settings::defaultSlot(slot, *src);
  const struct { uint8_t part; const String& text; } in[] = {
      {settings::kPartPump, pump}, {settings::kPartGrinders, grinders}, {settings::kPartNoise, noise}};
  for (const auto& i : in) {
    if ((parts & i.part) && !settings::decodePart(i.text, i.part, *src)) return false;
  }
  // Ziel: aktueller Stand des Slots, nur die gewählten Teile ersetzen
  if (slot == activeSlot_ && settingsDirty_) persistLocked(settings::kPartPump);
  ProfileSlot& dst = slotLocked(slot);
  if (parts & settings::kPartPump) {
    memcpy(dst.name, src->name, sizeof(dst.name));
    dst.pump = src->pump;
    dst.settings = src->settings;
  }
  if (parts & settings::kPartGrinders) dst.grinders = src->grinders;
  if (parts & settings::kPartNoise) dst.noise = src->noise;
  const bool ok = settings::saveSlot(slot, dst, parts);
  summarize(dst, ok, summaries_[slot]);
  // KI-Trainingsdaten der importierten Teile: aus der Datei übernehmen (falls
  // mitgeliefert und gewählt), sonst verwerfen – passend zu den neuen Einträgen.
  if (ok) {
    classifier::TrainingSet t, in;
    settings::loadTraining(slot, t);
    const bool withTraining = !training.isEmpty() && settings::decodeTraining(training, in);
    const struct { uint8_t part; classifier::Kind kind; } map[] = {
        {settings::kPartPump, classifier::Kind::Pump},
        {settings::kPartGrinders, classifier::Kind::Grinder},
        {settings::kPartNoise, classifier::Kind::Noise}};
    for (const auto& m : map) {
      if (!(parts & m.part)) continue;
      t.removeKind(m.kind);
      if (!withTraining) continue;
      for (classifier::Record& r : in.records) {
        if (r.kind == m.kind) t.records.push_back(r);
      }
    }
    // Ruhe-Klangbilder gehören zur Aufnahme der Pumpe -> mit ihr übernehmen
    if (withTraining && (parts & settings::kPartPump)) {
      t.removeKind(classifier::Kind::Background);
      for (classifier::Record& r : in.records) {
        if (r.kind == classifier::Kind::Background) t.records.push_back(r);
      }
    }
    settings::saveTraining(slot, t);
  }
  if (ok && slot == activeSlot_) {
    teach_.cancel();
    loadSlotLocked(slot);
  }
  Serial.printf("[profil] Import in Slot %u: %s%s%s%s\n", slot + 1, (parts & settings::kPartPump) ? "Pumpe " : "",
                (parts & settings::kPartGrinders) ? "Mühlen " : "", (parts & settings::kPartNoise) ? "Störgeräusche" : "",
                ok ? "" : " – FEHLER");
  return ok;
}

void App::learnFromTeachLocked(classifier::Kind kind, uint8_t index, bool replaceKind) {
  // Je Klasse 5 Aufnahmen (z. B. 1× Ersetzen + 4× Ergänzen), Ruhe aus den letzten 10
  // Teachs; erst darüber verdrängt eine neue Aufnahme die älteste.
  auto maxPer = [](classifier::Kind k) -> size_t {
    return k == classifier::Kind::Background ? cfg::kClsMaxBackground : cfg::kClsMaxRecords;
  };
  if (replaceKind) training_.removeKind(kind);
  classifier::Record rec;
  rec.kind = kind;
  rec.index = index;
  teach_.trainingFrames(false, rec.frames, classifier::kMaxFramesPerRecord);
  if (rec.count()) training_.add(std::move(rec), maxPer(kind));
  classifier::Record bg;
  bg.kind = classifier::Kind::Background;
  teach_.trainingFrames(true, bg.frames, classifier::kMaxFramesPerRecord);
  if (bg.count()) training_.add(std::move(bg), maxPer(classifier::Kind::Background));
  retrainLocked(true);
}

void App::retrainLocked(bool save) {
  if (save) settings::saveTraining(activeSlot_, training_);
  const uint32_t t0 = millis();
  classifier_.train(training_);
  detector_.reset();
  Serial.printf("[ki] %s: %u Klassen, %u Klangbilder, Trefferquote %.0f %% (%lu ms)\n",
                classifier_.ready() ? "trainiert" : "nicht einsatzbereit", (unsigned)classifier_.classCount(),
                (unsigned)classifier_.frameCount(), classifier_.trainingAccuracy() * 100.0f,
                (unsigned long)(millis() - t0));
}

void App::cmdSetMicGain(uint8_t gain) {
  gain = constrain(gain, cfg::kMicGainMin, cfg::kMicGainMax);
  settings::saveMicGain(gain);
  requestedGain_ = gain;  // Audio-Task übernimmt beim nächsten Durchlauf
}
