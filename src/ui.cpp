// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file ui.cpp
 * @brief Implementierung der Display-Oberfläche.
 */
#include "ui.h"

#include <M5Unified.h>

#include <algorithm>

#include "app.h"
#include "battery.h"
#include "datetime.h"
#include "net_manager.h"
#include "scale.h"
#include "vibration.h"

namespace {

// ---------------------------------------------------------------------------
// Farben (RGB565) und Layout
// ---------------------------------------------------------------------------
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t kColBg     = rgb(27, 20, 17);
constexpr uint16_t kColCard   = rgb(42, 32, 27);
constexpr uint16_t kColLine   = rgb(61, 47, 39);
constexpr uint16_t kColText   = rgb(243, 235, 228);
constexpr uint16_t kColMuted  = rgb(179, 161, 151);
constexpr uint16_t kColAccent = rgb(224, 164, 88);
constexpr uint16_t kColRun    = rgb(255, 179, 71);
constexpr uint16_t kColOk     = rgb(95, 191, 119);
constexpr uint16_t kColWarn   = rgb(230, 180, 80);
constexpr uint16_t kColErr    = rgb(229, 96, 90);
constexpr uint16_t kColDark   = rgb(42, 26, 10);

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kBtnY = 204;
constexpr int kBtnH = 34;

/// UTF-8-fähige Schriften (Umlaute) und 7-Segment-Schrift für den Timer.
const lgfx::IFont* kFontSmall = &fonts::efontJA_12;
const lgfx::IFont* kFontText  = &fonts::efontJA_16;
const lgfx::IFont* kFontBold  = &fonts::efontJA_16_b;
const lgfx::IFont* kFontTimer = &fonts::Font7;

enum class Screen { Main, TeachSelect, Teach, Info, Settings, ResetMenu };

/// Touch-Schaltfläche.
struct Button {
  int16_t x, y, w, h;
  const char* label;
  uint16_t color;
  uint16_t textColor;
  bool visible;

  bool hit(int px, int py) const {
    return visible && px >= x && px < x + w && py >= y && py < y + h;
  }
};

// Buttons je Screen (Positionen fest, Sichtbarkeit dynamisch)
Button btnTeach   {  4, kBtnY, 100, kBtnH, "TEACH",       kColAccent, kColDark, true};
Button btnReset   {110, kBtnY, 100, kBtnH, "RESET",       kColLine,   kColText, true};
Button btnInfo    {216, kBtnY, 100, kBtnH, "INFO",        kColLine,   kColText, true};
// RESET-Menü (nur mit verbundener Waage): alles oder einzeln auf 0
Button btnResetAll  {  8,  58, 304, 56, "ALLES AUF 0", kColAccent, kColDark, true};
Button btnResetTimer{  8, 122,  98, 56, "BEZUGSZEIT",  kColLine,   kColText, true};
Button btnResetTare {111, 122,  98, 56, "GEWICHT",     kColLine,   kColText, true};
Button btnResetScale{214, 122,  98, 56, "WAAGEN-ZEIT", kColLine,   kColText, true};

// Teach-Auswahl: Kacheln im Inhaltsbereich (Pumpe breit, darunter Mühle / Störgeräusch)
Button btnLearnPump   {  8,  58, 304, 56, "PUMPE",    kColAccent, kColDark, true};
Button btnLearnGrinder{  8, 122, 148, 56, "MÜHLE",    kColLine,   kColText, true};
Button btnLearnNoise  {164, 122, 148, 56, "STÖRGER.", kColLine,   kColText, true};

Button btnFinish  {  4, kBtnY, 100, kBtnH, "FERTIG",      kColOk,     kColDark, false};
Button btnSave    {  4, kBtnY, 100, kBtnH, "SPEICHERN",   kColOk,     kColDark, false};
Button btnAdd     {110, kBtnY, 100, kBtnH, "ERGÄNZEN",    kColAccent, kColDark, false};
Button btnRetry   {110, kBtnY, 100, kBtnH, "WIEDERHOLEN", kColLine,   kColText, false};
Button btnCancel  {216, kBtnY, 100, kBtnH, "ABBRECHEN",   kColErr,    kColText, true};

// Info-Screen: vier schmalere Buttons
Button btnProfile {  4, kBtnY,  75, kBtnH, "EINST.",      kColAccent, kColDark, true};
Button btnSensDown{ 83, kBtnY,  75, kBtnH, "EMPF -",      kColLine,   kColText, true};
Button btnSensUp  {162, kBtnY,  75, kBtnH, "EMPF +",      kColLine,   kColText, true};
Button btnInfoBack{241, kBtnY,  75, kBtnH, "ZURÜCK",      kColLine,   kColText, true};
Button btnBack    {216, kBtnY, 100, kBtnH, "ZURÜCK",      kColAccent, kColDark, true};

M5Canvas canvas(&M5.Display);
Screen screen = Screen::Main;

/// Große Anzeige im Hauptbildschirm: automatisch (beim Mahlen Gewicht, sonst
/// Timer) oder per Antippen fest auf Timer bzw. Gewicht umgeschaltet.
enum class BigView { Auto, Timer, Weight };
BigView bigView = BigView::Auto;
bool bigShowsWeight = false;   ///< was gerade groß angezeigt wird (für das Antippen)
bool lastDoseMode = false;     ///< Zustandswechsel setzen die Umschaltung zurück
bool lastTimerRunning = false;
/// Bereich der großen Zahl (zwischen Statuszeile und Gewichtszeile).
constexpr int kBigTop = 56;
constexpr int kBigBottom = 140;
StatusSnapshot snap;  // groß -> statisch statt auf dem Stack
uint32_t lastDrawMs = 0;

// ---------------------------------------------------------------------------
// Zeichen-Hilfen
// ---------------------------------------------------------------------------

void drawButton(const Button& b) {
  if (!b.visible) return;
  canvas.fillRoundRect(b.x, b.y, b.w, b.h, 8, b.color);
  canvas.setFont(kFontBold);
  canvas.setTextColor(b.textColor);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2 + 1);
}

/**
 * Zeichnet einen Balken mit optionaler Schwellenmarke.
 * @param value      Füllstand 0..1
 * @param threshold  Position der Marke 0..1, <0 = keine Marke
 */
