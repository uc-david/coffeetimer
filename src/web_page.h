// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 uc-david
/**
 * @file web_page.h
 * @brief Eingebettete Single-Page-Webseite (HTML/CSS/JS in einer Datei).
 *
 * Die Seite kommt ohne externe Ressourcen aus, damit sie auch im
 * Access-Point-Modus (ohne Internet) funktioniert. Sie fragt /api/status
 * etwa viermal pro Sekunde ab und spiegelt damit die Anzeige am Gerät.
 */
#pragma once

#include <pgmspace.h>

static const char kWebPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Coffee Timer</title>
<style>
  :root {
    --bg: #1b1411; --card: #2a201b; --line: #3d2f27; --text: #f3ebe4; --muted: #b3a197;
    --accent: #e0a458; --ok: #5fbf77; --warn: #e6b450; --err: #e5605a; --run: #ffb347;
  }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
         background: var(--bg); color: var(--text); }
  main { max-width: 640px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 20px; margin: 4px 0 16px; display: flex; justify-content: space-between; align-items: center; }
  h2 { font-size: 15px; margin: 0 0 12px; color: var(--muted); text-transform: uppercase; letter-spacing: .06em; }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 14px; padding: 16px; margin-bottom: 14px; }
  .status { font-size: 14px; padding: 4px 10px; border-radius: 999px; background: var(--line); }
  .status.on { background: var(--run); color: #2a1a0a; }
  .timer { font-size: 88px; font-weight: 700; text-align: center; font-variant-numeric: tabular-nums;
           line-height: 1; margin: 8px 0 4px; }
  .timer.running { color: var(--run); }
  .timer.dose { color: var(--accent); font-size: 72px; }
  .timer small { font-size: 32px; color: var(--muted); }
  .hint { text-align: center; color: var(--muted); font-size: 14px; min-height: 18px; }
  .meter { margin: 10px 0; }
  .meter label { display: flex; justify-content: space-between; font-size: 13px; color: var(--muted); margin-bottom: 4px; }
  .bar { position: relative; height: 10px; background: var(--line); border-radius: 5px; overflow: hidden; }
  .bar > i { position: absolute; left: 0; top: 0; bottom: 0; background: var(--accent); width: 0; transition: width .2s; }
  .bar > b { position: absolute; top: -2px; bottom: -2px; width: 2px; background: var(--text); }
  .bar.active > i { background: var(--ok); }
  .row { display: flex; gap: 10px; flex-wrap: wrap; }
  button { flex: 1; min-width: 120px; padding: 12px; border: 0; border-radius: 10px; font-size: 15px; font-weight: 600;
           background: var(--line); color: var(--text); cursor: pointer; }
  button.primary { background: var(--accent); color: #2a1a0a; }
  button.danger { background: var(--err); color: #fff; }
  button:disabled { opacity: .4; cursor: default; }
  .progress { height: 8px; background: var(--line); border-radius: 4px; overflow: hidden; margin: 12px 0; }
  .progress > i { display: block; height: 100%; background: var(--accent); width: 0; transition: width .2s; }
  .instr { font-size: 17px; margin: 6px 0; }
  .result { border-radius: 10px; padding: 12px; margin: 12px 0; border: 1px solid var(--line); }
  .result.ok { border-color: var(--ok); } .result.warning { border-color: var(--warn); } .result.error { border-color: var(--err); }
  .result h3 { margin: 0 0 6px; font-size: 18px; }
  .result.ok h3 { color: var(--ok); } .result.warning h3 { color: var(--warn); } .result.error h3 { color: var(--err); }
  .result ul { margin: 6px 0 0; padding-left: 20px; } .result li { margin: 4px 0; }
  .kv { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 6px; font-size: 13px; color: var(--muted); }
  .kv span b { color: var(--text); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 15px; }
  td { padding: 6px 4px; border-bottom: 1px solid var(--line); } td:last-child { text-align: right; font-variant-numeric: tabular-nums; }
  .field { margin: 12px 0; }
  .field label { display: flex; justify-content: space-between; font-size: 14px; margin-bottom: 4px; }
  .field small { color: var(--muted); display: block; margin-top: 2px; font-size: 12px; }
  input[type=range] { width: 100%; accent-color: var(--accent); }
  input[type=text], input[type=password] { width: 100%; padding: 10px; border-radius: 8px; border: 1px solid var(--line);
           background: var(--bg); color: var(--text); font-size: 15px; margin-bottom: 8px; }
  details summary { cursor: pointer; color: var(--muted); }
  .hidden { display: none; }
  .toast { position: fixed; left: 50%; bottom: 20px; transform: translateX(-50%); background: var(--text); color: var(--bg);
           padding: 10px 16px; border-radius: 10px; opacity: 0; transition: opacity .3s; pointer-events: none; }
  .toast.show { opacity: 1; }
  .offline { color: var(--err); font-size: 13px; text-align: center; }
</style>
</head>
<body>
<main>
  <h1>☕ Coffee Timer <span><span id="status" class="status">–</span> <span id="battery" class="status">–</span></span></h1>

  <!-- Timer ------------------------------------------------------------- -->
  <section class="card" id="timerCard">
    <div id="timer" class="timer" onclick="toggleBigView()" title="Antippen: Timer / Gewicht">0.0<small>s</small></div>
    <div id="weightLine" class="hidden" style="text-align:center;font-size:28px;font-weight:700;font-variant-numeric:tabular-nums">
      <span id="weight">–</span> <small id="flow" style="font-size:16px;color:var(--muted)"></small>
      <button style="min-width:0;padding:4px 10px;margin-left:8px;font-size:13px" onclick="post('/api/scale/tare')">Tara</button>
      <button style="min-width:0;padding:4px 10px;font-size:13px" onclick="post('/api/scale/timer-reset')" title="Timer an der Waage auf 0">Waagen-Zeit 0</button>
    </div>
    <div id="hint" class="hint"></div>
    <div id="profileLine" class="hint" style="margin-top:4px"></div>
    <div class="meter">
      <label><span>Pegel</span><span id="lvlTxt"></span></label>
      <div class="bar" id="lvlBar"><i></i><b></b></div>
    </div>
    <div class="meter">
      <label><span>Gleichmäßigkeit (Schwankung)</span><span id="stdTxt"></span></label>
      <div class="bar" id="stdBar"><i></i><b></b></div>
    </div>
    <div class="meter" id="clsMeter">
      <label><span>KI: klingt nach Pumpe</span><span id="clsTxt"></span></label>
      <div class="bar" id="clsBar"><i></i><b></b></div>
    </div>
    <div class="row" style="margin-top:14px">
      <button onclick="post('/api/timer/reset')" title="Timer, Gewicht und Timer der Waage auf 0">Reset</button>
    </div>
    <div class="row" style="margin-top:8px">
      <span style="color:var(--muted);align-self:center">Einlernen:</span>
      <button class="teachStart primary" data-mode="pump" onclick="startTeach('pump')">Pumpe</button>
      <button class="teachStart" data-mode="grinder" onclick="startTeach('grinder')">Mühle</button>
      <button class="teachStart" data-mode="noise" onclick="startTeach('noise')">Störgeräusch</button>
    </div>
    <small style="color:var(--muted)" id="teachTip">Jede Aufnahme misst zuerst 3 s Ruhe und ist unabhängig von den anderen –
      die Reihenfolge ist egal. Mühle: neue Mühle oder einer vorhandenen hinzufügen. Störgeräusch: z. B. Milchaufschäumer.</small>
    <div id="offline" class="offline hidden">Keine Verbindung zum Gerät …</div>
  </section>

  <!-- Teach-Modus --------------------------------------------------------- -->
  <section class="card hidden" id="teachCard">
    <h2 id="teachTitle">Teach-Modus</h2>
    <div id="teachInstr" class="instr"></div>
    <div class="progress"><i id="teachProg"></i></div>
    <div class="meter">
      <label><span>Mikrofonpegel</span><span id="teachLvlTxt"></span></label>
      <div class="bar" id="teachLvl"><i></i><b></b></div>
    </div>
    <div id="teachResult"></div>
    <p class="hint" id="teachHint" style="text-align:left"></p>
    <div class="row">
      <button class="primary hidden" id="finishBtn" onclick="post('/api/teach/finish')">Fertig</button>
      <button class="primary hidden" id="saveBtn" onclick="saveTeach('replace')">Profil speichern</button>
      <button class="primary hidden" id="addBtn" onclick="saveTeach('add')">Profil ergänzen</button>
      <button class="hidden" id="retryBtn" onclick="startTeach(teachMode)">Wiederholen</button>
      <button class="danger" id="cancelBtn" onclick="post('/api/teach/cancel')">Abbrechen</button>
    </div>
  </section>

  <!-- Historie ------------------------------------------------------------ -->
  <section class="card">
    <h2>Letzte Bezüge</h2>
    <table id="history"></table>
    <div class="row" style="margin-top:10px"><button onclick="post('/api/history/clear')">Historie löschen</button></div>
  </section>

  <!-- Profile ------------------------------------------------------------- -->
  <section class="card">
    <h2>Profile</h2>
    <small style="color:var(--muted)">Jeder Platz speichert Pumpe (Pegel), Mühlen, Störgeräusche, Einstellungen und
      die KI-Trainingsdaten. Leere Plätze nutzen das Standardprofil (erkennt gleichmäßige laute Geräusche – Teach
      empfohlen).</small>
    <table id="slotTable" style="margin-top:8px"></table>
    <div class="field">
      <label>Profil-Datei importieren in Platz
        <select id="importSlot" style="background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:6px"></select></label>
      <input type="file" id="importFile" accept=".json" style="width:100%;color:var(--muted)" onchange="readImportFile()">
      <div id="importParts" class="hidden" style="margin-top:8px">
        <small style="color:var(--muted)" id="importInfo"></small>
        <div class="field"><label><span><input type="checkbox" id="impPump"> Pumpe + Einstellungen + Profilname</span></label></div>
        <div class="field"><label><span><input type="checkbox" id="impGrinders"> Mühlen</span></label></div>
        <div class="field"><label><span><input type="checkbox" id="impNoise"> Störgeräusche</span></label></div>
        <div class="field"><label><span><input type="checkbox" id="impTraining"> KI-Trainingsdaten der gewählten Teile</span></label></div>
        <small style="color:var(--muted)">Nur die angehakten Teile werden im gewählten Platz ersetzt – alles andere bleibt
          unverändert.</small>
        <div class="row" style="margin-top:6px"><button class="primary" onclick="importProfile()">Importieren</button></div>
      </div>
    </div>
  </section>

  <!-- Waage ---------------------------------------------------------------- -->
  <section class="card">
    <h2>Waage (Bluetooth)</h2>
    <div class="kv" id="scaleInfo"></div>
    <div class="field">
      <label>Gekoppelte Waage <b id="scBound"></b></label>
      <div class="row">
        <button onclick="searchScales()" id="scSearchBtn">Waagen suchen</button>
        <button onclick="bindCurrentScale()" id="scBindCurrent" class="hidden">Diese Waage merken</button>
        <button onclick="unbindScale()" id="scUnbind" class="hidden">Kopplung lösen</button>
      </div>
      <table id="scFound" style="margin-top:6px"></table>
      <small>Gekoppelt verbindet sich der Coffee Timer nur noch mit dieser einen Waage (z. B. wenn mehrere in Reichweite
        sind). Ohne Kopplung wird die erste passende Waage genommen.</small>
    </div>
    <div class="field"><label><span><input type="checkbox" id="scEnabled"> Waage verwenden (Bluetooth)</span></label></div>
    <div class="field"><label><span><input type="checkbox" id="scAutoTare"> Beim Pumpenstart automatisch tarieren</span></label></div>
    <div class="field"><label><span><input type="checkbox" id="scSyncTimer"> Timer der Waage mit der Pumpe starten/stoppen</span></label></div>
    <div class="field"><label><span><input type="checkbox" id="scGrindTare"> Beim Mahlen auf −Siebträger tarieren (gemahlenen Kaffee wiegen)</span></label></div>
    <div class="field">
      <label>Gewicht leerer Siebträger</label>
      <div class="row"><input type="number" id="scPortafilter" min="0" max="2000" step="0.1" style="width:110px"> g
        <button onclick="learnPortafilter()">Jetzt wiegen</button></div>
      <small>Leeren Siebträger auf die Waage legen und „Jetzt wiegen“ tippen – oder Gewicht eintragen.</small>
    </div>
    <div class="row">
      <button class="primary" onclick="saveScale()">Übernehmen</button>
      <button onclick="post('/api/scale/tare')">Jetzt tarieren</button>
    </div>
    <small style="color:var(--muted)">Unterstützt: Felicita (Arc, Incline, Parallel), Acaia (Lunar, Pearl, Pyxis, Cinco …),
      Bookoo Themis, Decent Scale. Waage einschalten – sie wird automatisch gefunden. Das Endgewicht (inkl. Nachtropfen)
      wird in der Historie gespeichert.<br>
      Mühlen-Tara: jede Mühle einmal über „Einlernen → Mühle“ lernen (bis zu 3 Mühlen). Läuft eine davon, wird die
      leere Waage tariert und zeigt −Siebträgergewicht; mit dem gefüllten Siebträger darauf steht dort der gemahlene Kaffee.
      Die Dosis landet beim nächsten Bezug in der Historie.</small>
  </section>

  <!-- Vibration ---------------------------------------------------------- -->
  <section class="card" id="vibCard">
    <h2>Erkennung über Vibration</h2>
    <div class="kv" id="vibInfo"></div>
    <div class="meter">
      <label><span>Vibration über Ruhepegel</span><span id="vibTxt"></span></label>
      <div class="bar" id="vibBar"><i></i><b></b></div>
    </div>
    <div class="field">
      <label>Pumpe erkennen über</label>
      <select id="vibMode" style="background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:6px;padding:6px;width:100%">
        <option value="0">Mikrofon (Geräusch)</option>
        <option value="1">Vibration (CoreS3 steht auf der Maschine)</option>
        <option value="2">Mikrofon + Vibration (Start: beides, danach genügt eines)</option>
      </select>
    </div>
    <div class="field">
      <label>Vibrations-Schwelle <b id="v_vib"></b></label>
      <input type="range" id="vibTh" min="2" max="30" step="1">
      <small>So viel muss die Vibration über dem automatisch gemessenen Ruhepegel liegen. Pumpe laufen lassen und
        den Balken beobachten: Schwelle etwa in die Mitte zwischen Ruhe und laufender Pumpe legen.</small>
    </div>
    <div class="row"><button class="primary" onclick="saveVib()">Übernehmen</button></div>
    <small style="color:var(--muted)">Der CoreS3 misst mit seinem Beschleunigungssensor das Zittern der Maschine. Dazu muss er
      fest auf der Maschine stehen (z. B. Tassenablage). Unabhängig von Raumgeräuschen und der Mühle; „Mikrofon + Vibration“
      ist am zuverlässigsten. Hinweis: Auch eine Nachfüllpumpe (Zweikreiser) vibriert.</small>
  </section>

  <!-- Mikrofon ------------------------------------------------------------ -->
  <section class="card">
    <h2>Mikrofon</h2>
    <div class="field">
      <label>Verstärkung <b id="v_gain"></b></label>
      <input type="range" id="micGain" min="1" max="64" step="1">
      <small>Standard 16. Bei Übersteuerung (Pumpe sehr laut / Gerät nah) verringern, bei leiser Pumpe erhöhen.
        Gelernte Profile werden automatisch umgerechnet – kein neuer Teach nötig.</small>
    </div>
    <div id="clipWarn" class="hint" style="text-align:left;color:var(--err)"></div>
    <div class="row"><button class="primary" onclick="saveMic()">Verstärkung übernehmen</button></div>
  </section>

  <!-- Einstellungen ------------------------------------------------------- -->
  <section class="card">
    <h2>Erkennung</h2>
    <div class="kv" id="profileInfo"></div>
    <div class="field" style="margin-top:8px">
      <label><span>KI-Erkennung</span><b id="clsState"></b></label>
      <div class="kv" id="clsInfo"></div>
      <small>Lernt aus allen Teach-Aufnahmen dieses Profils, worin sich Pumpe, Mühlen, Störgeräusche und Ruhe
        unterscheiden, und entscheidet damit, was gerade zu hören ist. Pegel, Gleichmäßigkeit und Vibration kommen
        dazu. Je Geräusch werden die letzten 5 Aufnahmen behalten; je mehr Geräusche eingelernt sind, desto besser.</small>
    </div>
    <table id="partsList" style="margin-top:8px"></table>
    <small style="color:var(--muted)">Mühlen lösen die Mühlen-Tara aus, Störgeräusche werden ignoriert. Löschen entfernt
      auch ihre KI-Trainingsdaten.</small>
    <div class="row" style="margin-top:10px"><button id="noiseClearBtn" onclick="clearNoise()">Alle Störgeräusche löschen</button></div>
    <div class="field">
      <label>Pegel-Schwelle <b id="v_lvl"></b></label>
      <input type="range" id="levelThreshold" min="0.05" max="1" step="0.05">
      <small>Anteil des Abstands Hintergrund → leiser Teil der Pumpe. Niedriger = empfindlicher.</small>
    </div>
    <div class="field">
      <label>Einschaltverzögerung <b id="v_on"></b></label>
      <input type="range" id="onDelayMs" min="50" max="2000" step="50">
      <small>Wird automatisch vom Timer abgezogen (Start rückdatiert).</small>
    </div>
    <div class="field">
      <label>Ausschaltverzögerung <b id="v_off"></b></label>
      <input type="range" id="offDelayMs" min="100" max="5000" step="100">
      <small>Überbrückt kurze Aussetzer, z. B. bei Pre-Infusion mit Pause.</small>
    </div>
    <div class="field">
      <label>Mindestdauer für Historie <b id="v_min"></b></label>
      <input type="range" id="minShotSeconds" min="0" max="30" step="1">
      <small>Kürzeres Pumpen (Spülen) wird nicht gespeichert.</small>
    </div>
    <div class="row"><button class="primary" onclick="saveSettings()">Einstellungen speichern</button></div>
  </section>

  <!-- WLAN ---------------------------------------------------------------- -->
  <section class="card">
    <h2>WLAN</h2>
    <div class="kv" id="netInfo"></div>
    <details style="margin-top:10px">
      <summary>WLAN konfigurieren</summary>
      <div style="margin-top:10px">
        <input type="text" id="ssid" placeholder="WLAN-Name (SSID)" autocomplete="off">
        <input type="password" id="pass" placeholder="Passwort">
        <div class="row"><button class="primary" onclick="saveWifi()">Speichern &amp; neu starten</button></div>
        <small style="color:var(--muted)">Leere SSID = eigenes WLAN „CoffeeTimer-…“ (Access-Point).</small>
      </div>
    </details>
  </section>
  <!-- Diagnose ------------------------------------------------------------ -->
  <section class="card">
    <h2>Diagnose</h2>
    <small style="color:var(--muted)">Profil, Schwellen und Messwerte der letzten 10 Erkennungen.
      Bei Fehlerkennung (z. B. Mühle): Mühle laufen lassen, dann „Bericht laden“ und kopieren.</small>
    <textarea id="diagText" readonly rows="10" style="width:100%;margin:10px 0;background:var(--bg);color:var(--text);
      border:1px solid var(--line);border-radius:8px;font:12px ui-monospace,Menlo,monospace;padding:8px"></textarea>
    <div class="row">
      <button onclick="loadDiag()">Bericht laden</button>
      <button class="primary" onclick="copyDiag()">Kopieren</button>
    </div>
  </section>

  <!-- Firmware-Update ----------------------------------------------------- -->
  <section class="card">
    <h2>Firmware-Update</h2>
    <div class="kv"><span>Installiert: <b id="fwVersion">–</b></span></div>
    <div style="margin-top:10px">
      <input type="file" id="fwFile" accept=".bin" style="width:100%;margin-bottom:8px;color:var(--muted)">
      <div class="progress hidden" id="fwProgWrap"><i id="fwProg"></i></div>
      <div class="row"><button class="primary" id="fwBtn" onclick="uploadFirmware()">Firmware hochladen</button></div>
      <small style="color:var(--muted)">Datei: <code>.pio/build/m5stack-cores3/firmware.bin</code> (nach „Build“ in PlatformIO).
        Bei Fehlern startet weiter die bisherige Firmware.</small>
    </div>
  </section>
</main>
<div id="toast" class="toast"></div>

<script>
const $ = id => document.getElementById(id);
let settingsLoaded = false;
let profileValid = false;
let teachMode = 'pump';
let profileDefault = true;
let grinderNames = [];  // gelernte Mühlen
let maxGrinders = 3;    // vom Gerät (profile.maxGrinders)
let lastSlot = -1;
let micLoaded = false;
let scaleLoaded = false;
// Große Anzeige: 'auto' | 'timer' | 'weight'
let bigView = 'auto', bigLastDose = false, bigLastRunning = false;
let bigShowsWeight = false, weightKnown = false;
function toggleBigView() {
  if (!weightKnown) return;
  bigView = bigShowsWeight ? 'timer' : 'weight';
  poll();  // sofort neu zeichnen
}
let vibLoaded = false;
let lastOk = Date.now();

// --- Hilfsfunktionen -------------------------------------------------------
function toast(msg) {
  const t = $('toast'); t.textContent = msg; t.classList.add('show');
  clearTimeout(t._h); t._h = setTimeout(() => t.classList.remove('show'), 2500);
}
async function post(url, body) {
  try {
    const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                 body: body ? JSON.stringify(body) : '{}' });
    const j = await r.json();
    if (j.message) toast(j.message);
    poll();
    return j;
  } catch (e) { toast('Fehler: ' + e); }
}
const fmtSec = ms => (ms / 1000).toFixed(1);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

/** Balken setzen: value/threshold jeweils 0..1 */
function setBar(id, value, threshold, active) {
  const el = $(id);
  el.querySelector('i').style.width = Math.max(0, Math.min(1, value)) * 100 + '%';
  const m = el.querySelector('b');
  if (threshold == null) m.style.display = 'none';
  else { m.style.display = ''; m.style.left = Math.min(1, threshold) * 100 + '%'; }
  el.classList.toggle('active', !!active);
}

// --- Darstellung -----------------------------------------------------------
function render(s) {
  profileValid = s.profile.valid;
  profileDefault = s.profile.isDefault;
  grinderNames = s.profile.grinders || [];
  maxGrinders = s.profile.maxGrinders || maxGrinders;
  // Status-Pille
  const st = $('status');
  let txt = 'Bereit', on = false;
  if (s.teach.phase !== 'idle') txt = 'Teach';
  else if (!s.profile.valid) txt = 'Kein Profil';
  else if (s.pumpOn) { txt = 'Pumpe läuft'; on = true; }
  else if (s.live.grinderOn) txt = (s.live.grinderName || 'Mühle') + ' läuft';
  else if (s.live.rejected) txt = 'Störgeräusch – ignoriert';
  st.textContent = txt; st.classList.toggle('on', on);

  // Akku (Farbe wie am Display: grün > 50 %, gelb > 20 %, sonst rot)
  const b = s.battery || { level: -1 };
  const bEl = $('battery');
  bEl.textContent = (b.charging ? '⚡ ' : '🔋 ') + (b.level >= 0 ? b.level + ' %' : '–');
  bEl.style.color = b.level < 0 ? 'var(--muted)' : b.level > 50 ? 'var(--ok)' : b.level > 20 ? 'var(--warn)' : 'var(--err)';

  // Timer
  // Große Anzeige wie am Display: automatisch beim Mahlen der gemahlene Kaffee, sonst
  // der Timer; Klick schaltet um, Beginn/Ende von Mahlen oder Bezug -> wieder Automatik.
  const t = $('timer');
  if (s.scale.doseMode !== bigLastDose || s.running !== bigLastRunning) {
    bigView = 'auto';
    bigLastDose = s.scale.doseMode;
    bigLastRunning = s.running;
  }
  weightKnown = s.scale.enabled && s.scale.weight !== undefined;
  const autoWeight = s.scale.doseMode && !s.running;
  bigShowsWeight = weightKnown && (bigView === 'weight' || (bigView === 'auto' && autoWeight));
  t.innerHTML = bigShowsWeight
    ? s.scale.weight.toFixed(1) + `<small>g${s.scale.doseMode ? ' Kaffee gemahlen' : ''}</small>`
    : fmtSec(s.timerMs) + '<small>s</small>';
  t.classList.toggle('running', s.running);
  t.classList.toggle('dose', bigShowsWeight);
  t.style.cursor = weightKnown ? 'pointer' : '';
  $('hint').textContent = !s.profile.saveOk ? '⚠ Speichern fehlgeschlagen – Profil geht beim Neustart verloren!'
                        : s.running ? 'Bezug läuft …' : '';
  $('profileLine').textContent = `Profil ${s.profile.slot + 1}: ${s.profile.name}` +
    (s.profile.isDefault ? ' (Standardprofil – Teach empfohlen)' : '');
  if (s.profile.slot !== lastSlot) { lastSlot = s.profile.slot; settingsLoaded = false; loadProfiles(); }

  // Waage
  const sc = s.scale;
  $('weightLine').classList.toggle('hidden', !sc.enabled);
  // Steht das Gewicht groß oben, zeigt diese Zeile stattdessen die Zeit.
  $('weight').textContent = sc.weight === undefined ? 'Waage ' + sc.state
    : bigShowsWeight ? (sc.doseMode && !s.running ? 'Kaffee gemahlen · Bezug ' : '') + fmtSec(s.timerMs) + ' s'
    : (sc.doseMode ? 'Kaffee gemahlen ' : '') + sc.weight.toFixed(1) + ' g';
  $('flow').textContent = sc.weight !== undefined && s.running ? sc.flow.toFixed(1) + ' g/s'
    : sc.dose !== undefined ? `Dosis ${sc.dose.toFixed(1)} g` : '';
  renderScaleBinding(sc);
  $('scaleInfo').innerHTML = `<span>Status: <b>${esc(sc.state)}</b></span>` +
    (sc.connected ? `<span>Gerät: <b>${esc(sc.name)}</b> (${esc(sc.model)})</span>` +
                    `<span>Akku: <b>${sc.battery >= 0 ? sc.battery + ' %' : '–'}</b></span>` : '') +
    `<span>Siebträger: <b>${sc.portafilterG.toFixed(1)} g</b></span>`;
  if (!scaleLoaded) {
    $('scEnabled').checked = sc.enabled; $('scAutoTare').checked = sc.autoTare; $('scSyncTimer').checked = sc.syncTimer;
    $('scGrindTare').checked = sc.grindTare; $('scPortafilter').value = sc.portafilterG.toFixed(1);
    scaleLoaded = true;
  }

  // Vibration
  const v = s.vibration;
  $('vibCard').classList.toggle('hidden', !v.available);
  if (v.available) {
    const over = v.levelDb - v.baselineDb;
    setBar('vibBar', over / (2 * v.thresholdDb), 0.5, v.active);
    $('vibTxt').textContent = (over >= 0 ? '+' : '') + over.toFixed(1) + ' dB';
    $('vibInfo').innerHTML = `<span>Status: <b>${v.mode === 0 ? 'nicht verwendet' : v.active ? 'Vibration erkannt' : 'Ruhe'}</b></span>` +
      `<span>Pegel: <b>${v.levelDb.toFixed(1)} dB</b></span><span>Ruhepegel: <b>${v.baselineDb.toFixed(1)} dB</b></span>` +
      `<span>Schwankung: <b>${v.steadiness >= 20 ? '–' : '±' + v.steadiness.toFixed(1) + ' dB'}</b></span>`;
    if (!vibLoaded) { $('vibMode').value = v.mode; $('vibTh').value = v.thresholdDb; updateVibLabel(); vibLoaded = true; }
  }

  // Mikrofon
  if (!micLoaded) { $('micGain').value = s.mic.gain; updateGainLabel(); micLoaded = true; }
  $('clipWarn').textContent = s.mic.clip > 0.001
    ? `Übersteuerung: ${(s.mic.clip * 100).toFixed(1)} % der Samples am Anschlag – Verstärkung verringern.` : '';

  // Live-Messwerte (Schwellen aus den Einstellungen)
  const lv = s.live, set = s.settings;
  setBar('lvlBar', lv.levelRatio, set.levelThreshold, lv.levelRatio >= set.levelThreshold);
  $('lvlTxt').textContent = lv.rmsDb.toFixed(1) + ' dBFS';
  // Schwankung 0..6 dB; grün, solange unter der gelernten Grenze
  const sTh = s.profile.steadyThreshold || 3;
  setBar('stdBar', Math.min(lv.steadiness, 6) / 6, sTh / 6, lv.steadiness <= sTh);
  $('stdTxt').textContent = lv.steadiness >= 20 ? '–' : '±' + lv.steadiness.toFixed(1) + ' dB';
  // KI: Wahrscheinlichkeit "Pumpe", Marke = Schwelle; Text = was gerade erkannt wird
  const c = s.classifier || {};
  $('clsMeter').classList.toggle('hidden', !(c.ready && c.pumpKnown));
  if (c.kind) {
    setBar('clsBar', c.pump, c.minProb, c.pump >= c.minProb);
    $('clsTxt').textContent = `${Math.round(c.pump * 100)} %  ·  jetzt: ${clsName(c)} (${Math.round(c.prob * 100)} %)`;
  }
  document.querySelectorAll('.teachStart').forEach(b => {
    const m = b.dataset.mode;
    b.disabled = s.teach.phase !== 'idle';
  });

  renderTeach(s.teach);
  renderHistory(s.history);

  // Profil / Netzwerk
  const p = s.profile;
  $('profileInfo').innerHTML = p.isDefault
    ? '<span>Pumpe: <b>Standardprofil</b> – Teach empfohlen</span>'
    : `<span>Pumpe: <b>${p.recordings} Aufnahme(n)</b></span>` +
      `<span>Qualität: <b>${p.quality.toFixed(0)}/100</b></span>` +
      `<span>Hintergrund: <b>${p.bgDb.toFixed(1)} dB</b></span><span>Pumpe (leise): <b>${p.pumpDb.toFixed(1)} dB</b></span>` +
      `<span>Max. Schwankung: <b>±${p.steadyThreshold.toFixed(1)} dB</b></span>` +
      `<span>Störgeräusche: <b>${p.noiseCount}</b></span>`;
  $('noiseClearBtn').disabled = !p.noiseCount;
  renderParts(p);
  // KI-Erkennung
  $('clsState').textContent = !c.ready ? 'noch nicht trainiert' : `trainiert · Trefferquote ${(c.accuracy * 100).toFixed(0)} %`;
  $('clsInfo').innerHTML = c.ready
    ? `<span>Klassen: <b>${c.classes}</b></span><span>Klangbilder: <b>${c.frames}</b></span>` +
      (c.kind ? `<span>Jetzt: <b>${esc(clsName(c))} (${(c.prob * 100).toFixed(0)} %)</b></span>` : '') +
      (c.pumpKnown ? '' : '<span>Pumpe noch nicht eingelernt</span>')
    : '<span>Pumpe (und Mühlen/Störgeräusche) einlernen – jede Aufnahme trainiert die KI.</span>';
  $('netInfo').innerHTML = `<span>Modus: <b>${s.net.ap ? 'Access-Point' : 'WLAN'}</b></span>` +
    `<span>Netz: <b>${esc(s.net.name)}</b></span><span>IP: <b>${esc(s.net.ip)}</b></span>`;

  $('fwVersion').textContent = s.firmware || '–';
  if (!settingsLoaded) { loadSettings(set); settingsLoaded = true; }
}

function renderTeach(t) {
  const card = $('teachCard');
  card.classList.toggle('hidden', t.phase === 'idle');
  if (t.phase === 'idle') return;
  teachMode = t.mode;
  const noise = t.mode === 'noise' || t.mode === 'grinder';
  $('teachTitle').textContent = 'Teach-Modus: ' + { pump: 'Pumpe', noise: 'Störgeräusch', grinder: 'Mühle' }[t.mode];

  $('teachInstr').textContent = t.instruction +
    (t.phase === 'recordPump' ? `  (${(t.recordedMs / 1000).toFixed(1)} s)` : '');
  $('teachProg').style.width = (t.progress * 100) + '%';

  // Pegelanzeige -90..0 dBFS, Markierung = Hintergrund + 6 dB (Auslöseschwelle)
  const norm = db => (db + 90) / 90;
  const showBg = t.phase === 'waitPump' || t.phase === 'recordPump';
  setBar('teachLvl', norm(t.liveDb), showBg ? norm(t.bgDb + 6) : null, showBg && t.liveDb > t.bgDb + 6);
  $('teachLvlTxt').textContent = t.liveDb.toFixed(1) + ' dBFS' + (showBg ? ` (Hintergrund ${t.bgDb.toFixed(1)})` : '');

  const done = t.phase === 'done';
  const res = $('teachResult');
  if (done && t.result) {
    const r = t.result;
    const title = { ok: '✔ Aufnahme OK', warning: '⚠ Aufnahme mit Einschränkungen', error: '✖ Aufnahme nicht verwendbar' }[r.quality];
    const head = `<div class="result ${r.quality}"><h3>${title} – ${r.score}/100</h3>` +
      `<ul>${r.messages.map(m => '<li>' + esc(m) + '</li>').join('')}</ul>`;
    const kv = (label, value) => `<span>${label}: <b>${value}</b></span>`;
    const common = kv('Hörbar', r.activeSeconds.toFixed(1) + ' s') + kv('Abstand (leise)', r.snrDb.toFixed(1) + ' dB') +
      kv('Schwankung Hintergr.', '±' + r.bgStdDb.toFixed(1) + ' dB') + kv('Übersteuerung', (r.clip * 100).toFixed(2) + ' %') +
      kv('Klangbilder für die KI', r.trainFrames) +
      (r.clsReady ? kv('Bisher als Pumpe erkannt', (r.clsAsPump * 100).toFixed(0) + ' %') : '');
    res.innerHTML = head + `<details style="margin-top:8px"><summary>Messwerte</summary><div class="kv" style="margin-top:6px">` +
      common + (noise ? '' :
        kv('Längste Pause', (r.longestPauseMs / 1000).toFixed(1) + ' s') +
        kv('Schwankung Pumpe', '±' + r.pumpSteadiness.toFixed(1) + ' dB (Grenze ' + r.steadyThreshold.toFixed(1) + ')') +
        kv('→ Ausschaltverz.', r.suggestedOffDelayMs + ' ms')) +
      `</div></details></div>`;
  } else {
    res.innerHTML = '';
  }
  const usable = done && t.result && t.result.quality !== 'error';
  $('finishBtn').classList.toggle('hidden', t.phase !== 'recordPump');
  const grinderMode = t.mode === 'grinder';
  const grindersFull = grinderNames.length >= maxGrinders;
  $('saveBtn').classList.toggle('hidden', !usable || (grinderMode && grindersFull));
  $('saveBtn').textContent = grinderMode ? (grinderNames.length ? 'Als neue Mühle speichern' : 'Mühle speichern')
    : noise ? 'Störgeräusch speichern' : (profileValid ? 'Profil ersetzen' : 'Profil speichern');
  const pumpAdd = usable && profileValid && !profileDefault && t.mode === 'pump';
  const match = t.result && t.result.grinderMatch >= 0 ? grinderNames[t.result.grinderMatch] : null;
  const grinderAdd = usable && grinderMode && !!match;
  const noiseIdx = t.result && t.result.noiseMatch >= 0 ? t.result.noiseMatch : -1;
  const noiseAdd = usable && t.mode === 'noise' && noiseIdx >= 0;
  $('addBtn').classList.toggle('hidden', !(pumpAdd || grinderAdd || noiseAdd));
  $('addBtn').textContent = grinderMode ? `Zu „${match}“ hinzufügen`
    : noiseAdd ? `Zu Störgeräusch ${noiseIdx + 1} hinzufügen` : 'Profil ergänzen';
  $('teachHint').textContent = pumpAdd
    ? 'Ergänzen: z. B. Leerbezug, Rückspülen und normaler Bezug – die KI lernt alle Varianten (je 5 Aufnahmen). ' +
      'Ersetzen: verwirft die bisherigen Pumpen-Aufnahmen.'
    : grinderAdd ? 'Dieselbe Mühle (z. B. anderer Mahlgrad, leer gelaufen)? Dann hinzufügen – sonst als neue Mühle speichern.'
    : '';
  $('retryBtn').classList.toggle('hidden', !done);
  $('cancelBtn').textContent = done ? 'Verwerfen' : 'Abbrechen';
}

function renderHistory(h) {
  const tb = $('history');
  if (!h.length) { tb.innerHTML = '<tr><td style="color:var(--muted)">Noch keine Bezüge</td><td></td></tr>'; return; }
  tb.innerHTML = h.map((e, i) => {
    const when = e.epoch ? new Date(e.epoch * 1000).toLocaleTimeString('de-DE', { hour: '2-digit', minute: '2-digit' })
                         : '#' + (h.length - i);
    let g = e.g !== undefined ? `${e.g.toFixed(1)} g` : '';
    if (e.dose !== undefined) {
      g = `${e.dose.toFixed(1)} g → ${g || '?'}` + (e.g !== undefined ? ` (1:${(e.g / e.dose).toFixed(1)})` : '');
    }
    return `<tr><td>${when}</td><td style="color:var(--muted)">${g}</td><td><b>${fmtSec(e.ms)} s</b></td></tr>`;
  }).join('');
}

// --- Einstellungen ---------------------------------------------------------
const labels = {
  levelThreshold: ['v_lvl', v => Math.round(v * 100) + ' %'],
  onDelayMs: ['v_on', v => v + ' ms'],
  offDelayMs: ['v_off', v => v + ' ms'],
  minShotSeconds: ['v_min', v => v + ' s'],
};
function loadSettings(set) {
  for (const k in labels) { $(k).value = set[k]; updateLabel(k); }
}
function updateLabel(k) { const [id, f] = labels[k]; $(id).textContent = f(parseFloat($(k).value)); }
for (const k in labels) $(k).addEventListener('input', () => updateLabel(k));
function saveSettings() {
  const body = {}; for (const k in labels) body[k] = parseFloat($(k).value);
  post('/api/settings', body).then(() => toast('Einstellungen gespeichert'));
}
function startTeach(mode) { post('/api/teach/start', { mode }); }
/** Name der von der KI erkannten Klasse. */
function clsName(c) {
  if (c.kind === 'Muehle') return grinderNames[c.index] || 'Mühle ' + (c.index + 1);
  if (c.kind === 'Stoergeraeusch') return 'Störgeräusch ' + (c.index + 1);
  return { Pumpe: 'Pumpe', Ruhe: 'Ruhe' }[c.kind] || c.kind;
}
/** Liste der Mühlen und Störgeräusche mit Löschknöpfen (nur bei Änderung neu aufbauen). */
let partsKey = '';
function renderParts(p) {
  const key = `${p.slot}/${p.noiseCount}/${(p.grinders || []).join('|')}`;
  if (key === partsKey) return;
  partsKey = key;
  const btn = (fn, i, enabled) =>
    `<button style="min-width:0;padding:4px 10px" ${enabled ? '' : 'disabled'} onclick="${fn}(${i})">🗑</button>`;
  let rows = '';
  (p.grinders || []).forEach((name, i) => {
    const ren = `<button style="min-width:0;padding:4px 10px" title="Umbenennen" onclick="renameGrinder(${i})">✎</button>`;
    rows += `<tr><td>☕ ${esc(name)}</td><td style="white-space:nowrap">${ren} ${btn('deleteGrinder', i, true)}</td></tr>`;
  });
  for (let i = 0; i < p.noiseCount; ++i)
    rows += `<tr><td>Störgeräusch ${i + 1}</td><td>${btn('deleteNoise', i, true)}</td></tr>`;
  $('partsList').innerHTML = rows;
}
function deleteGrinder(i) {
  if (confirm(`„${grinderNames[i]}“ löschen?`)) post('/api/grinders/delete', { index: i });
}
function renameGrinder(i) {
  const name = prompt('Name der Mühle:', grinderNames[i]);
  if (name && name.trim()) post('/api/grinders/rename', { index: i, name: name.trim() });
}
function deleteNoise(i) {
  if (confirm(`Störgeräusch ${i + 1} löschen?`)) post('/api/noise/delete', { index: i }).then(loadProfiles);
}
function clearNoise() {
  if (confirm('Alle gelernten Störgeräusche löschen?')) post('/api/noise/clear');
}
// --- Profile -------------------------------------------------------------
async function loadProfiles() {
  try {
    const p = await (await fetch('/api/profiles', { cache: 'no-store' })).json();
    $('slotTable').innerHTML = p.slots.map((s, i) => {
      const info = (s.isDefault ? 'Pumpe: Standardprofil'
        : `Pumpe: ${s.recordings} Teach-Aufnahme(n) · Qualität ${s.quality.toFixed(0)}/100`) +
          `${s.noise ? ' · ' + s.noise + ' Störgeräusch(e)' : ''}` +
          `${s.grinders ? ' · ' + s.grinders + ' Mühle(n)' : ''}`;
      const active = i === p.active;
      return `<tr><td>${active ? '▶ ' : ''}<b>${i + 1}. ${esc(s.name)}</b><br><small style="color:var(--muted)">${info}</small></td>` +
        `<td style="white-space:nowrap">` +
        (active ? '' : `<button style="min-width:0;padding:6px 10px" onclick="slotAction('select',${i})">Aktivieren</button> `) +
        `<button style="min-width:0;padding:6px 10px" onclick="renameSlot(${i})">✎</button> ` +
        `<button style="min-width:0;padding:6px 10px" title="Als Klartext ansehen" onclick="window.open('/api/profiles/text?slot=${i}')">Klartext</button> ` +
        `<button style="min-width:0;padding:6px 10px" title="Exportieren (Datei)" onclick="location.href='/api/profiles/export?slot=${i}'">⤓</button> ` +
        `<button style="min-width:0;padding:6px 10px" onclick="deleteSlot(${i})">🗑</button></td></tr>`;
    }).join('');
    $('importSlot').innerHTML = p.slots.map((s, i) => `<option value="${i}" ${i === p.active ? 'selected' : ''}>${i + 1}. ${esc(s.name)}</option>`).join('');
  } catch (e) { /* nächster Versuch beim nächsten Wechsel */ }
}
async function slotAction(action, slot, extra) {
  await post('/api/profiles/' + action, Object.assign({ slot }, extra || {}));
  settingsLoaded = false;
  loadProfiles();
}
function renameSlot(i) {
  const name = prompt('Neuer Name für Profil ' + (i + 1) + ':');
  if (name) slotAction('rename', i, { name });
}
function deleteSlot(i) {
  if (confirm('Profil ' + (i + 1) + ' löschen? Der Platz nutzt danach das Standardprofil.')) slotAction('delete', i);
}
let pendingImport = null;  // gelesene Profil-Datei
/** Datei lesen und anzeigen, welche Teile sie enthält. */
function readImportFile() {
  const f = $('importFile').files[0];
  pendingImport = null;
  $('importParts').classList.add('hidden');
  if (!f) return;
  const reader = new FileReader();
  reader.onload = () => {
    try {
      const j = JSON.parse(reader.result);
      if (j.format !== 'coffeetimer-profile') throw 'Format';
      const has = { pump: !!j.pump, grinders: !!j.grinders, noise: !!j.noise };
      pendingImport = j;
      for (const [id, key] of [['impPump', 'pump'], ['impGrinders', 'grinders'], ['impNoise', 'noise']]) {
        $(id).checked = has[key];
        $(id).disabled = !has[key];
      }
      $('impTraining').checked = !!j.training;
      $('impTraining').disabled = !j.training;
      $('importInfo').textContent = `„${j.name}“ (Firmware ${j.firmware || '?'}) – was soll übernommen werden?`;
      $('importParts').classList.remove('hidden');
    } catch (e) { toast('Keine gültige Profil-Datei'); }
  };
  reader.readAsText(f);
}
function importProfile() {
  const j = pendingImport;
  if (!j) { toast('Bitte eine Profil-Datei (.json) wählen'); return; }
  const slot = parseInt($('importSlot').value);
  const parts = [['impPump', 'pump'], ['impGrinders', 'grinders'], ['impNoise', 'noise']]
    .filter(([id]) => $(id).checked).map(([, key]) => key);
  if (!parts.length) { toast('Nichts ausgewählt'); return; }
  const names = { pump: 'Pumpe', grinders: 'Mühlen', noise: 'Störgeräusche' };
  if (!confirm(`In Platz ${slot + 1} ersetzen: ${parts.map(p => names[p]).join(', ')}?\nAlles andere bleibt unverändert.`)) return;
  slotAction('import', slot, { parts, pump: j.pump, grinders: j.grinders, noise: j.noise,
                               training: j.training, withTraining: $('impTraining').checked });
  $('importParts').classList.add('hidden');
  $('importFile').value = '';
  pendingImport = null;
}

// --- Mikrofon --------------------------------------------------------------
function updateGainLabel() { $('v_gain').textContent = $('micGain').value + '×'; }
$('micGain').addEventListener('input', updateGainLabel);
function saveScale() {
  post('/api/scale', { enabled: $('scEnabled').checked, autoTare: $('scAutoTare').checked,
                       syncTimer: $('scSyncTimer').checked, grindTare: $('scGrindTare').checked,
                       portafilterG: parseFloat($('scPortafilter').value) || 0 });
}
function searchScales() { post('/api/scale/search'); }
function bindScale(addr, name) { post('/api/scale/bind', { addr, name }); }
function bindCurrentScale() { bindScale(lastScale.address, lastScale.name); }
function unbindScale() { post('/api/scale/unbind'); }
let lastScale = {}, foundKey = '';
/** Kopplung und Suchergebnisse anzeigen (Liste nur bei Änderung neu aufbauen). */
function renderScaleBinding(sc) {
  lastScale = sc;
  const bound = sc.boundAddr || '';
  $('scBound').textContent = bound ? `${sc.boundName || 'Waage'} (${bound})` : 'keine – erste passende Waage';
  $('scUnbind').classList.toggle('hidden', !bound);
  $('scBindCurrent').classList.toggle('hidden', !sc.connected || !!bound);
  $('scSearchBtn').disabled = !!sc.searching;
  $('scSearchBtn').textContent = sc.searching ? 'Suche läuft …' : 'Waagen suchen';
  const list = sc.found || [];
  const key = JSON.stringify(list) + bound;
  if (key === foundKey) return;
  foundKey = key;
  $('scFound').innerHTML = list.map((f, i) => {
    const isBound = bound && f.addr.toLowerCase() === bound.toLowerCase();
    return `<tr><td><b>${esc(f.name)}</b><br><small style="color:var(--muted)">${esc(f.addr)} · ${f.rssi ? f.rssi + ' dBm' : 'verbunden'}</small></td>` +
      `<td>${isBound ? '✓ gekoppelt'
        : `<button style="min-width:0;padding:6px 10px" onclick="bindFound(${i})">Koppeln</button>`}</td></tr>`;
  }).join('');
}
/** Waage aus der Suchliste koppeln (Index statt Name im HTML: keine Probleme mit Sonderzeichen). */
function bindFound(i) {
  const f = (lastScale.found || [])[i];
  if (f) bindScale(f.addr, f.name);
}
function learnPortafilter() {
  post('/api/scale/portafilter').then(j => { if (j && j.ok) scaleLoaded = false; });
}
function updateVibLabel() { $('v_vib').textContent = '+' + $('vibTh').value + ' dB'; }
$('vibTh').addEventListener('input', updateVibLabel);
function saveVib() {
  post('/api/vibration', { mode: parseInt($('vibMode').value), thresholdDb: parseFloat($('vibTh').value) });
}
function saveMic() { post('/api/mic', { gain: parseInt($('micGain').value) }); }

function saveTeach(mode) {
  // Ausschaltverzögerung wird beim Speichern ggf. verlängert -> Formular neu laden
  post('/api/teach/save', { mode }).then(() => { settingsLoaded = false; });
}
async function loadDiag() {
  try { $('diagText').value = await (await fetch('/api/diag', { cache: 'no-store' })).text(); }
  catch (e) { toast('Fehler: ' + e); }
}
/** Kopieren – navigator.clipboard ist über http meist gesperrt, daher execCommand als Fallback. */
async function copyDiag() {
  const ta = $('diagText');
  if (!ta.value) await loadDiag();
  try { await navigator.clipboard.writeText(ta.value); }
  catch (e) { ta.select(); document.execCommand('copy'); }
  toast('Bericht kopiert');
}
/** Lädt firmware.bin hoch (XHR wegen Fortschrittsanzeige). */
function uploadFirmware() {
  const f = $('fwFile').files[0];
  if (!f) { toast('Bitte zuerst firmware.bin auswählen'); return; }
  if (!f.name.endsWith('.bin')) { toast('Nur .bin-Dateien'); return; }
  if (!confirm(`Firmware "${f.name}" (${(f.size / 1024).toFixed(0)} kB) installieren?`)) return;
  const fd = new FormData(); fd.append('firmware', f, f.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/update');
  xhr.setRequestHeader('X-File-Size', f.size);
  $('fwProgWrap').classList.remove('hidden'); $('fwBtn').disabled = true;
  xhr.upload.onprogress = e => { if (e.lengthComputable) $('fwProg').style.width = (e.loaded / e.total * 100) + '%'; };
  xhr.onload = () => {
    let msg = 'Fehler'; try { msg = JSON.parse(xhr.responseText).message; } catch (e) {}
    toast(msg); $('fwBtn').disabled = false;
    if (xhr.status === 200) setTimeout(() => location.reload(), 8000);
  };
  xhr.onerror = () => { toast('Upload fehlgeschlagen'); $('fwBtn').disabled = false; };
  xhr.send(fd);
}
function saveWifi() {
  if (!confirm('WLAN-Daten speichern und Gerät neu starten?')) return;
  post('/api/wifi', { ssid: $('ssid').value, password: $('pass').value });
}

// --- Abfrage-Schleife --------------------------------------------------------
let busy = false;
async function poll() {
  if (busy) return; busy = true;
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    render(await r.json());
    lastOk = Date.now();
  } catch (e) { /* Verbindung kurz weg – nächster Versuch */ }
  busy = false;
  $('offline').classList.toggle('hidden', Date.now() - lastOk < 3000);
}
setInterval(poll, 250);
poll();
</script>
</body>
</html>
)HTML";
