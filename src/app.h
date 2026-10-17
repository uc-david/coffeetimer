/**
 * @file app.h
 * @brief Zentrale Anwendungslogik – verbindet Audio, Erkennung, Timer und Teach-Modus.
 *
 * Threading-Modell:
 *  - Ein eigener FreeRTOS-Task ("audio") liest kontinuierlich das Mikrofon,
 *    berechnet die Merkmale und füttert Detektor, Timer und Teach-Modus.
 *  - Display (loop) und Webserver greifen ausschließlich über snapshot() und
 *    die cmd*-Methoden zu.
 *  - Ein Mutex schützt alle gemeinsam genutzten Daten. Die rechenintensive FFT
 *    läuft außerhalb des Mutex, damit UI und Web nie lange blockiert werden.
 */
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <freertos/semphr.h>

#include "audio_features.h"
#include "pump_detector.h"
#include "settings.h"
#include "shot_timer.h"
#include "teach_mode.h"

/**
 * Protokoll einer erkannten Pumpenphase (Diagnose): Mittel- und Minimalwerte
 * der Merkmale während die Pumpe als EIN galt. Damit lässt sich z. B.
 * nachvollziehen, warum eine Mühle als Pumpe erkannt wurde.
 */
struct DetectionEvent {
  uint32_t durationMs = 0;
  time_t endEpoch = 0;     ///< Uhrzeit des Endes (0 = unbekannt)
  uint16_t frames = 0;     ///< Anzahl ausgewerteter Frames
  FrameScores mean;        ///< Mittelwerte
  FrameScores min;         ///< Minimalwerte (schwächster Moment)
};

/// Kurzinfo zu einem Profil-Slot (für Listen auf Webseite und Display).
struct SlotSummary {
  char name[ProfileSlot::kNameLen] = {};
  bool stored = false;        ///< Datei vorhanden (sonst Standardprofil)
  bool isDefault = true;      ///< Standardprofil ohne Teach
  uint8_t recordings = 0;
  uint8_t templates = 0;
  uint8_t noiseCount = 0;
  uint8_t grinderCount = 0;
  float periodHz = 0.0f;
  float quality = 0.0f;
};

/// Konsistente Momentaufnahme des gesamten Zustands für Anzeige und Webseite.
struct StatusSnapshot {
  // Profil
  uint8_t activeSlot = 0;
  char profileName[ProfileSlot::kNameLen] = {};
  bool profileDefault = true;     ///< Standardprofil ohne Teach
  bool lastSaveOk = true;         ///< letzter Speichervorgang erfolgreich
  bool profileValid = false;
  float profileBgDb = 0.0f;
  float profilePumpDb = 0.0f;
  float profileQuality = 0.0f;
  uint8_t profileTemplates = 0;
  uint8_t profileRecordings = 0;
  float profilePeriodHz = 0.0f;   ///< 0 = kein Takt
  uint8_t noiseCount = 0;         ///< Anzahl gelernter Störgeräusche
  float profileRhythmTh = 0.0f;   ///< Schwelle der Rhythmus-Form (aus dem Teach)
  float profileSteadyTh = 0.0f;   ///< max. Pegelschwankung in dB (aus dem Teach)

  // Erkennung
  DetectorStatus detector;
  DetectorSettings settings;

  // Timer
  bool timerRunning = false;
  uint32_t timerMs = 0;
  size_t historyCount = 0;
  ShotRecord history[cfg::kHistorySize];

  // Teach-Modus
  TeachPhase teachPhase = TeachPhase::Idle;
  TeachMode teachMode = TeachMode::Pump;
  float teachProgress = 0.0f;
  uint32_t teachRecordedMs = 0;   ///< Dauer der laufenden Pumpenaufnahme
  const char* teachInstruction = "";
  float teachLiveDb = -90.0f;
  float teachBgDb = -90.0f;
  TeachResult teachResult;  ///< nur gültig in Phase Done