void drawBar(int x, int y, int w, int h, float value, float threshold, uint16_t fill) {
  canvas.fillRoundRect(x, y, w, h, h / 2, kColLine);
  const int fw = static_cast<int>(constrain(value, 0.0f, 1.0f) * w);
  if (fw > 0) canvas.fillRoundRect(x, y, fw, h, h / 2, fill);
  if (threshold >= 0.0f) {
    const int tx = x + static_cast<int>(constrain(threshold, 0.0f, 1.0f) * (w - 2));
    canvas.fillRect(tx, y - 3, 2, h + 6, kColText);
  }
}

/**
 * Gibt Text mit Wortumbruch aus.
 * @return y-Position unterhalb der letzten Zeile
 */
int drawWrapped(const char* text, int x, int y, int w, int lineH, int maxY) {
  canvas.setTextDatum(textdatum_t::top_left);
  String line, word;
  const char* p = text;
  auto flush = [&]() {
    if (y + lineH <= maxY) canvas.drawString(line, x, y);
    y += lineH;
    line = "";
  };
  while (true) {
    const char c = *p;
    if (c == ' ' || c == '\0') {
      if (!word.isEmpty()) {
        const String candidate = line.isEmpty() ? word : line + " " + word;
        if (canvas.textWidth(candidate) > w && !line.isEmpty()) {
          flush();
          line = word;
        } else {
          line = candidate;
        }
        word = "";
      }
      if (c == '\0') break;
    } else {
      word += c;
    }
    ++p;
  }
  if (!line.isEmpty()) flush();
  return y;
}

/**
 * Akkusymbol mit Prozentanzeige, rechtsbündig an Position right.
 * Farbe: grün > 50 %, gelb > 20 %, sonst rot; beim Laden mit Blitz.
 * @return linke x-Kante der Anzeige (für weitere Elemente links davon)
 */
int drawBattery(int right, int cy) {
  const int lvl = battery::level();
  const bool chg = battery::charging();

  // Prozenttext
  char txt[8];
  if (lvl >= 0) {
    snprintf(txt, sizeof(txt), "%d%%", lvl);
  } else {
    snprintf(txt, sizeof(txt), "--%%");
  }
  canvas.setFont(kFontSmall);
  canvas.setTextColor(kColText);
  canvas.setTextDatum(textdatum_t::middle_right);
  canvas.drawString(txt, right, cy);
  const int textLeft = right - canvas.textWidth(txt);

  // Symbol: Gehäuse 22x11 + Kontakt-Nase
  constexpr int bw = 22, bh = 11;
  const int bx = textLeft - 4 - bw - 2;
  const int by = cy - bh / 2;
  const uint16_t col = lvl < 0 ? kColMuted : (lvl > 50 ? kColOk : (lvl > 20 ? kColWarn : kColErr));
  canvas.drawRect(bx, by, bw, bh, kColText);
  canvas.fillRect(bx + bw, cy - 2, 2, 5, kColText);
  if (lvl > 0) {
    const int fill = (bw - 4) * lvl / 100;
    canvas.fillRect(bx + 2, by + 2, fill < 1 ? 1 : fill, bh - 4, col);
  }
  if (chg) {
    // Blitz als Ladesymbol über dem Gehäuse
    const int mx = bx + bw / 2;
    canvas.fillTriangle(mx + 1, by - 1, mx - 4, cy + 1, mx, cy + 1, kColAccent);
    canvas.fillTriangle(mx - 1, by + bh + 1, mx + 4, cy - 1, mx, cy - 1, kColAccent);
  }
  return bx;
}

/// Kopfzeile mit Datum/Uhrzeit (sonst Titel), Netzwerkstatus und Akkuanzeige.
void drawHeader() {
  canvas.fillRect(0, 0, kW, 26, kColCard);
  char clock[24];
  datetime::format(clock, sizeof(clock));
  canvas.setFont(kFontBold);
  canvas.setTextColor(kColAccent);
  canvas.setTextDatum(textdatum_t::middle_left);
  canvas.drawString(clock[0] ? clock : "Coffee Timer", 8, 13);

  String netTxt;
  uint16_t col = kColMuted;
  if (net::connected()) {
    netTxt = net::ip();
    col = kColOk;
  } else if (net::apActive()) {
    netTxt = "AP " + net::ip();
    col = kColWarn;
  } else {
    netTxt = "WLAN …";
  }
  // Akku ganz rechts, Netzwerk links daneben
  const int batteryLeft = drawBattery(kW - 6, 13);
  canvas.setFont(kFontSmall);
  canvas.setTextColor(col);
  canvas.setTextDatum(textdatum_t::middle_right);
  canvas.drawString(netTxt, batteryLeft - 8, 13);
}

/// Sekunden mit einer Nachkommastelle, z. B. "27.4".
String formatSeconds(uint32_t ms) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
  return buf;
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

