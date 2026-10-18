# Coffee Timer für Siebträgermaschinen (M5Stack CoreS3)

Ein Shot-Timer, der **ohne Eingriff in die Maschine** erkennt, wann die Pumpe läuft – am Geräusch und optional
an der Vibration. Er misst die Bezugszeit, arbeitet mit gängigen **Bluetooth-Kaffeewaagen** zusammen und wiegt
beim Mahlen automatisch den **gemahlenen Kaffee** ab. Bedienung am Touch-Display und parallel über eine Webseite.

## Funktionen

- **Automatischer Shot-Timer:** startet und stoppt mit der Pumpe – erkannt über das Mikrofon, den
  Beschleunigungssensor (Gerät steht auf der Maschine) oder beides zusammen.
- **Einlernen statt Einstellen:** Pumpe, Mühlen und Störgeräusche (z. B. Milchaufschäumer) werden jeweils
  einmal aufgenommen. Jede Aufnahme ist eigenständig – die Reihenfolge ist egal.
- **Bluetooth-Waagen:** Felicita (Arc, Incline, Parallel), Acaia (Lunar, Pearl, Pyxis, Cinco …),
  Bookoo Themis, Decent Scale. Gewicht und Durchfluss live, Endgewicht in der Historie.
- **Mühlen-Tara:** Läuft eine eingelernte Mühle, wird die leere Waage auf *−Siebträgergewicht* tariert. Legst du
  danach den gefüllten Siebträger auf, steht dort direkt der gemahlene Kaffee – groß auf dem Display. Die Dosis wird dem
  nächsten Bezug zugeordnet (Historie: `18,0 g → 36,2 g (1:2,0)`).
- **Bis zu 3 Mühlen** und 3 Störgeräusche je Profil, **5 Profile** (z. B. verschiedene Maschinen).
- **Uhrzeit** per NTP, gespeichert in der eingebauten RTC (läuft auch ohne WLAN weiter).
- **Webseite** mit allen Einstellungen, Historie, Profil-Export/-Import, Diagnosebericht und Firmware-Update.
- **Akkuanzeige**, Firmware-Update über WLAN, Access-Point zur Ersteinrichtung.

## Hardware

- **M5Stack CoreS3** (ESP32-S3, Touch-Display, Mikrofon, Beschleunigungssensor, RTC, Akku)
- optional eine der oben genannten Bluetooth-Waagen

## Installation