  // Waage (optional)
  bool scaleEnabled = false;
  bool scaleConnected = false;
  const char* scaleState = "";
  float scaleWeight = NAN;        ///< g, NAN = keine Daten
  float scaleFlow = 0.0f;         ///< g/s
  int scaleBattery = -1;
  bool scaleDoseMode = false;     ///< Dosier-Tara aktiv (Gewicht = Kaffeemehl)
  float doseG = NAN;              ///< zuletzt gewogene Dosis (für den nächsten Bezug)

  // Mühle
  uint8_t grinderCount = 0;       ///< gelernte Mühlen
  char grinderNames[cfg::kMaxGrinders][GrinderProfile::kNameLen] = {};
  bool grinderOn = false;         ///< eine Mühle läuft gerade
  int8_t grinderActive = -1;      ///< welche (-1 = keine)

  // Vibration (Beschleunigungssensor)
  uint8_t vibMode = 0;            ///< vibration::Mode
  bool vibAvailable = false;
  bool vibActive = false;
  float vibLevelDb = -120.0f;
  float vibBaselineDb = -120.0f;
  float vibThresholdDb = 8.0f;
  float vibSteadiness = 99.0f;

  // Mikrofon
  uint8_t micGain = cfg::kMicGainDefault;
  float clipRatio = 0.0f;    ///< Übersteuerung (Spitzenwert, klingt langsam ab)

  // Diagnose
  uint32_t audioFrames = 0;  ///< Anzahl verarbeiteter Frames (Mikrofon lebt?)
};

class App {
 public:
  /// Lädt Einstellungen/Profil, startet Mikrofon und Audio-Task.
  void begin();

  /// Zyklisch aus loop() aufrufen: speichert geänderte Einstellungen verzögert.
  void loop();
  /// Noch nicht gespeicherte Einstellungen sofort speichern (z. B. beim Verlassen des Menüs).
  void flushSettings();

  /// Liefert eine threadsichere Kopie des aktuellen Zustands.
  void snapshot(StatusSnapshot& out);

  /// Aktuelle Erkennungseinstellungen (threadsicher).
  DetectorSettings settings();

  // --- Befehle (von Display und Webseite) ----------------------------------
  /// Startet den Teach-Modus für die Pumpe oder ein Störgeräusch.
  void cmdStartTeach(TeachMode mode = TeachMode::Pump);
  void cmdCancelTeach();
  /// Beendet die Pumpenaufnahme vorzeitig ("Fertig").
  void cmdFinishTeach();
  /**
   * Übernimmt das Teach-Ergebnis (Pumpe: Profil, Störgeräusch: Liste ergänzen).
   * @param add   Pumpe: false = Profil ersetzen, true = bestehendes Profil ergänzen;
   *              Mühle: false = neue Mühle, true = der ähnlichsten Mühle hinzufügen
   * @param note  optional: erhält einen Hinweistext
   * @return false bei Fehler-Ergebnis
   */
  bool cmdSaveTeach(bool add, String* note = nullptr);
  /// Erstellt einen Diagnosebericht (Klartext) zum Kopieren/Weitergeben.
  void diagnosticsText(String& out);

  /// Löscht alle gelernten Störgeräusche.
  void cmdClearNoise();
  /// Löscht ein einzelnes Störgeräusch (Index 0..count-1).
  bool cmdDeleteNoise(uint8_t index);
  /// Löscht bzw. benennt eine gelernte Mühle (Index 0..count-1).
  bool cmdDeleteGrinder(uint8_t index);
  bool cmdRenameGrinder(uint8_t index, const String& name);
  /// Löscht ein einzelnes Klangmuster der Pumpe (das letzte bleibt immer erhalten).
  bool cmdDeleteTemplate(uint8_t index);

  // --- Profil-Slots ----------------------------------------------------------
  /// Kurzinfo aller Slots und aktiver Slot.
  void slots(SlotSummary (&out)[cfg::kProfileSlots], uint8_t& active);
  /// Wechselt den aktiven Slot (lädt Profil, Störgeräusche, Schwellen).
  void cmdSelectSlot(uint8_t slot);
  bool cmdRenameSlot(uint8_t slot, const String& name);
  /// Löscht einen Slot (danach Standardprofil).
  bool cmdDeleteSlot(uint8_t slot);
  /// Export als Text (Base64) bzw. Import aus Text in einen Slot.
  bool exportSlot(uint8_t slot, String& out, String& name);
  bool cmdImportSlot(uint8_t slot, const String& data);