void drawMain() {
  // --- Statuszeile -----------------------------------------------------------
  const char* statusTxt;
  uint16_t statusCol;
  if (!snap.lastSaveOk) {
    statusTxt = "Speichern fehlgeschlagen!";
    statusCol = kColErr;
  } else if (snap.detector.pumpOn) {
    statusTxt = "Pumpe läuft";
    statusCol = kColRun;
  } else if (snap.grinderOn) {
    static char grindTxt[GrinderProfile::kNameLen + 8];
    const bool named = snap.grinderActive >= 0 && snap.grinderActive < snap.grinderCount;
    snprintf(grindTxt, sizeof(grindTxt), "%s läuft", named ? snap.grinderNames[snap.grinderActive] : "Mühle");
    statusTxt = grindTxt;
    statusCol = kColAccent;
  } else if (snap.detector.rejected) {
    statusTxt = "Störgeräusch – ignoriert";
    statusCol = kColMuted;
  } else {
    statusTxt = "Bereit";
    statusCol = kColOk;
  }
  canvas.fillCircle(14, 42, 5, statusCol);
  canvas.setFont(kFontText);
  canvas.setTextColor(statusCol);
  canvas.setTextDatum(textdatum_t::middle_left);
  canvas.drawString(statusTxt, 26, 42);

  // Aktives Profil rechts in der Statuszeile (gekürzt, falls zu lang)
  {
    const int left = 26 + canvas.textWidth(statusTxt) + 12;
    canvas.setFont(kFontSmall);
    String prof = "P" + String(snap.activeSlot + 1) + " " + snap.profileName;
    if (snap.profileDefault) prof += " (Std.)";
    while (prof.length() > 3 && canvas.textWidth(prof) > kW - 8 - left) {
      prof.remove(prof.length() - 1);
    }
    canvas.setTextColor(kColMuted);
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.drawString(prof, kW - 8, 42);
  }

  // --- Große Anzeige: Timer oder Gewicht ----------------------------------------
  // Automatisch: beim Mahlen (Mühlen-Tara bis zum Pumpenstart) der gemahlene Kaffee,
  // sonst der Timer. Antippen schaltet um; beginnt das Mahlen oder ein Bezug
  // bzw. endet einer, gilt wieder die Automatik.
  if (snap.scaleDoseMode != lastDoseMode || snap.timerRunning != lastTimerRunning) {
    bigView = BigView::Auto;
    lastDoseMode = snap.scaleDoseMode;
    lastTimerRunning = snap.timerRunning;
  }
  const bool weightKnown = snap.scaleEnabled && !isnan(snap.scaleWeight);
  const bool showDose = snap.scaleDoseMode && !snap.timerRunning;  // automatisch Gewicht
  bigShowsWeight = weightKnown && (bigView == BigView::Weight || (bigView == BigView::Auto && showDose));
  if (bigShowsWeight) {
    char num[16];
    snprintf(num, sizeof(num), "%.1f", snap.scaleWeight);
    canvas.setFont(kFontTimer);
    canvas.setTextSize(2);
    // z. B. "-491.6" ist breiter als der Timer -> auf die verfügbare Breite verkleinern
    constexpr int kMaxWidth = 250;
    const int w2 = canvas.textWidth(num);
    const float size = w2 > kMaxWidth ? 2.0f * kMaxWidth / w2 : 2.0f;
    canvas.setTextSize(size);
    const int w = canvas.textWidth(num);
    canvas.setTextColor(snap.timerRunning ? kColRun : kColAccent);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.drawString(num, kW / 2 - 10, 100);
    canvas.setTextSize(1);
    canvas.setFont(kFontBold);
    canvas.setTextColor(kColMuted);
    canvas.setTextDatum(textdatum_t::bottom_left);
    canvas.drawString("g", kW / 2 - 10 + w / 2 + 6, 145);
  } else {
    const String secs = formatSeconds(snap.timerMs);
    canvas.setFont(kFontTimer);
    canvas.setTextSize(2);
    const int w = canvas.textWidth(secs);
    canvas.setTextColor(snap.timerRunning ? kColRun : kColText);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.drawString(secs, kW / 2 - 10, 100);
    canvas.setTextSize(1);
    canvas.setFont(kFontBold);
    canvas.setTextColor(kColMuted);
    canvas.setTextDatum(textdatum_t::bottom_left);
    canvas.drawString("s", kW / 2 - 10 + w / 2 + 6, 145);  // direkt hinter der Zahl, wie beim "g"
  }

  // --- Waage (optional) bzw. letzte Bezüge ---------------------------------------
  if (snap.scaleEnabled) {
    // Gewicht und (während des Bezugs) Durchfluss
    char buf[40];
    if (!isnan(snap.scaleWeight)) {
      if (bigShowsWeight) {
        // Gewicht steht groß oben -> hier die Zeit (und ggf. was gewogen wird)
        if (snap.timerRunning) {
          snprintf(buf, sizeof(buf), "%s s   %.1f g/s", formatSeconds(snap.timerMs).c_str(), snap.scaleFlow);
        } else {
          snprintf(buf, sizeof(buf), "%sBezug %s s", snap.scaleDoseMode ? "Kaffee gemahlen  ·  " : "",
                   formatSeconds(snap.timerMs).c_str());
        }
      } else if (snap.scaleDoseMode) {
        snprintf(buf, sizeof(buf), "Kaffee gemahlen %.1f g", snap.scaleWeight);
      } else if (snap.timerRunning) {
        snprintf(buf, sizeof(buf), "%.1f g   %.1f g/s", snap.scaleWeight, snap.scaleFlow);
      } else {
        snprintf(buf, sizeof(buf), "%.1f g", snap.scaleWeight);
      }
      canvas.setFont(kFontBold);
      canvas.setTextColor(kColText);
    } else {
      snprintf(buf, sizeof(buf), "Waage %s", snap.scaleState);
      canvas.setFont(kFontSmall);
      canvas.setTextColor(kColMuted);
    }
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.drawString(buf, kW / 2, 155);
  } else if (snap.historyCount > 0) {
    String hist = "Zuletzt:";
    for (size_t i = 0; i < snap.historyCount && i < 4; ++i) {
      hist += (i ? " | " : " ") + formatSeconds(snap.history[i].durationMs);
    }
    hist += " s";
    canvas.setFont(kFontSmall);
    canvas.setTextColor(kColMuted);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.drawString(hist, kW / 2, 156);
  }

  // --- Live-Messwerte ----------------------------------------------------------
  const DetectorStatus& d = snap.detector;
  const DetectorSettings& st = snap.settings;
  canvas.setFont(kFontSmall);
  canvas.setTextColor(kColMuted);
  canvas.setTextDatum(textdatum_t::middle_left);
  // Balken: Pegel, KI ("klingt nach Pumpe") und ggf. Vibration.
  // Grün = Kriterium erfüllt; weiße Marke = Schwelle.
  struct Meter { const char* label; float value; float threshold; };
  const FrameScores& sc = d.scores;
  Meter meters[3];
  int count = 0;
  const bool useMic = snap.vibMode != static_cast<uint8_t>(vibration::Mode::Vib) || !snap.vibAvailable;
  const bool useVib = snap.vibMode != static_cast<uint8_t>(vibration::Mode::Mic) && snap.vibAvailable;
  if (useMic) {
    meters[count++] = {"Pegel", sc.levelRatio, st.levelThreshold};
    if (sc.clsActive && sc.clsPumpKnown) meters[count++] = {"KI Pumpe", sc.clsPump, cfg::kClsMinProb};
  }
  if (useVib) {
    // Vibration über dem Ruhepegel; Marke = Schwelle (Mitte des Balkens)
    const float over = snap.vibLevelDb - snap.vibBaselineDb;
    meters[count++] = {"Vibration", over / (2.0f * snap.vibThresholdDb), 0.5f};
  }
  const int step = count == 3 ? 12 : 15;
  int y = count == 3 ? 165 : count == 1 ? 175 : 168;
  for (int i = 0; i < count; ++i, y += step) {
    canvas.drawString(meters[i].label, 8, y + 4);
    drawBar(64, y + 1, 248, 6, meters[i].value, meters[i].threshold,
            meters[i].value >= meters[i].threshold ? kColOk : kColAccent);
  }

  drawButton(btnTeach);
  drawButton(btnReset);
  drawButton(btnInfo);
}