1. [PlatformIO](https://platformio.org/) installieren (z. B. als Erweiterung für VS Code).
2. Repository klonen und den CoreS3 per USB anschließen.
3. Bauen und flashen: `pio run -t upload`
   (klappt die Verbindung nicht: Reset-Taste ca. 2 s halten, bis die grüne LED leuchtet → Download-Modus).
4. Ohne WLAN-Daten öffnet das Gerät den Access-Point **CoffeeTimer-XXXX** (offen). Damit verbinden,
   `http://192.168.4.1` öffnen, unter *WLAN* die Zugangsdaten eintragen → das Gerät startet neu.
5. Danach ist die Webseite unter `http://coffeetimer.local` bzw. der IP oben im Display erreichbar.

Nur 2,4-GHz-WLAN. Ist das Netz nicht erreichbar, öffnet das Gerät nach 15 s wieder den Access-Point und
versucht die Verbindung im Hintergrund weiter.

## Erste Schritte

1. Gerät nah an die Maschine stellen (die Pumpe sitzt meist unten/hinten) – für die Vibrationserkennung
   **auf** die Maschine, z. B. auf die Tassenablage.
2. **TEACH → PUMPE:** einen normalen Bezug einlernen.
3. **TEACH → MÜHLE:** jede Mühle einmal mahlen lassen (für die Mühlen-Tara und damit sie nicht als Pumpe gilt).
4. Optional **TEACH → STÖRGER.** für weitere Geräusche wie den Milchaufschäumer.
5. Optional Waage einschalten, Siebträger wiegen und *Mühlen-Tara* aktivieren (siehe [Waage](#waage)).

Ohne Teach arbeitet ein **Standardprofil**: Es erkennt jedes gleichmäßige Geräusch deutlich über dem
automatisch gemessenen Raumpegel. Das ist nur eine Notlösung – zuverlässig wird es mit dem Teach.

## Bedienung am Display

**Hauptbildschirm**

- Statuszeile: *Bereit*, *Pumpe läuft*, *<Mühle> läuft*, *Störgeräusch – ignoriert*; rechts das aktive Profil.
- Große Zahl: der Timer – beim Mahlen automatisch der gemahlene Kaffee. **Antippen** schaltet zwischen Timer und
  Gewicht um; beginnt oder endet das Mahlen bzw. ein Bezug, gilt wieder die Automatik.
- Waagenzeile mit Gewicht bzw. Zeit und Durchfluss.
- Messbalken der Erkennung (Pegel, Takt/Rhythmus bzw. Klangmuster, ggf. Vibration).
- **TEACH** · **RESET** · **INFO**
- **RESET** ohne Waage: Bezugszeit auf 0. Mit verbundener Waage öffnet sich eine Auswahl: **ALLES AUF 0**
  oder einzeln **BEZUGSZEIT** (Zeit am Display), **GEWICHT** (Waage tarieren) und **WAAGEN-ZEIT** (Timer an der
  Waage).

**INFO** zeigt WLAN, Adresse, Profil und Kennwerte; **EMPF − / +** ändert die Empfindlichkeit.
**EINST.** enthält alle Einstellungen: Zeile antippen, mit **− / +** ändern, unten auf „< Seite >“ blättern.
Lösch-Aktionen erst nach zweitem Tippen („SICHER?“). Änderungen wirken sofort und werden kurz danach gespeichert.

## Einlernen (Teach)

Jede Aufnahme beginnt mit **3 s Ruhe** – dabei wird der Hintergrund gemessen. Jedes Geräusch speichert seinen
eigenen Hintergrund, Pegel, seine eigene Schwelle und die Mikrofon-Verstärkung; spätere Aufnahmen anderer
Geräusche ändern daran nichts.

| Modus | Ablauf | Ergebnis |
|---|---|---|
| **Pumpe** | Ruhe → normalen Bezug starten (am besten mit Kaffee) → endet automatisch, wenn die Pumpe 4 s still ist (oder **FERTIG**) | **Ersetzen** oder **Ergänzen** (z. B. Leerbezug + echter Bezug) |
| **Mühle** | Ruhe → mahlen lassen → ausschalten | **Neue Mühle** oder **Ergänzen** der ähnlichsten Mühle (anderer Mahlgrad, leer gelaufen) |
| **Störgeräusch** | Ruhe → Gerät einschalten → ausschalten | wird künftig ignoriert |

Die Auswertung bewertet jede Aufnahme (0–100 Punkte, OK / Warnung / Fehler) mit Begründung und Tipps – etwa
bei Übersteuerung, zu leiser Pumpe oder unruhigem Hintergrund – und setzt passende Schwellen automatisch.

## Erkennung

Einstellbar unter *EINST. → Erkennung* bzw. auf der Webseite:

| Modus | Funktionsweise |
|---|---|
| **Mikrofon** | Klang und Rhythmus der Pumpe |
| **Vibration** | Beschleunigungssensor, 400 Messungen/s; CoreS3 muss fest auf der Maschine stehen |
| **Mikrofon + Vibration** | Start nur, wenn beides passt; läuft die Pumpe, genügt eines (unter Druck wird sie oft leiser) – am zuverlässigsten |

**Mikrofon:** Alle 32 ms wird ein 64-ms-Fenster per FFT ausgewertet:

- **Klangmuster** – 40 Frequenzbänder, verglichen wird die Klangfarbe, nicht die Lautstärke
  (gut für Rotationspumpen).
- **Takt und Rhythmus** – Vibrationspumpen schlagen im Netztakt (50 Hz). Dieser Rhythmus bleibt auch unter
  Druck gleich und unterscheidet sie von netzbrummenden Motoren.
- **Gleichmäßigkeit** – Niesen, Sprechen und Klappern schwanken stärker als eine Pumpe.
- Klingt ein Geräusch einer **Mühle** oder einem **Störgeräusch** ähnlicher als der Pumpe, wird es ignoriert.
  Solange eine Mühle läuft (und kurz danach), startet keine Pumpe.

**Vibration:** Gemessen wird die Vibrationsstärke über einem automatisch nachgeführten Ruhepegel. Kurze Stöße
(Tasse abstellen) zählen nicht. Die Schwelle lässt sich auf der Webseite anhand eines Live-Balkens einstellen.
Hinweis: Bei Zweikreisern vibriert auch die Nachfüllpumpe – dann besser *Mikrofon + Vibration*.

Der Timer-Start wird auf den Beginn des Geräuschs zurückdatiert. Als Sicherung stoppt der Timer nach 60 s
(Standardprofil) bzw. 120 s (eingelernt).

## Waage

Einschalten auf der Webseite (*Waage*) oder am Display (*EINST. → Waage +*). Die Waage wird über Bluetooth
automatisch gefunden und bei Abbruch neu verbunden. Ausgeschaltet wird Bluetooth gar nicht gestartet.

- **Auto-Tara:** beim Pumpenstart wird tariert (wird wiederholt, falls die Waage den Befehl ignoriert).
- **Waagen-Timer:** startet/stoppt mit der Pumpe; beim Mahlen wird er auf 0 gesetzt.
- **Mühlen-Tara:**
  1. Leeren Siebträger auflegen → *Jetzt wiegen* (Webseite) bzw. *EINST. → Siebträger → WIEGEN*.
  2. *Mühlen-Tara* einschalten.
  3. Mahlen: Die leere Waage wird tariert und zeigt −Siebträgergewicht; gefüllten Siebträger auflegen → der
     gemahlene Kaffee steht groß im Display. Nachmahlen tariert nicht erneut.
- **Historie:** Endgewicht je Bezug (inkl. 4 s Nachtropfen), Dosis und Verhältnis.

Die Waagen selbst zeigen das Rohgewicht – den Siebträger zieht der Coffee Timer rechnerisch ab, da die Waagen
per Bluetooth nur „auf 0 tarieren“ kennen. Der Diagnosebericht zeigt das erkannte Waagenmodell, die gesendeten
Befehle und das letzte Rohpaket (hilfreich, um weitere Waagen zu unterstützen).

## Profile

- **5 Plätze** – jeder speichert Pumpe, Mühlen, Störgeräusche und Schwellen.
- Wechseln, umbenennen, löschen, **exportieren** (Sicherung als Datei) und **importieren** auf der Webseite;
  wechseln auch am Display (*EINST. → Profil*).
- Gespeichert wird je Profil in **drei getrennten Dateien** (Pumpe + Einstellungen, Mühlen, Störgeräusche).
  Jede Änderung schreibt nur ihre eigene Datei – ein Pumpen-Teach kann z. B. die Mühlen nicht verändern.
- Der **Export** enthält die drei Teile einzeln; beim **Import** wird per Häkchen gewählt, welche Teile ersetzt
  werden (z. B. nur die Mühlen aus einem anderen Profil). Exporte älterer Firmware lassen sich weiterhin importieren.
- Profile älterer Firmware-Versionen werden beim Start automatisch ins aktuelle Format übernommen
  (die alte Datei bleibt als `/slotN_old.bin` auf dem Gerät).

## Firmware-Update über WLAN

- **Webseite:** bauen, dann unter *Firmware-Update* die Datei `.pio/build/m5stack-cores3/firmware.bin` hochladen.
- **PlatformIO:** `pio run -e m5stack-cores3-ota -t upload`. Funktioniert `coffeetimer.local` nicht, die IP
  angeben: `--upload-port 192.168.x.y` (oder `upload_port` in `platformio.ini` anpassen).
- Das Update landet im zweiten App-Slot und wird erst nach erfolgreicher Prüfung aktiv. Profile und
  Einstellungen bleiben erhalten.

## Fehlersuche

- **Diagnose** auf der Webseite: kopierbarer Bericht mit Profil, Schwellen, Waage, Vibration, Mühlen,
  freiem Speicher und den Messwerten der letzten 10 Erkennungen.
- Serieller Monitor (115200 Baud): Teach-Kennwerte, jede Erkennung, Waagenbefehle.
- Übersteuert das Mikrofon, erscheint eine Warnung – dann die Verstärkung (Webseite *Mikrofon*) verringern.
  Eingelernte Geräusche rechnen eine geänderte Verstärkung automatisch um.

## Projektstruktur

| Datei | Aufgabe |
|---|---|
| `src/main.cpp` | Einstiegspunkt, Modulübersicht |
| `src/config.h` | feste Parameter (Audio, Teach-Grenzwerte, Netzwerk, Anzeige) |
| `src/audio_features.*` | FFT, Pegel, Bandspektrum, Rhythmus-Kurve |
| `src/pump_detector.*` | Vergleich mit Pumpe, Mühlen und Störgeräuschen; Entprellung |
| `src/teach_mode.*` | Teach-Ablauf und Qualitätsbewertung |
| `src/vibration.*` | Vibrationserkennung (Beschleunigungssensor) |
| `src/shot_timer.*` | Timer und Historie |
| `src/app.*` | Audio-Task, zentrale Logik, Mühlen-Tara, Mutex |
| `src/scale.*` | Bluetooth-Waagen (Protokolle, Verbindung, Tara) |
| `src/ui.*` | Display und Touch |
| `src/web_server.*`, `src/web_page.h` | Webseite und JSON-API |
| `src/settings.*` | Speicherung (Profile im LittleFS, kleine Werte im NVS), Format-Umrechnung |
| `src/net_manager.*` | WLAN, Access-Point, mDNS, NTP |
| `src/datetime.*` | Uhrzeit und RTC |
| `src/ota.*` | Firmware-Update über WLAN |
| `src/battery.*` | Akkustand |

## Lizenz

Copyright (C) 2026 uc-david

Dieses Projekt ist freie Software: Du darfst es unter den Bedingungen der
**GNU General Public License, Version 3 oder (nach deiner Wahl) jeder späteren Version** weitergeben und/oder
verändern – siehe [LICENSE](LICENSE).

Das bedeutet kurz:

- Nutzen, verändern und weitergeben ist erlaubt – privat wie gewerblich.
- Wer die Firmware oder eine veränderte Version **weitergibt** (auch vorinstalliert auf einem Gerät), muss den
  vollständigen Quellcode unter derselben Lizenz mitliefern bzw. zugänglich machen.
- Es gibt keine Gewährleistung.

Verwendete Bibliotheken und deren Lizenzen sowie die Quellen der Waagen-Protokolle: siehe
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