  /// Setzt die Mikrofon-Verstärkung (wird im Audio-Task übernommen).
  void cmdSetMicGain(uint8_t gain);
  /// Timer auf 0; mit verbundener Waage zusätzlich Tara und deren Timer auf 0.
  void cmdResetTimer();
  void cmdClearHistory();
  /// Übernimmt Einstellungen sofort; gespeichert wird erst, wenn sie
  /// cfg::kSettingsSaveDelayMs lang unverändert bleiben (schont den Flash).
  void cmdApplySettings(DetectorSettings s);

 private:
  /// RAII-Sperre für den Mutex.
  class Lock {
   public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
   private:
    SemaphoreHandle_t m_;
  };

  static void audioTaskEntry(void* arg);
  void audioTask();
  void handleFrame(AudioFrame& f);  ///< Aufruf nur unter Mutex
  void applyMicGain(uint8_t gain);  ///< nur aus dem Audio-Task

  // Slots (Aufruf nur unter Mutex)
  void loadSlotLocked(uint8_t slot);
  bool persistLocked();             ///< speichert den aktiven Slot
  void refreshSummariesLocked();
  void summarize(const ProfileSlot& s, bool stored, SlotSummary& out);

  SemaphoreHandle_t mutex_ = nullptr;
  FeatureExtractor extractor_;
  PumpDetector detector_;
  ShotTimer timer_;
  TeachController teach_;
  PumpProfile profile_;
  NoiseProfile noise_;
  DetectorSettings settings_;
  uint32_t audioFrames_ = 0;

  // Slots
  uint8_t activeSlot_ = 0;
  char slotName_[ProfileSlot::kNameLen] = {};
  SlotSummary summaries_[cfg::kProfileSlots];
  bool lastSaveOk_ = true;
  bool settingsDirty_ = false;      ///< Einstellungen geändert, noch nicht gespeichert
  uint32_t settingsChangedMs_ = 0;  ///< Zeitpunkt der letzten Änderung

  // Mikrofon
  std::atomic<uint8_t> requestedGain_{cfg::kMicGainDefault};
  uint8_t micGain_ = cfg::kMicGainDefault;  ///< aktuell aktive Verstärkung
  float clipPeak_ = 0.0f;

  // Waage
  uint32_t weightUntilMs_ = 0;  ///< bis dahin Endgewicht nachführen (0 = aus)
  uint32_t weightTares_ = 0;    ///< Tara-Zähler beim Pumpenstopp (Tara beendet das Nachführen)
  void handleGrinder(const AudioFrame& f);  ///< Dosier-Tara und Dosis (unter Mutex)
  GrinderProfile grinders_;     ///< Mühlen des aktiven Slots (Detektor hält einen Zeiger)
  bool grinderWasOn_ = false;
  float doseG_ = NAN;           ///< zuletzt gewogene Dosis
  uint32_t doseAtMs_ = 0;       ///< Zeitpunkt der Dosis
  uint32_t doseTareMs_ = 0;     ///< Zeitpunkt der letzten Dosier-Tara
  uint32_t grinderStarts_ = 0;  ///< Diagnose: erkannte Mühlenläufe

  // --- Diagnose ---
  static constexpr size_t kMaxEvents = 10;
  void accumulateEvent(const FrameScores& s);
  DetectionEvent events_[kMaxEvents];  ///< Ringpuffer, Index 0 = ältester
  size_t eventCount_ = 0;
  DetectionEvent current_;             ///< laufende Pumpenphase
  uint32_t rejectedFrames_ = 0;        ///< als Störgeräusch verworfene Frames
  String lastPumpTeach_;               ///< Kennwerte des zuletzt gespeicherten Teach
  String lastNoiseTeach_;
};

/// Einzige Instanz der Anwendung.
extern App app;