const char* teachTitle(TeachMode m) {
  switch (m) {
    case TeachMode::Pump:       return "TEACH: PUMPE";
    case TeachMode::Noise:      return "TEACH: STÖRGERÄUSCH";
    case TeachMode::Grinder:    return "TEACH: MÜHLE";
  }
  return "TEACH";
}

void drawTeach() {
  const bool done = snap.teachPhase == TeachPhase::Done;

  if (!done) {
    // --- Aufnahme läuft ------------------------------------------------------
    canvas.setFont(kFontBold);
    canvas.setTextColor(kColAccent);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString(teachTitle(snap.teachMode), 8, 34);

    canvas.setFont(kFontText);
    canvas.setTextColor(kColText);
    drawWrapped(snap.teachInstruction, 8, 58, kW - 16, 20, 125);

    drawBar(8, 130, kW - 16, 10, snap.teachProgress, -1.0f, kColAccent);

    // Mikrofonpegel -90..0 dBFS; Marke = Auslöseschwelle für "Pumpe läuft"
    const bool showBg = snap.teachPhase == TeachPhase::WaitPump ||
                        snap.teachPhase == TeachPhase::RecordPump;
    const float trigger = snap.teachBgDb + cfg::kTeachPumpTriggerDb;
    auto norm = [](float db) { return (db + 90.0f) / 90.0f; };
    drawBar(8, 160, kW - 16, 12, norm(snap.teachLiveDb), showBg ? norm(trigger) : -1.0f,
            showBg && snap.teachLiveDb >= trigger ? kColOk : kColAccent);

    char buf[64];
    if (snap.teachPhase == TeachPhase::RecordPump) {
      snprintf(buf, sizeof(buf), "Aufnahme %.1f s  |  Pegel %.1f dB",
               snap.teachRecordedMs / 1000.0f, snap.teachLiveDb);
    } else if (showBg) {
      snprintf(buf, sizeof(buf), "Pegel %.1f dB  (Hintergrund %.1f dB)", snap.teachLiveDb, snap.teachBgDb);
    } else {
      snprintf(buf, sizeof(buf), "Pegel %.1f dB", snap.teachLiveDb);
    }
    canvas.setFont(kFontSmall);
    canvas.setTextColor(kColMuted);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString(buf, 8, 178);
  } else {
    // --- Ergebnis ---------------------------------------------------------------
    const TeachResult& r = snap.teachResult;
    const char* title;
    uint16_t col;
    switch (r.quality) {
      case TeachQuality::Ok:      title = "Aufnahme OK";             col = kColOk;   break;
      case TeachQuality::Warning: title = "OK mit Einschränkungen"; col = kColWarn; break;
      default:                    title = "Nicht verwendbar";        col = kColErr;  break;
    }
    canvas.fillRoundRect(8, 32, kW - 16, 28, 6, col);
    canvas.setFont(kFontBold);
    canvas.setTextColor(kColDark);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.drawString(title, 16, 46);
    char score[24];
    snprintf(score, sizeof(score), "%d/100", r.score);
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.drawString(score, kW - 16, 46);

    // Begründungen (so viele wie Platz haben)
    canvas.setFont(kFontSmall);
    canvas.setTextColor(kColText);
    int y = 66;
    const int maxY = kBtnY - 4;
    for (uint8_t i = 0; i < r.messageCount && y < maxY - 13; ++i) {
      canvas.fillCircle(11, y + 6, 2, col);
      y = drawWrapped(r.messages[i], 18, y, kW - 26, 14, maxY) + 3;
    }
    if (y >= maxY - 13 && r.messageCount > 1) {
      canvas.setTextColor(kColMuted);
      canvas.setTextDatum(textdatum_t::bottom_right);
      canvas.drawString("Details: Webseite", kW - 8, maxY);
    }
  }

  // Buttons je nach Phase
  // Ergebnis verwendbar: neues Profil speichern bzw. bestehendes ersetzen
  // oder ergänzen (z. B. Leerbezug + echter Bezug).
  // Störgeräusch: speichern (neuer Eintrag).
  const bool usable = done && snap.teachResult.quality != TeachQuality::Error;
  const bool pumpMode = snap.teachMode == TeachMode::Pump;
  btnFinish.visible = snap.teachPhase == TeachPhase::RecordPump;
  btnSave.visible = usable;
  const bool learned = snap.profileValid && !snap.profileDefault;
  // Mühle: als neue Mühle speichern (solange Platz ist) oder der ähnlichsten ergänzen
  const bool grinderMode = snap.teachMode == TeachMode::Grinder;
  if (grinderMode && snap.grinderCount >= cfg::kMaxGrinders) btnSave.visible = false;
  btnSave.label = pumpMode && learned                     ? "ERSETZEN"
                  : grinderMode && snap.grinderCount > 0 ? "NEUE MÜHLE"
                                                          : "SPEICHERN";
  btnAdd.visible = usable && (pumpMode      ? learned
                              : grinderMode ? snap.grinderCount > 0
                                            : snap.noiseCount > 0);  // Störgeräusch ergänzen
  btnAdd.label = "ERGÄNZEN";
  btnRetry.visible = done && !btnAdd.visible;
  btnCancel.label = done ? "VERWERFEN" : "ABBRECHEN";
  drawButton(btnFinish);
  drawButton(btnSave);
  drawButton(btnAdd);
  drawButton(btnRetry);
  drawButton(btnCancel);
}

/// Kachel mit Titel und kleinem Untertitel.
void drawTile(const Button& b, const char* subtitle) {
  if (!b.visible) return;
  canvas.fillRoundRect(b.x, b.y, b.w, b.h, 8, b.color);
  canvas.setTextColor(b.textColor);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.setFont(kFontBold);
  canvas.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2 - 8);
  canvas.setFont(kFontSmall);
  canvas.drawString(subtitle, b.x + b.w / 2, b.y + b.h / 2 + 12);
}

void drawResetMenu() {
  canvas.setFont(kFontBold);
  canvas.setTextColor(kColAccent);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.drawString("Was soll auf 0?", 8, 34);

  drawTile(btnResetAll, "Bezugszeit, Gewicht und Waagen-Zeit");
  drawTile(btnResetTimer, "hier am Display");
  drawTile(btnResetTare, "Waage tarieren");
  drawTile(btnResetScale, "Timer Waage");
  drawButton(btnBack);
}

