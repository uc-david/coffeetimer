/**
 * @file app.cpp
 * @brief Implementierung der zentralen Anwendungslogik.
 */
#include "app.h"

#include <M5Unified.h>
#include <math.h>

#include "scale.h"

App app;

namespace {
// Großzügig: die Teach-Auswertung läuft ebenfalls in diesem Task.
constexpr uint32_t kAudioTaskStack = 16384;
constexpr UBaseType_t kAudioTaskPriority = 3;
constexpr BaseType_t kAudioTaskCore = 0;  // Core 1 gehört loop() (Display/Web)
}  // namespace

void App::begin() {
  mutex_ = xSemaphoreCreateMutex();

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

  // Wurde das Profil mit einer anderen Mikrofon-Verstärkung gelernt, die
  // Pegel umrechnen (gleiche Verschiebung für Gesamtpegel und alle Bänder).
  // Ausnahme: Pumpen-Teach – dort wird mit der aktuellen Verstärkung gelernt.
  const bool pumpTeach = teach_.engaged() && teach_.mode() == TeachMode::Pump;
  if (!pumpTeach && profile_.micGain > 0.0f && profile_.micGain != micGain_) {
    const float offset = 20.0f * log10f(profile_.micGain / micGain_);
    f.rmsDb += offset;
    for (float& b : f.bandDb) b += offset;
  }

  // Während des Teach-Modus (inkl. Ergebnisanzeige) ruht die Erkennung,
  // damit das Lernen nicht versehentlich Bezüge auslöst.
  // extractor_ wird nur vom Audio-Task benutzt -> Rhythmus-Kurve hier ohne Wettlauf lesbar.
  if (teach_.engaged()) {
    teach_.process(f, extractor_.acfCurve());
    return;
  }

  const PumpDetector::Event ev = detector_.process(f, extractor_.acfCurve());
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
      Serial.printf("[detect] Pumpe EIN (sim=%.2f per=%.2f rhy=%.2f lvl=%.2f)\n",
                    detector_.status().scores.similarity, detector_.status().scores.periodicity,
                    detector_.status().scores.rhythm, detector_.status().scores.levelRatio);
      break;
    }
    case PumpDetector::Event::Stopped: {
      // Diagnose-Eintrag abschließen
      current_.durationMs = detector_.lastActiveMs() - detector_.onsetMs();
      const time_t now = time(nullptr);
      current_.endEpoch = now > 1600000000 ? now : 0;
      if (current_.frames > 0) {
        FrameScores& m = current_.mean;
        const float n = current_.frames;
        m.similarity /= n; m.periodicity /= n; m.rhythm /= n; m.levelRatio /= n;
        m.noiseSimilarity /= n; m.noiseRhythm /= n; m.steadiness /= n;
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
      // Nachtropfen: Gewicht des letzten Bezugs noch einige Sekunden aktualisieren
      if (weightUntilMs_) {
        const float w = scale::weight();
        if (static_cast<int32_t>(f.timestampMs - weightUntilMs_) < 0 && !isnan(w)) {
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
    Serial.println("[detect] Mühle läuft");
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
    // Neuer Bezug wird vorbereitet: Timer der Waage auf 0 (steht sonst noch auf dem letzten Bezug)
    if (sc.syncTimer && scale::connected() && !timer_.running()) scale::timerReset();
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
    e.min.similarity = fminf(e.min.similarity, s.similarity);
    e.min.periodicity = fminf(e.min.periodicity, s.periodicity);
    e.min.rhythm = fminf(e.min.rhythm, s.rhythm);
    e.min.levelRatio = fminf(e.min.levelRatio, s.levelRatio);
    e.min.noiseSimilarity = fminf(e.min.noiseSimilarity, s.noiseSimilarity);
    e.min.noiseRhythm = fminf(e.min.noiseRhythm, s.noiseRhythm);
    e.min.steadiness = fmaxf(e.min.steadiness, s.steadiness);  // "Min" = ungünstigster Wert
  }
  e.mean.steadiness += s.steadiness;
  e.mean.similarity += s.similarity;
  e.mean.periodicity += s.periodicity;
  e.mean.rhythm += s.rhythm;
  e.mean.levelRatio += s.levelRatio;
  e.mean.noiseSimilarity += s.noiseSimilarity;
  e.mean.noiseRhythm += s.noiseRhythm;
  ++e.frames;
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
    add("  %-6s Pegel=%.2f Schwankung=%.1fdB Muster=%.2f Takt=%.2f Rhythmus=%.2f | Stoer: Muster=%.2f "
        "Rhythmus=%.2f",
        label, s.levelRatio, fminf(s.steadiness, 20.0f), s.similarity, s.periodicity, s.rhythm,
        s.noiseSimilarity, s.noiseRhythm);
  };

  out = "=== Coffee Timer Diagnose ===\n";
  add("Slot %u \"%s\"%s, Mikrofon-Verstaerkung %u (Profil gelernt mit %.0f), Uebersteuerung %.2f%%",
      activeSlot_ + 1, slotName_, profile_.isDefault() ? " (Standardprofil)" : "", micGain_,
      profile_.micGain, clipPeak_ * 100.0f);
  add("Firmware %s, Laufzeit %lu min, Frames %lu, als Stoergeraeusch verworfen: %lu",
      cfg::kFirmwareVersion, (unsigned long)(millis() / 60000), (unsigned long)audioFrames_,
      (unsigned long)rejectedFrames_);

  const PumpProfile& p = profile_;
  add("Profil: gueltig=%d Aufnahmen=%u Muster=%u Takt=%d (%.1f Hz, Lag %.0f) RhythmusSchwelle=%.2f "
      "SchwankungMax=%.1fdB",
      p.valid, p.recordings, p.templateCount, p.periodic, p.periodHz(), p.periodLag,
      p.rhythmThreshold, p.steadyThreshold);
  add("        Hintergrund %.1f dB, Pumpe(leise) %.1f dB, Qualitaet %.0f, Stoergeraeusche %u",
      p.bgRmsDb, p.pumpRmsDb, p.quality, noise_.count);
  const DetectorSettings& s = settings_;
  add("Schwellen: Muster=%.2f Takt=%.2f Pegel=%.2f Ein=%ums Aus=%ums MinBezug=%.0fs",
      s.similarityThreshold, s.periodicityThreshold, s.levelThreshold, s.onDelayMs, s.offDelayMs,
      s.minShotSeconds);
  if (!lastPumpTeach_.isEmpty()) out += "Teach Pumpe: " + lastPumpTeach_ + "\n";
  if (!lastNoiseTeach_.isEmpty()) out += "Teach Stoer: " + lastNoiseTeach_ + "\n";

  scale::diagnostics(out);
  add("Muehle: Stoergeraeusch %d, erkannt %lu x, Dosier-Tara %s, Dosis %.1f g", grinder_ + 1,
      (unsigned long)grinderStarts_, scale::doseMode() ? "aktiv" : "aus", doseG_);

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
  out.grinderNoise = grinder_;
  out.grinderOn = detector_.status().grinderOn;
  out.clipRatio = clipPeak_;

  out.profileValid = profile_.valid;
  out.profileBgDb = profile_.bgRmsDb;
  out.profilePumpDb = profile_.pumpRmsDb;
  out.profileQuality = profile_.quality;
  out.profileTemplates = profile_.templateCount;
  out.profileRecordings = profile_.recordings;
  out.profilePeriodHz = profile_.periodHz();
  out.noiseCount = noise_.count;
  out.profileRhythmTh = profile_.rhythmThreshold;
  out.profileSteadyTh = profile_.steadyThreshold;

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
  static PumpProfile ref;
  ref = profile_;
  if (ref.isDefault()) {
    ref.bgRmsDb = detector_.status().bgDb;
    ref.pumpRmsDb = ref.bgRmsDb + cfg::kDefaultSpanDb;
    ref.steadyThreshold = cfg::kDefaultSteadyDb;
  }
  timer_.reset();
  detector_.reset();
  teach_.start(millis(), mode, settings_, ref, noise_);
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

  // Kennwerte für den Diagnosebericht merken.
  char info[220];
  if (r.mode == TeachMode::Noise) {
    snprintf(info, sizeof(info), "vorher %.0f%% nachher %.0f%% (%.1f s)", r.noiseHitBefore * 100.0f,
             r.noiseHitAfter * 100.0f, r.activeSeconds);
    lastNoiseTeach_ = info;
  } else {
    snprintf(info, sizeof(info),
             "snr=%.1f cov=%.2f takt=%.0fHz per(p10)=%.2f/bg %.2f rhy(p5)=%.2f/bg %.2f "
             "sim(p5)=%.2f/bg %.2f schwankung(p95)=%.1fdB kontrast=%.1fdB muster=%u %.1fs %s",
             r.snrDb, r.coverage, r.profile.periodHz(), r.pumpPeriodicity, r.bgPeriodicity,
             r.pumpRhythm, r.bgRhythm, r.selfSimilarity, r.bgSimilarity, r.pumpSteadiness,
             r.contrastDb, r.profile.templateCount, r.activeSeconds, add ? "(ergaenzt)" : "");
    lastPumpTeach_ = info;
  }

  if (r.mode == TeachMode::Noise) {
    // Störgeräusch anhängen; ist die Liste voll, fällt das älteste heraus.
    if (noise_.count >= cfg::kMaxNoises) {
      memmove(&noise_.entries[0], &noise_.entries[1],
              sizeof(NoiseProfile::Entry) * (cfg::kMaxNoises - 1));
      --noise_.count;
    }
    noise_.entries[noise_.count++] = r.noise;
    detector_.setNoise(noise_);
    detector_.reset();
    teach_.cancel();
    msg = persistLocked() ? "Störgeräusch gespeichert (" + String(noise_.count) + " gelernt)"
                          : String("FEHLER: Störgeräusch konnte nicht gespeichert werden!");
    Serial.printf("[teach] %s\n", msg.c_str());
    if (note) *note = msg;
    return true;
  }

  if (add && profile_.valid && !profile_.isDefault()) {
    // Ergänzen: Muster anhängen; Schwellen so wählen, dass beide Aufnahmen
    // erkannt werden (jeweils die tolerantere). Die neue Aufnahme vorher auf
    // die Mikrofon-Verstärkung des bestehenden Profils umrechnen.
    static PumpProfile incoming;
    incoming = r.profile;
    const float offset = 20.0f * log10f(profile_.micGain / micGain_);
    incoming.bgRmsDb += offset;
    incoming.pumpRmsDb += offset;
    for (float& b : incoming.bgBandDb) b += offset;
    const bool wasPeriodic = profile_.periodic;
    PumpDetector::mergeProfile(profile_, incoming, msg);
    settings_.similarityThreshold = fminf(settings_.similarityThreshold, r.suggestedSimilarity);
    if (profile_.periodic && wasPeriodic) {
      settings_.periodicityThreshold = fminf(settings_.periodicityThreshold, r.suggestedPeriodicity);
    }
    if (msg.isEmpty()) msg = "Profil ergänzt";
  } else {
    // Ersetzen: Profil und Schwellen komplett aus der Aufnahme übernehmen.
    profile_ = r.profile;
    profile_.micGain = micGain_;  // gelernt mit der aktuellen Verstärkung
    settings_.similarityThreshold = r.suggestedSimilarity;
    settings_.periodicityThreshold = r.suggestedPeriodicity;
    msg = "Profil gespeichert";
  }
  if (r.suggestedOffDelayMs > settings_.offDelayMs) settings_.offDelayMs = r.suggestedOffDelayMs;

  settings::clamp(settings_);
  if (!persistLocked()) msg = "FEHLER: Profil konnte nicht gespeichert werden!";
  detector_.setProfile(profile_);
  detector_.setSettings(settings_);
  teach_.cancel();

  Serial.printf("[teach] %s: %u Muster, %u Aufnahme(n), Takt %.0f Hz, sim>=%.2f per>=%.2f\n",
                msg.c_str(), profile_.templateCount, profile_.recordings, profile_.periodHz(),
                settings_.similarityThreshold, settings_.periodicityThreshold);
  if (note) *note = msg;
  return true;
}

bool App::cmdSetGrinder(int8_t index) {
  Lock lock(mutex_);
  if (index >= (int8_t)noise_.count) return false;
  grinder_ = index < 0 ? -1 : index;
  settings::saveGrinder(activeSlot_, grinder_);
  detector_.setGrinder(grinder_);
  Serial.printf("[profil] Mühle = Störgeräusch %d\n", grinder_ + 1);
  return true;
}

void App::cmdClearNoise() {
  Lock lock(mutex_);
  noise_.count = 0;
  grinder_ = -1;
  settings::saveGrinder(activeSlot_, grinder_);
  detector_.setGrinder(grinder_);
  persistLocked();
  detector_.setNoise(noise_);
  Serial.println("[teach] Störgeräusche gelöscht");
}

bool App::cmdDeleteNoise(uint8_t index) {
  Lock lock(mutex_);
  if (index >= noise_.count) return false;
  memmove(&noise_.entries[index], &noise_.entries[index + 1],
          sizeof(NoiseProfile::Entry) * (noise_.count - index - 1));
  --noise_.count;
  detector_.setNoise(noise_);
  // Markierung "Mühle" mitverschieben
  if (grinder_ == (int8_t)index) grinder_ = -1;
  else if (grinder_ > (int8_t)index) --grinder_;
  settings::saveGrinder(activeSlot_, grinder_);
  detector_.setGrinder(grinder_);
  Serial.printf("[profil] Störgeräusch %u gelöscht\n", index + 1);
  return persistLocked();
}

bool App::cmdDeleteTemplate(uint8_t index) {
  Lock lock(mutex_);
  // Mindestens ein Muster muss bleiben, sonst wäre das Profil unbrauchbar.
  if (index >= profile_.templateCount || profile_.templateCount <= 1) return false;
  memmove(profile_.templates[index], profile_.templates[index + 1],
          sizeof(profile_.templates[0]) * (profile_.templateCount - index - 1));
  --profile_.templateCount;
  detector_.setProfile(profile_);
  Serial.printf("[profil] Klangmuster %u gelöscht\n", index + 1);
  return persistLocked();
}

void App::cmdResetTimer() {
  Lock lock(mutex_);
  timer_.reset();
  if (scale::connected() && scale::config().syncTimer) {
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
  persistLocked();
}

// ---------------------------------------------------------------------------
// Profil-Slots
// ---------------------------------------------------------------------------

namespace {
/// Arbeitspuffer für Slot-Operationen (~9 kB, nur unter Mutex benutzt).
ProfileSlot slotBuf;
}  // namespace

void App::summarize(const ProfileSlot& s, bool stored, SlotSummary& out) {
  memcpy(out.name, s.name, sizeof(out.name));
  out.stored = stored;
  out.isDefault = s.pump.isDefault();
  out.recordings = s.pump.recordings;
  out.templates = s.pump.templateCount;
  out.noiseCount = s.noise.count;
  out.periodHz = s.pump.periodHz();
  out.quality = s.pump.quality;
}

void App::loadSlotLocked(uint8_t slot) {
  activeSlot_ = slot < cfg::kProfileSlots ? slot : 0;
  settings::loadSlot(activeSlot_, slotBuf);  // leer -> Standardprofil
  profile_ = slotBuf.pump;
  noise_ = slotBuf.noise;
  settings_ = slotBuf.settings;
  memcpy(slotName_, slotBuf.name, sizeof(slotName_));
  grinder_ = settings::loadGrinder(activeSlot_);
  if (grinder_ >= (int8_t)noise_.count) grinder_ = -1;
  detector_.setProfile(profile_);
  detector_.setNoise(noise_);
  detector_.setGrinder(grinder_);
  detector_.setSettings(settings_);
  Serial.printf("[profil] Slot %u aktiv: %s%s\n", activeSlot_ + 1, slotName_,
                profile_.isDefault() ? " (Standardprofil)" : "");
}

bool App::persistLocked() {
  memcpy(slotBuf.name, slotName_, sizeof(slotBuf.name));
  slotBuf.pump = profile_;
  slotBuf.noise = noise_;
  slotBuf.settings = settings_;
  lastSaveOk_ = settings::saveSlot(activeSlot_, slotBuf);
  summarize(slotBuf, lastSaveOk_, summaries_[activeSlot_]);
  if (!lastSaveOk_) Serial.println("[profil] FEHLER: Speichern fehlgeschlagen");
  return lastSaveOk_;
}

void App::refreshSummariesLocked() {
  for (uint8_t i = 0; i < cfg::kProfileSlots; ++i) {
    const bool stored = settings::loadSlot(i, slotBuf);
    summarize(slotBuf, stored, summaries_[i]);
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
  teach_.cancel();
  timer_.reset();
  loadSlotLocked(slot);
  settings::saveActiveSlot(activeSlot_);
}

void App::cmdNextSlot() {
  uint8_t next;
  {
    Lock lock(mutex_);
    next = (activeSlot_ + 1) % cfg::kProfileSlots;
  }
  cmdSelectSlot(next);
}

bool App::cmdRenameSlot(uint8_t slot, const String& name) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  String n = name;
  n.trim();
  if (n.isEmpty()) return false;
  if (slot == activeSlot_) {
    strlcpy(slotName_, n.c_str(), sizeof(slotName_));
    return persistLocked();
  }
  settings::loadSlot(slot, slotBuf);
  strlcpy(slotBuf.name, n.c_str(), sizeof(slotBuf.name));
  const bool ok = settings::saveSlot(slot, slotBuf);
  summarize(slotBuf, ok, summaries_[slot]);
  return ok;
}

bool App::cmdDeleteSlot(uint8_t slot) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  const bool ok = settings::deleteSlot(slot);
  settings::saveGrinder(slot, -1);
  settings::defaultSlot(slot, slotBuf);
  summarize(slotBuf, false, summaries_[slot]);
  if (slot == activeSlot_) {
    teach_.cancel();
    loadSlotLocked(slot);
  }
  return ok;
}

bool App::exportSlot(uint8_t slot, String& out, String& name) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  if (slot == activeSlot_) {
    memcpy(slotBuf.name, slotName_, sizeof(slotBuf.name));
    slotBuf.pump = profile_;
    slotBuf.noise = noise_;
    slotBuf.settings = settings_;
  } else {
    settings::loadSlot(slot, slotBuf);
  }
  name = slotBuf.name;
  return settings::encodeSlot(slotBuf, out);
}

bool App::cmdImportSlot(uint8_t slot, const String& data) {
  if (slot >= cfg::kProfileSlots) return false;
  Lock lock(mutex_);
  if (!settings::decodeSlot(data, slotBuf)) return false;
  const bool ok = settings::saveSlot(slot, slotBuf);
  if (ok) settings::saveGrinder(slot, -1);  // Markierung "Mühle" gehört nicht zum Export
  summarize(slotBuf, ok, summaries_[slot]);
  if (ok && slot == activeSlot_) {
    teach_.cancel();
    loadSlotLocked(slot);
  }
  return ok;
}

void App::cmdSetMicGain(uint8_t gain) {
  gain = constrain(gain, cfg::kMicGainMin, cfg::kMicGainMax);
  settings::saveMicGain(gain);
  requestedGain_ = gain;  // Audio-Task übernimmt beim nächsten Durchlauf
}
