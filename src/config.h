/**
 * @file config.h
 * @brief Zentrale Konstanten des Coffee Timers.
 *
 * Alle festen Parameter (Audio, Teach-Modus, Netzwerk) sind hier gebündelt,
 * damit sie an einer Stelle angepasst werden können. Vom Benutzer zur Laufzeit
 * veränderbare Werte liegen dagegen in settings.h (DetectorSettings).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cfg {

/// Firmware-Version (wird auf Webseite und Info-Screen angezeigt).
constexpr const char* kFirmwareVersion = "1.5 (" __DATE__ ")";

// ---------------------------------------------------------------------------
// Audio-Erfassung und Merkmale
// ---------------------------------------------------------------------------

/// Abtastrate des Mikrofons in Hz. 16 kHz reichen für das Pumpengeräusch
/// (Vibrationspumpen takten mit 50/100 Hz plus Obertönen bis einige kHz).
constexpr uint32_t kSampleRate = 16000;

/// Samples pro Aufnahme-Block (= Schrittweite der Analyse).
/// 512 Samples @ 16 kHz = 32 ms zeitliche Auflösung.
constexpr size_t kFrameSize = 512;

/// Länge des Analysefensters (zwei Blöcke, 50 % Überlappung) = 64 ms.
/// Umfasst gut drei Takte einer 50-Hz-Vibrationspumpe, dadurch mittelt sich
/// deren Pulsieren heraus.
constexpr size_t kWindowSize = 1024;

/// FFT-Länge für die Autokorrelation (Fenster mit Nullen aufgefüllt, damit
/// die Korrelation nicht zyklisch "herumläuft").
constexpr size_t kAcfFftSize = 2048;

/// Bereich der Autokorrelation, in dem nach dem Pumpentakt gesucht wird.
/// 64 Samples = 250 Hz ... 512 Samples = 31 Hz (deckt 50/60/100/120 Hz ab).
/// Kürzere Verschiebungen würden eher Gehäuse-Resonanzen finden, die sich
/// unter Last verschieben – der Pumpentakt selbst bleibt dagegen stabil.
constexpr size_t kMinLag = 64;
constexpr size_t kMaxLag = 512;
constexpr size_t kAcfLen = kMaxLag - kMinLag + 1;

/// Toleranz der Taktlänge bei der Erkennung (Drehzahl einer Rotationspumpe
/// sinkt unter Last leicht).
constexpr float kLagTolerance = 0.03f;

/// Glättungsfaktor (exponentieller Mittelwert) für Pegel und Bänder.
/// 1.0 = keine Glättung; 0.5 ≈ Mittel über ~2 Schritte.
constexpr float kSmoothing = 0.5f;

/// Gleichmäßigkeit: Pegelschwankung wird über so viele Frames (≈0,5 s) der
/// aktuellen lauten Phase gemessen ...
constexpr size_t kSteadyFrames = 16;
/// ... und erst ab so vielen Frames (≈0,25 s) bewertet.
constexpr size_t kSteadyMinFrames = 8;

/// Anzahl der logarithmisch verteilten Frequenzbänder des "Fingerabdrucks".
constexpr size_t kNumBands = 24;

/// Untere / obere Grenze des ausgewerteten Frequenzbereichs in Hz.
constexpr float kBandMinHz = 80.0f;
constexpr float kBandMaxHz = 7000.0f;

/// Ab diesem Betrag gilt ein Sample als übersteuert (int16-Vollaussteuerung = 32767).
constexpr int16_t kClipLevel = 32000;

/// Anzahl Frames, die nach dem Start verworfen werden (Einschwingen des Codecs).
constexpr uint32_t kWarmupFrames = 30;

// ---------------------------------------------------------------------------
// Profil
// ---------------------------------------------------------------------------

/// Maximale Anzahl Klangmuster im Profil (z. B. Leerbezug + echter Bezug).
constexpr size_t kMaxTemplates = 6;

/// Klangmuster je Teach-Aufnahme (z. B. Anlauf / Druckaufbau / voller Druck).
constexpr size_t kTemplatesPerTeach = 3;

/// Muster, die sich ähnlicher sind als dieser Wert, werden zusammengefasst.
constexpr float kTemplateMergeSimilarity = 0.97f;

/// Anzahl der Profil-Speicherplätze (z. B. verschiedene Maschinen / Orte).
constexpr size_t kProfileSlots = 5;

/// Mikrofon-Verstärkung (digitaler Faktor in M5Unified): Standard und Grenzen.
constexpr uint8_t kMicGainDefault = 16;
constexpr uint8_t kMicGainMin = 1;
constexpr uint8_t kMicGainMax = 64;

/// Standardprofil (ohne Teach): Erkannt wird jedes gleichmäßige Geräusch
/// deutlich über dem automatisch gemessenen Raumpegel – ein Teach macht die
/// Erkennung spezifisch. Startwerte (Verstärkung 16), danach automatisch:
constexpr float kDefaultBgDb = -67.0f;
constexpr float kDefaultPumpDb = -46.0f;
/// Abstand Pumpe – Hintergrund, auf den die Pegel-Schwelle bezogen wird
/// (Pegel-Schwelle 0,5 => 10 dB über dem Raumpegel).
constexpr float kDefaultSpanDb = 20.0f;
/// Strengere Grenzen des Standardprofils: max. Schwankung, Mindestdauer.
constexpr float kDefaultSteadyDb = 1.5f;
constexpr uint16_t kDefaultMinOnMs = 1000;
/// Automatischer Hintergrund: Anstieg (dB je Frame ≈ 0,5 dB/s) und
/// Anpassungsrate beim Absinken.
constexpr float kAutoBgRiseDb = 0.016f;
constexpr float kAutoBgFall = 0.2f;

/// Sicherung gegen hängende Timer: längste plausible Pumpenlaufzeit.
constexpr uint32_t kMaxPumpMsDefault = 60000;  ///< Standardprofil
constexpr uint32_t kMaxPumpMsLearned = 120000; ///< gelerntes Profil

/// Maximale Anzahl gelernter Störgeräusche (z. B. Mühle, Milchaufschäumer).
constexpr size_t kMaxNoises = 3;

/// Klangmuster je Störgeräusch.
constexpr size_t kNoiseTemplates = 2;

// ---------------------------------------------------------------------------
// Teach-Modus
// ---------------------------------------------------------------------------

/// Dauer der Hintergrund-Aufnahme (Pumpe AUS).
constexpr uint32_t kTeachBackgroundMs = 3000;

/// Am Anfang der Hintergrundaufnahme ignorierte Zeit (Berührungsgeräusch des Tasters).
constexpr uint32_t kTeachBackgroundSkipMs = 400;

/// Maximale Wartezeit, bis der Benutzer die Pumpe eingeschaltet hat.
constexpr uint32_t kTeachWaitPumpTimeoutMs = 20000;

/// Pegelanstieg gegenüber Hintergrund, ab dem "Pumpe läuft" angenommen wird.
constexpr float kTeachPumpTriggerDb = 6.0f;

/// So viele Frames in Folge muss der Pegelanstieg anhalten (Störgeräusche filtern).
constexpr uint32_t kTeachPumpTriggerFrames = 5;

/// Nach Erkennen der Pumpe ignorierte Zeit (Einschaltknacken).
constexpr uint32_t kTeachPumpSkipMs = 200;

/// Frames unter Hintergrund + diesem Wert gelten als "Pumpe aus / Pause".
constexpr float kTeachActiveDb = 3.0f;

/// Ist die Pumpe so lange still, endet die Aufnahme automatisch.
/// Lang genug, um eine Pre-Infusion-Pause zu überbrücken.
constexpr uint32_t kTeachStopSilenceMs = 4000;

/// Maximale Aufnahmedauer der Pumpe (ein kompletter Bezug passt hinein).
constexpr uint32_t kTeachPumpMaxMs = 60000;

/// Mindestdauer, die die Pumpe während der Aufnahme hörbar sein muss.
constexpr uint32_t kTeachPumpMinActiveMs = 3000;

/// Störgeräusch-Teach: Ende nach so langer Stille, Maximal- und Mindestdauer.
constexpr uint32_t kTeachNoiseStopSilenceMs = 1500;
constexpr uint32_t kTeachNoiseMaxMs = 30000;
constexpr uint32_t kTeachNoiseMinActiveMs = 2000;

/// Puffergrößen (Frames) für die Aufnahmen; liegen im PSRAM.
constexpr size_t kTeachMaxBgFrames = kTeachBackgroundMs / 32 + 8;
constexpr size_t kTeachMaxPumpFrames = kTeachPumpMaxMs / 32 + 8;

// Qualitätsgrenzen für die Bewertung der Teach-Aufnahme --------------------
constexpr float kQualMinSnrDb       = 6.0f;   ///< leiser Teil der Pumpe vs. Hintergrund: Fehler
constexpr float kQualGoodSnrDb      = 10.0f;  ///< darunter: Warnung
constexpr float kQualMaxClipRatio   = 0.005f; ///< max. Anteil übersteuerter Samples
constexpr float kQualBgStdWarnDb    = 4.0f;   ///< Hintergrund zu unruhig
constexpr float kQualCoverageErr    = 0.70f;  ///< erkannter Anteil der Aufnahme: Fehler
constexpr float kQualCoverageWarn   = 0.90f;  ///< erkannter Anteil der Aufnahme: Warnung
constexpr float kQualMaxFalseRatio  = 0.03f;  ///< Hintergrund-Frames, die auslösen würden
constexpr float kQualDistinctBandDb = 6.0f;   ///< "markantes" Band: so viel über Hintergrund
constexpr float kQualMinPeriodicity = 0.25f;  ///< Takt gilt als ausgeprägt ab ...
constexpr float kQualMinPeriodicGap = 0.10f;  ///< ... und Abstand zum Hintergrund
constexpr float kQualMaxNoiseHit    = 0.05f;  ///< Störgeräusch darf höchstens so oft auslösen

// ---------------------------------------------------------------------------
// Timer / Historie
// ---------------------------------------------------------------------------

/// Nach dem Pumpenstopp wird das Gewicht noch so lange nachgeführt
/// (Nachtropfen), bevor es in der Historie feststeht.
constexpr uint32_t kWeightSettleMs = 4000;

/// Anzahl der gespeicherten Bezüge in der Historie.
constexpr size_t kHistorySize = 10;

// ---------------------------------------------------------------------------
// Netzwerk
// ---------------------------------------------------------------------------

/// Präfix des Access-Points, falls kein WLAN konfiguriert / erreichbar ist.
constexpr const char* kApSsidPrefix = "CoffeeTimer-";

/// mDNS-Name -> http://coffeetimer.local
constexpr const char* kHostname = "coffeetimer";

/// Zeit, die eine Verbindung mit dem gespeicherten WLAN versucht wird.
constexpr uint32_t kWifiConnectTimeoutMs = 15000;

/// Zeitzone für die Uhrzeit in der Historie (Mitteleuropa inkl. Sommerzeit).
constexpr const char* kTimezone = "CET-1CEST,M3.5.0,M10.5.0/3";

// ---------------------------------------------------------------------------
// Anzeige
// ---------------------------------------------------------------------------

/// Bildwiederholintervall des Displays.
constexpr uint32_t kDisplayIntervalMs = 50;

}  // namespace cfg