void drawTeachSelect() {
  canvas.setFont(kFontBold);
  canvas.setTextColor(kColAccent);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.drawString("Was soll gelernt werden?", 8, 34);

  drawTile(btnLearnPump, "Ruhe + Bezug");
  drawTile(btnLearnGrinder, "mahlen lassen");
  drawTile(btnLearnNoise, "z. B. Milch");

  canvas.setFont(kFontSmall);
  canvas.setTextColor(kColMuted);
  canvas.setTextDatum(textdatum_t::top_left);
  canvas.drawString("Jede Aufnahme misst zuerst 3 s Ruhe – Reihenfolge egal.", 8, 184);
  drawButton(btnBack);
}

void drawInfo() {
  canvas.setFont(kFontText);
  canvas.setTextDatum(textdatum_t::top_left);
  char buf[96];
  int y = 34;
  auto line = [&](const char* label, const String& value, uint16_t col = kColText) {
    canvas.setTextColor(kColMuted);
    canvas.drawString(label, 8, y);
    canvas.setTextColor(col);
    canvas.drawString(value, 100, y);
    y += 20;
  };

  line("WLAN", net::apActive() ? "AP " + net::networkName()
                                : (net::connected() ? net::networkName() : String("verbinde …")),
       net::connected() ? kColOk : kColWarn);
  line("Adresse", "http://" + net::ip());

  snprintf(buf, sizeof(buf), "%u: %s", snap.activeSlot + 1, snap.profileName);
  line("Profil", buf, snap.profileDefault ? kColWarn : kColOk);
  if (snap.profileDefault) {
    line("Pumpe", "Standard (ohne Teach)", kColWarn);
  } else {
    snprintf(buf, sizeof(buf), "%u Aufnahme(n), Q %.0f", snap.profileRecordings, snap.profileQuality);
    line("Pumpe", buf);
  }
  if (snap.clsReady) {
    snprintf(buf, sizeof(buf), "%u Klassen, %.0f %% richtig", snap.clsClasses, snap.clsAccuracy * 100.0f);
    line("KI", buf, kColOk);
  } else {
    line("KI", "noch nicht trainiert", kColWarn);
  }

  snprintf(buf, sizeof(buf), "Störger. %u  |  Mikro %ux%s", snap.noiseCount, snap.micGain,
           snap.clipRatio > 0.001f ? " ÜBERST.!" : "");
  line("Sonst.", buf, snap.clipRatio > 0.001f ? kColErr : kColText);
  snprintf(buf, sizeof(buf), "%.0f %%  (Pegel-Schwelle)", snap.settings.levelThreshold * 100.0f);
  line("Empfindl.", buf);
  snprintf(buf, sizeof(buf), "Ein %u ms / Aus %u ms", snap.settings.onDelayMs, snap.settings.offDelayMs);
  line("Verzög.", buf);

  drawButton(btnProfile);
  drawButton(btnSensDown);
  drawButton(btnSensUp);
  drawButton(btnInfoBack);
}

// ---------------------------------------------------------------------------
// Einstellungen am Display (alle Werte der Webseite, mit -/+ änderbar)
// ---------------------------------------------------------------------------

/// Einträge des Einstellungs-Screens.
enum class SetItem {
  Profile, Source, VibThreshold, Level, OnDelay, OffDelay, MinShot, MicGain, Scale, ScaleSearch, ScaleChoose,
  ScaleBound, ScaleAutoTare, ScaleTimer, ScaleGrindTare, ScalePortafilter, Grinder, Noise, ClearHistory,
  ResetProfile
};

constexpr int kSetRowY = 30;                                ///< erste Zeile
constexpr int kSetRowH = 22;                                ///< Zeilenhöhe
constexpr int kSetRows = 7;                                 ///< Zeilen je Seite
constexpr int kSetPagerY = kSetRowY + kSetRows * kSetRowH;  ///< Blätter-Leiste
constexpr int kMaxSetItems = 24;

Button btnMinus  {  4, kBtnY, 75, kBtnH, "-",       kColLine,   kColText, true};
Button btnPlus   { 83, kBtnY, 75, kBtnH, "+",       kColLine,   kColText, true};
Button btnAction {162, kBtnY, 75, kBtnH, "LÖSCHEN", kColErr,    kColText, false};
Button btnSetBack{241, kBtnY, 75, kBtnH, "ZURÜCK",  kColAccent, kColDark, true};

int setSelected = 0;          ///< gewählter Eintrag
int setPage = 0;              ///< aktuelle Seite
uint8_t noiseSel = 0;         ///< gewähltes Störgeräusch (zum Löschen)
uint8_t grinderSel = 0;       ///< gewählte Mühle (zum Löschen)
uint8_t scaleSel = 0;         ///< gewählte Waage aus der Suche (zum Koppeln)
SetItem confirmItem = SetItem::Profile;
uint32_t confirmUntil = 0;    ///< Löschen erst nach zweitem Tippen bis zu diesem Zeitpunkt

/// Stufen der Mikrofon-Verstärkung (feiner im unteren Bereich).
constexpr uint8_t kGainSteps[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};

/// Sichtbare Einträge – abhängig von Sensor, Waage und gelernten Geräuschen.
int buildItems(SetItem* out) {
  int n = 0;
  out[n++] = SetItem::Profile;
  if (snap.vibAvailable) {
    out[n++] = SetItem::Source;
    if (snap.vibMode != 0) out[n++] = SetItem::VibThreshold;
  }
  out[n++] = SetItem::Level;
  out[n++] = SetItem::OnDelay;
  out[n++] = SetItem::OffDelay;
  out[n++] = SetItem::MinShot;
  out[n++] = SetItem::MicGain;
  out[n++] = SetItem::Scale;
  if (snap.scaleEnabled) {
    out[n++] = SetItem::ScaleAutoTare;
    out[n++] = SetItem::ScaleTimer;
    out[n++] = SetItem::ScaleGrindTare;
    out[n++] = SetItem::ScalePortafilter;
    out[n++] = SetItem::ScaleSearch;
    if (!scale::found().empty()) out[n++] = SetItem::ScaleChoose;
    out[n++] = SetItem::ScaleBound;
  }
  if (snap.grinderCount > 0) out[n++] = SetItem::Grinder;
  if (snap.noiseCount > 0) out[n++] = SetItem::Noise;
  out[n++] = SetItem::ClearHistory;
  out[n++] = SetItem::ResetProfile;
  return n;
}

const char* itemLabel(SetItem it) {
  switch (it) {
    case SetItem::Profile:      return "Profil";
    case SetItem::Source:       return "Erkennung";
    case SetItem::VibThreshold: return "Vibr.-Schwelle";
    case SetItem::Level:        return "Pegel-Schwelle";
    case SetItem::OnDelay:      return "Einschaltverz.";
    case SetItem::OffDelay:     return "Ausschaltverz.";
    case SetItem::MinShot:      return "Mindestdauer";
    case SetItem::MicGain:      return "Mikrofon";
    case SetItem::Scale:        return "Waage";
    case SetItem::ScaleAutoTare:return "Auto-Tara";
    case SetItem::ScaleTimer:   return "Waagen-Timer";
    case SetItem::ScaleGrindTare: return "Mühlen-Tara";
    case SetItem::ScalePortafilter: return "Siebträger";
    case SetItem::ScaleSearch:  return "Waage suchen";
    case SetItem::ScaleChoose:  return "Waage wählen";
    case SetItem::ScaleBound:   return "Gekoppelt";
    case SetItem::Grinder:      return "Mühle";
    case SetItem::Noise:        return "Störgeräusch";
    case SetItem::ClearHistory: return "Historie";
    case SetItem::ResetProfile: return "Profil zurücks.";
  }
  return "";
}

/// Eintrag hat einen mit -/+ änderbaren Wert?
bool hasValue(SetItem it) {
  return it != SetItem::ClearHistory && it != SetItem::ResetProfile && it != SetItem::ScaleSearch &&
         it != SetItem::ScaleBound;
}

/// Eintrag hat eine Aktion (Löschen/Zurücksetzen)?
bool hasAction(SetItem it) {
  return it == SetItem::Noise || it == SetItem::Grinder || it == SetItem::ClearHistory ||
         it == SetItem::ResetProfile || it == SetItem::ScalePortafilter || it == SetItem::ScaleSearch ||
         it == SetItem::ScaleChoose || (it == SetItem::ScaleBound && scale::config().boundAddr[0]);
}

/// Aktionen, die ohne Sicherheitsabfrage sofort ausgeführt werden.
bool isHarmless(SetItem it) {
  // Kopplung lösen (ScaleBound) bewusst mit Rückfrage
  return it == SetItem::ScalePortafilter || it == SetItem::ScaleSearch || it == SetItem::ScaleChoose;
}

String itemValue(SetItem it) {
  const DetectorSettings& st = snap.settings;
  char buf[40];
  switch (it) {
    case SetItem::Profile:
      snprintf(buf, sizeof(buf), "%u: %.14s", snap.activeSlot + 1, snap.profileName);
      break;
    case SetItem::Source:
      snprintf(buf, sizeof(buf), "%s", vibration::modeName(static_cast<vibration::Mode>(snap.vibMode)));
      break;
    case SetItem::VibThreshold: snprintf(buf, sizeof(buf), "+%.0f dB", snap.vibThresholdDb); break;
    case SetItem::Level:       snprintf(buf, sizeof(buf), "%.0f %%", st.levelThreshold * 100.0f); break;
    case SetItem::OnDelay:     snprintf(buf, sizeof(buf), "%u ms", st.onDelayMs); break;
    case SetItem::OffDelay:    snprintf(buf, sizeof(buf), "%.1f s", st.offDelayMs / 1000.0f); break;
    case SetItem::MinShot:     snprintf(buf, sizeof(buf), "%.0f s", st.minShotSeconds); break;
    case SetItem::MicGain:     snprintf(buf, sizeof(buf), "%ux", snap.micGain); break;
    case SetItem::Scale:
      snprintf(buf, sizeof(buf), "%s", snap.scaleEnabled ? snap.scaleState : "aus");
      break;
    case SetItem::ScaleAutoTare: snprintf(buf, sizeof(buf), scale::config().autoTare ? "an" : "aus"); break;
    case SetItem::ScaleTimer:    snprintf(buf, sizeof(buf), scale::config().syncTimer ? "an" : "aus"); break;
    case SetItem::ScaleGrindTare: snprintf(buf, sizeof(buf), scale::config().grindTare ? "an" : "aus"); break;
    case SetItem::ScalePortafilter: snprintf(buf, sizeof(buf), "%.1f g", scale::config().portafilterG); break;
    case SetItem::ScaleSearch: {
      const size_t n = scale::found().size();
      if (scale::searching()) {
        snprintf(buf, sizeof(buf), "sucht …");
      } else {
        snprintf(buf, sizeof(buf), "%u gefunden", (unsigned)n);
      }
      break;
    }
    case SetItem::ScaleChoose: {
      const std::vector<scale::Found> list = scale::found();
      if (scaleSel >= list.size()) scaleSel = 0;
      if (list.empty()) {
        snprintf(buf, sizeof(buf), "-");
      } else {
        snprintf(buf, sizeof(buf), "%.14s (%u/%u)", list[scaleSel].name, scaleSel + 1, (unsigned)list.size());
      }
      break;
    }
    case SetItem::ScaleBound: {
      const scale::Config c = scale::config();
      snprintf(buf, sizeof(buf), "%.18s", c.boundAddr[0] ? (c.boundName[0] ? c.boundName : c.boundAddr) : "keine");
      break;
    }
    case SetItem::Grinder:
      snprintf(buf, sizeof(buf), "%.12s (%u/%u)", snap.grinderNames[grinderSel], grinderSel + 1,
               snap.grinderCount);
      break;
    case SetItem::Noise:       snprintf(buf, sizeof(buf), "%u von %u", noiseSel + 1, snap.noiseCount); break;
    case SetItem::ClearHistory: snprintf(buf, sizeof(buf), "%u Einträge", (unsigned)snap.historyCount); break;
    case SetItem::ResetProfile: snprintf(buf, sizeof(buf), snap.profileDefault ? "Standard" : "gelernt"); break;
  }
  return buf;
}

/// Wert eines Eintrags um eine Stufe ändern (dir = -1 / +1).
void adjustItem(SetItem it, int dir) {
  DetectorSettings s = app().settings();
  switch (it) {
    case SetItem::Profile:
      app().cmdSelectSlot((snap.activeSlot + cfg::kProfileSlots + dir) % cfg::kProfileSlots);
      return;
    case SetItem::Source:
    case SetItem::VibThreshold: {
      vibration::Config c = vibration::config();
      if (it == SetItem::Source) {
        c.mode = static_cast<vibration::Mode>((static_cast<int>(c.mode) + 3 + dir) % 3);
      } else {
        c.thresholdDb += dir;
      }
      vibration::setConfig(c);
      return;
    }
    case SetItem::Level:       s.levelThreshold += 0.05f * dir; break;
    case SetItem::OnDelay:     s.onDelayMs = (uint16_t)std::max(0, (int)s.onDelayMs + 50 * dir); break;
    case SetItem::OffDelay:    s.offDelayMs = (uint16_t)std::max(0, (int)s.offDelayMs + 100 * dir); break;
    case SetItem::MinShot:     s.minShotSeconds += dir; break;
    case SetItem::MicGain: {
      // nächste Stufe oberhalb bzw. unterhalb der aktuellen Verstärkung
      int idx = 0;
      while (idx + 1 < (int)sizeof(kGainSteps) && kGainSteps[idx] < snap.micGain) ++idx;
      if (kGainSteps[idx] != snap.micGain && dir < 0) ++idx;  // Zwischenwert -> darunter
      idx = constrain(idx + dir, 0, (int)sizeof(kGainSteps) - 1);
      app().cmdSetMicGain(kGainSteps[idx]);
      return;
    }
    case SetItem::Scale:
    case SetItem::ScaleAutoTare:
    case SetItem::ScaleTimer:
    case SetItem::ScaleGrindTare: {
      // -/+ schaltet aus bzw. ein
      scale::Config c = scale::config();
      bool& flag = it == SetItem::Scale         ? c.enabled
                   : it == SetItem::ScaleAutoTare ? c.autoTare
                   : it == SetItem::ScaleTimer    ? c.syncTimer
                                                  : c.grindTare;
      flag = dir > 0;
      scale::setConfig(c);
      return;
    }
    case SetItem::ScaleChoose: {
      const size_t n = scale::found().size();
      if (n) scaleSel = (scaleSel + n + dir) % n;
      return;
    }
    case SetItem::ScalePortafilter: {
      scale::Config c = scale::config();
      c.portafilterG = fmaxf(0.0f, roundf(c.portafilterG) + dir);
      scale::setConfig(c);
      return;
    }
    case SetItem::Grinder:
      if (snap.grinderCount) grinderSel = (grinderSel + snap.grinderCount + dir) % snap.grinderCount;
      return;
    case SetItem::Noise:
      if (snap.noiseCount) noiseSel = (noiseSel + snap.noiseCount + dir) % snap.noiseCount;
      return;
    default:
      return;
  }
  app().cmdApplySettings(s);  // begrenzt die Werte und speichert im Profil
}

void runAction(SetItem it) {
  switch (it) {
    case SetItem::Noise:        app().cmdDeleteNoise(noiseSel); noiseSel = 0; break;
    case SetItem::Grinder:      app().cmdDeleteGrinder(grinderSel); grinderSel = 0; break;
    case SetItem::ClearHistory: app().cmdClearHistory(); break;
    case SetItem::ResetProfile: app().cmdDeleteSlot(snap.activeSlot); break;
    case SetItem::ScalePortafilter: scale::learnPortafilter(); break;
    case SetItem::ScaleSearch:  scale::startSearch(); scaleSel = 0; break;
    case SetItem::ScaleChoose: {
      const std::vector<scale::Found> list = scale::found();
      if (scaleSel < list.size()) scale::bind(list[scaleSel].addr, list[scaleSel].name);
      break;
    }
    case SetItem::ScaleBound:   scale::unbind(); break;
    default: break;
  }
}

void drawSettings() {
  SetItem items[kMaxSetItems];
  const int n = buildItems(items);
  setSelected = constrain(setSelected, 0, n - 1);
  const int pages = (n + kSetRows - 1) / kSetRows;
  setPage = constrain(setPage, 0, pages - 1);
  if (noiseSel >= snap.noiseCount) noiseSel = 0;
  if (grinderSel >= snap.grinderCount) grinderSel = 0;

  // Zeilen: Bezeichnung links, Wert rechts; gewählte Zeile hinterlegt
  for (int r = 0; r < kSetRows; ++r) {
    const int i = setPage * kSetRows + r;
    if (i >= n) break;
    const int y = kSetRowY + r * kSetRowH;
    if (i == setSelected) canvas.fillRoundRect(4, y, kW - 8, kSetRowH - 2, 4, kColLine);
    canvas.setFont(kFontText);
    canvas.setTextColor(i == setSelected ? kColText : kColMuted);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.drawString(itemLabel(items[i]), 10, y + kSetRowH / 2);
    canvas.setTextColor(kColAccent);
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.drawString(itemValue(items[i]), kW - 10, y + kSetRowH / 2);
  }

  // Blätter-Leiste
  if (pages > 1) {
    char buf[32];
    snprintf(buf, sizeof(buf), "<   Seite %d/%d   >", setPage + 1, pages);
    canvas.setFont(kFontSmall);
    canvas.setTextColor(kColMuted);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.drawString(buf, kW / 2, kSetPagerY + 9);
  }

  // Buttons passend zum gewählten Eintrag
  const SetItem sel = items[setSelected];
  btnMinus.visible = btnPlus.visible = hasValue(sel);
  btnAction.visible = hasAction(sel);
  const bool confirming = confirmItem == sel && millis() < confirmUntil;
  btnAction.label = confirming ? "SICHER?"
                  : sel == SetItem::ResetProfile ? "RESET"
                  : sel == SetItem::ScalePortafilter ? "WIEGEN"
                  : sel == SetItem::ScaleSearch  ? "SUCHEN"
                  : sel == SetItem::ScaleChoose  ? "KOPPELN"
                  : sel == SetItem::ScaleBound   ? "LÖSEN"
                                                 : "LÖSCHEN";
  btnAction.color = isHarmless(sel) ? kColAccent : kColErr;
  btnAction.textColor = isHarmless(sel) ? kColDark : kColText;
  drawButton(btnMinus);
  drawButton(btnPlus);
  drawButton(btnAction);
  drawButton(btnSetBack);
}

void handleSettingsTouch(int x, int y) {
  SetItem items[kMaxSetItems];
  const int n = buildItems(items);
  const int pages = (n + kSetRows - 1) / kSetRows;
  const SetItem sel = items[constrain(setSelected, 0, n - 1)];

  if (y >= kSetRowY && y < kSetPagerY) {
    // Zeile antippen = auswählen
    const int i = setPage * kSetRows + (y - kSetRowY) / kSetRowH;
    if (i < n) {
      setSelected = i;
      confirmUntil = 0;
    }
  } else if (y >= kSetPagerY && y < kBtnY && pages > 1) {
    // linke Hälfte zurück, rechte Hälfte weiter blättern
    setPage = (setPage + (x < kW / 2 ? pages - 1 : 1)) % pages;
    setSelected = setPage * kSetRows;
    confirmUntil = 0;
  } else if (btnMinus.hit(x, y)) {
    adjustItem(sel, -1);
  } else if (btnPlus.hit(x, y)) {
    adjustItem(sel, +1);
  } else if (btnAction.hit(x, y)) {
    // Sicherheitsabfrage: erst nach zweitem Tippen innerhalb von 3 s ausführen
    // (harmlose Aktionen wie Tara sofort)
    if (isHarmless(sel) || (confirmItem == sel && millis() < confirmUntil)) {
      runAction(sel);
      confirmUntil = 0;
    } else {
      confirmItem = sel;
      confirmUntil = millis() + 3000;
    }
  } else if (btnSetBack.hit(x, y)) {
    app().flushSettings();  // Änderungen beim Verlassen sofort sichern
    screen = Screen::Info;
  }
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

/// Empfindlichkeit ändern: niedrigere Pegel-Schwelle = empfindlicher.
void changeSensitivity(float delta) {
  DetectorSettings s = app().settings();
  s.levelThreshold -= delta;
  app().cmdApplySettings(s);  // wird intern begrenzt und gespeichert
}

void handleTouch() {
  const auto t = M5.Touch.getDetail();
  if (!t.wasClicked()) return;
  const int x = t.x, y = t.y;

  switch (screen) {
    case Screen::Main:
      if (btnTeach.hit(x, y)) {
        screen = Screen::TeachSelect;
      } else if (y >= kBigTop && y < kBigBottom && snap.scaleEnabled && !isnan(snap.scaleWeight)) {
        // Große Zahl antippen: zwischen Timer und Gewicht umschalten
        bigView = bigShowsWeight ? BigView::Timer : BigView::Weight;
      } else if (btnReset.hit(x, y)) {
        // Mit Waage: Auswahl, was auf 0 gesetzt wird; ohne Waage sofort den Timer
        if (snap.scaleConnected) {
          screen = Screen::ResetMenu;
        } else {
          app().cmdResetTimer();
        }
      } else if (btnInfo.hit(x, y)) {
        screen = Screen::Info;
      }
      break;

    case Screen::ResetMenu:
      if (btnResetAll.hit(x, y)) {
        app().cmdResetTimer();  // Timer, Gewicht und Waagen-Timer
      } else if (btnResetTimer.hit(x, y)) {
        app().cmdResetTimer(false);
      } else if (btnResetTare.hit(x, y)) {
        scale::tare();
      } else if (btnResetScale.hit(x, y)) {
        scale::timerStop();
        scale::timerReset();
      } else if (!btnBack.hit(x, y)) {
        break;  // daneben getippt: Menü bleibt offen
      }
      screen = Screen::Main;  // nach jeder Auswahl bzw. ZURÜCK
      break;

    case Screen::TeachSelect: {
      const struct { const Button& b; TeachMode m; } choices[] = {
          {btnLearnPump, TeachMode::Pump},
          {btnLearnGrinder, TeachMode::Grinder}, {btnLearnNoise, TeachMode::Noise}};
      for (const auto& c : choices) {
        if (c.b.hit(x, y)) {
          app().cmdStartTeach(c.m);
          screen = Screen::Teach;
          return;
        }
      }
      if (btnBack.hit(x, y)) {
        screen = Screen::Main;
      }
      break;
    }

    case Screen::Teach:
      if (btnFinish.hit(x, y)) {
        app().cmdFinishTeach();
      } else if (btnSave.hit(x, y)) {
        if (app().cmdSaveTeach(false)) screen = Screen::Main;
      } else if (btnAdd.hit(x, y)) {
        if (app().cmdSaveTeach(true)) screen = Screen::Main;
      } else if (btnRetry.hit(x, y)) {
        app().cmdStartTeach(snap.teachMode);  // gleichen Modus wiederholen
      } else if (btnCancel.hit(x, y)) {
        app().cmdCancelTeach();
        screen = Screen::Main;
      }
      break;

    case Screen::Settings:
      handleSettingsTouch(x, y);
      break;

    case Screen::Info:
      if (btnProfile.hit(x, y)) {
        screen = Screen::Settings;
      } else if (btnSensDown.hit(x, y)) {
        changeSensitivity(-0.05f);
      } else if (btnSensUp.hit(x, y)) {
        changeSensitivity(+0.05f);
      } else if (btnInfoBack.hit(x, y)) {
        screen = Screen::Main;
      }
      break;
  }
}

}  // namespace

namespace ui {

void begin() {
  M5.Display.setRotation(1);
  M5.Display.setBrightness(160);
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(kW, kH);
}

void loop() {
  handleTouch();

  const uint32_t now = millis();
  if (now - lastDrawMs < cfg::kDisplayIntervalMs) return;
  lastDrawMs = now;

  app().snapshot(snap);

  // Teach-Modus wurde ggf. über die Webseite gestartet/beendet -> Screen folgen.
  if (snap.teachPhase != TeachPhase::Idle) {
    screen = Screen::Teach;
  } else if (screen == Screen::Teach) {
    screen = Screen::Main;
  }

  canvas.fillScreen(kColBg);
  drawHeader();
  switch (screen) {
    case Screen::Main:  drawMain();  break;
    case Screen::TeachSelect: drawTeachSelect(); break;
    case Screen::Teach: drawTeach(); break;
    case Screen::Info:  drawInfo();  break;
    case Screen::Settings: drawSettings(); break;
    case Screen::ResetMenu: drawResetMenu(); break;
  }
  canvas.pushSprite(0, 0);
}

void showUpdateProgress(int percent, const char* title) {
  // Nur bei geändertem Wert neu zeichnen – onProgress kommt sehr häufig.
  static int lastPercent = -2;
  if (percent == lastPercent && percent >= 0 && percent < 100) return;
  lastPercent = percent;

  canvas.fillScreen(kColBg);
  drawHeader();
  canvas.setFont(kFontBold);
  canvas.setTextColor(percent < 0 ? kColErr : kColAccent);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.drawString(title, kW / 2, 90);
  if (percent >= 0) {
    drawBar(20, 120, kW - 40, 16, percent / 100.0f, -1.0f, kColAccent);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d %%", percent);
    canvas.setTextColor(kColText);
    canvas.drawString(buf, kW / 2, 160);
    canvas.setFont(kFontSmall);
    canvas.setTextColor(kColMuted);
    canvas.drawString("Bitte nicht ausschalten", kW / 2, 190);
  }
  canvas.pushSprite(0, 0);
}

}  // namespace ui
