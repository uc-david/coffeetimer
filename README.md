# Coffee Timer (M5Stack CoreS3)

Erkennt am Geräusch, wann die Pumpe einer Siebträger-Kaffeemaschine läuft, und misst die Bezugszeit.
Bedienung am Gerät (Touch) und parallel über eine Webseite.

## Erste Inbetriebnahme

1. Flashen: `pio run -t upload`
2. Ohne WLAN-Konfiguration öffnet das Gerät den Access-Point **CoffeeTimer-XXXX** (offen).
   Damit verbinden, `http://192.168.4.1` öffnen, unter *WLAN* die Zugangsdaten eintragen → Neustart.
3. Danach ist die Seite unter `http://coffeetimer.local` bzw. der IP in der Kopfzeile des Displays erreichbar.
   Klappt die Verbindung nicht innerhalb von 15 s, öffnet das Gerät wieder den Access-Point.

## WLAN

- Ohne gespeichertes WLAN öffnet der CoreS3 den Access-Point **CoffeeTimer-XXXX** (offen, `http://192.168.4.1`).
- Auf der Webseite unter **WLAN → WLAN konfigurieren** SSID und Passwort eintragen → Gerät startet neu und
  verbindet sich. Die IP steht oben rechts im Display (grün = verbunden, gelb = Access-Point).
- Ist das WLAN nicht erreichbar, kommt nach 15 s wieder der Access-Point; die Verbindung wird im Hintergrund
  weiter versucht. Leere SSID speichern = zurück zum Access-Point.
- Nur 2,4-GHz-Netze (ESP32-S3 kann kein 5 GHz).

## Firmware-Update (OTA)

- **Webseite:** In PlatformIO bauen, dann unter *Firmware-Update* die Datei
  `.pio/build/m5stack-cores3/firmware.bin` hochladen.
- **PlatformIO direkt:** `pio run -e m5stack-cores3-ota -t upload` (bzw. in VS Code die Umgebung
  *m5stack-cores3-ota* wählen → Upload). Geht `coffeetimer.local` nicht, `upload_port` auf die IP setzen.
- Das Update landet im zweiten App-Slot und wird erst nach erfolgreicher Prüfung aktiv; bei Abbruch läuft die
  alte Firmware weiter. Profil und Einstellungen (NVS) bleiben erhalten.
- Der erste Flash nach dieser Änderung muss einmal per USB erfolgen.

## Profile

- **5 Profil-Plätze** (z. B. verschiedene Maschinen oder Aufstellorte). Jeder speichert Pumpenprofil,
  Störgeräusche und Schwellen. Wechseln am Display (**INFO → PROFIL**) oder auf der Webseite (*Profile*).
- **Standardprofil:** Leere Plätze funktionieren ohne Teach – erkannt wird jedes *gleichmäßige* Geräusch (±1,5 dB,
  mind. 1 s) deutlich über dem **automatisch gemessenen Raumpegel** (fällt schnell, steigt langsam mit ≈0,5 dB/s).
  Niesen, Sprechen, Klappern und Dauergeräusche (Lüfter) lösen nicht aus; eine gleichmäßige Mühle schon → als
  Störgeräusch einlernen oder besser einen Teach durchführen. Notlösung – zuverlässig erst mit Teach.
- **Sicherung:** Meldet die Erkennung länger als 60 s (Standardprofil) bzw. 120 s (gelernt) „Pumpe“, stoppt der Timer.
- Umbenennen, Löschen, **Export als Datei** (Sicherung) und **Import** auf der Webseite.
- Gespeichert wird im Dateisystem (LittleFS); Speicherfehler werden am Display und auf der Webseite angezeigt.
  Ein Profil aus Firmware 1.3/1.4 wird beim ersten Start – falls noch vorhanden – in Platz 1 übernommen.

## Bedienung am Display

- **TEACH**: Pumpe oder Störgeräusch einlernen · **RESET**: Timer auf 0 · **INFO**: Übersicht
- **INFO → EINST.**: alle Einstellungen der Webseite. Zeile antippen, mit **− / +** ändern; unten auf
  „< Seite >“ tippen zum Blättern. Enthalten: Profil wechseln, Pegel-, Muster- bzw. Takt-Schwelle,
  Ein-/Ausschaltverzögerung, Mindestdauer, Mikrofon-Verstärkung, einzelnes Störgeräusch bzw. Klangmuster löschen,
  Historie löschen, Profil zurücksetzen. Lösch-Aktionen erst nach zweitem Tippen („SICHER?“).
- Profil-Kurzinfo: *Aufnahmen* = Anzahl Teach-Aufnahmen, *Muster* = gelernte Klangbilder (z. B. Anlauf / voller
  Druck; eines muss passen), *Takt* = Pumpentakt (Vibrationspumpe), *Q* = Qualität 0–100,
  *Störger.* = eingelernte Störgeräusche.

## Waage (optional): Felicita Arc

- Einschalten auf der Webseite (*Waage*) oder am Display (**INFO → EINST. → Waage +**). Die Waage einschalten –
  sie wird über Bluetooth LE automatisch gefunden (Name beginnt mit „FELICITA“) und bei Abbruch neu verbunden.
  Ausgeschaltet wird Bluetooth gar nicht gestartet.
- Anzeige: Gewicht unter dem Timer, während des Bezugs zusätzlich Durchfluss (g/s); Webseite ebenso.
- **Auto-Tara** (Standard an): beim Erkennen des Pumpenstarts wird die Waage tariert.
  **Waagen-Timer** (optional): Timer der Waage startet/stoppt mit der Pumpe.
- Tarieren von Hand: Gewichtszeile am Display antippen, *EINST. → Waage tarieren* oder Webseite.
- Historie: Endgewicht je Bezug, inkl. 4 s Nachtropfen nach dem Pumpenstopp.
- Protokoll: Service `FFE0` / Characteristic `FFE1`; 18-Byte-Pakete mit Vorzeichen und 6 Ziffern (0,01 g),
  Befehle `T` (Tara), `R`/`S`/`C` (Timer). Der Diagnose-Bericht zeigt das letzte Rohpaket.

## Mikrofon

Verstärkung auf der Webseite einstellbar (1×–64×, Standard 16×). Bei Übersteuerung erscheint eine Warnung
(Webseite, INFO-Screen). Profile merken sich die Verstärkung beim Teach und werden bei einer Änderung
automatisch umgerechnet – kein neuer Teach nötig.

## Teach-Modus

Das Gerät nah an der Maschine aufstellen (Pumpe sitzt meist unten/hinten). Dann **TEACH** (Display oder Webseite):

| Schritt | Was tun | Dauer |
|---|---|---|
| 1 | Ruhe, Pumpe **aus** – Hintergrund wird aufgenommen | 3 s |
| 2 | Einen ganz normalen **Bezug starten** (am besten mit Kaffee) | wartet max. 20 s |
| 3 | Bezug wie gewohnt beenden – Aufnahme endet automatisch, wenn die Pumpe 4 s still ist (oder **FERTIG**) | bis 60 s |

Ausgewertet wird:

- **Takt** (Autokorrelation der Hüllkurve): Vibrationspumpen schlagen im Netztakt (50 Hz). Dieser Rhythmus
  bleibt unter Druck exakt gleich, auch wenn sich der Klang stark ändert.
- **Klangmuster**: bis zu 3 Spektren je Aufnahme (z. B. Anlauf / Druckaufbau / voller Druck).
- **Schwellen** werden aus der Aufnahme berechnet und beim Speichern übernommen.
- **Pausen** (Pre-Infusion) werden erkannt; die Ausschaltverzögerung wird so gesetzt, dass sie überbrückt werden.

Die Bewertung (0–100 Punkte, **OK / Warnung / Fehler**) prüft u. a., welcher Anteil der Aufnahme mit den
berechneten Schwellen tatsächlich erkannt würde und ob der Hintergrund fälschlich auslösen würde – jeweils mit
Begründung und Tipp (Übersteuerung, zu leise, zu wenig Abstand zum Hintergrund, unruhiger Hintergrund,
unspezifisches Klangmuster …).

Danach: **Ersetzen** (neues Profil) oder **Ergänzen** (z. B. erst Leerbezug, dann echter Bezug – beide werden erkannt).

### Störgeräusch einlernen (z. B. Mühle)

**TEACH → STÖRGER.** (bzw. „Störgeräusch einlernen“ auf der Webseite), dann die Mühle laufen lassen. Gespeichert
werden Klangmuster und Rhythmus-Fingerabdruck des Geräuschs; klingt etwas künftig einem Störgeräusch ähnlicher als
der Pumpe, wird es ignoriert (Display: „Störgeräusch – ignoriert“). Das Ergebnis zeigt, zu wie viel % das Geräusch
vorher bzw. nachher als Pumpe erkannt wird, und warnt, falls es der Pumpe so ähnlich ist, dass auch die Pumpe
unterdrückt werden könnte. Bis zu 3 Störgeräusche; löschen auf der Webseite.

## Erkennung

Alle 32 ms wird ein 64-ms-Fenster ausgewertet (geglättet, damit das Pulsieren einer Vibrationspumpe nicht als
Schwankung erscheint). Die Pumpe gilt als *an*, wenn der Pegel ≥ *Pegel-Schwelle* ist (Anteil zwischen
Hintergrund und dem *leisen* Teil der Pumpe), der Pegel über ~0,5 s **gleichmäßig** ist (Grenze aus dem Teach;
Niesen, Sprechen, Klappern schwanken stärker) **und**

- bei Pumpen **mit Takt** (Vibrationspumpe): die Takt-Stärke ≥ *Takt-Schwelle* ist **und** die Rhythmus-Form
  zum gelernten Fingerabdruck passt – unabhängig vom Klang, daher gleich gut bei Leerbezug und echtem Bezug.
  Die Rhythmus-Form unterscheidet die Pumpe (ein Schlag je Netzperiode) von netzbrummenden Motoren wie Mühlen
  (Brummen mit 100 Hz),
- bei Pumpen **ohne Takt** (Rotationspumpe): das Klangmuster ≥ *Klangmuster-Schwelle* ist. Verglichen wird
  die Klangfarbe (welche Frequenzbänder relativ stärker sind), nicht die Lautstärke,
- und das Geräusch keinem gelernten Störgeräusch ähnlicher ist als der Pumpe.

Technik: Alle Merkmale stammen aus FFTs – 24 Frequenzbänder (Klangmuster) und die per FFT berechnete
Autokorrelation der Hüllkurve (Takt und Rhythmus-Form).

Welche Variante gilt, entscheidet der Teach automatisch. Ein-/Ausschaltverzögerung entprellen das Signal;
die Einschaltverzögerung wird vom Timer rückwirkend berücksichtigt, das Ende ist der letzte aktive Frame.
Alle Schwellen sind auf der Webseite feinjustierbar, die Empfindlichkeit zusätzlich am Display (INFO → EMPF. +/-).

Der Timer-Start wird auf den Beginn der lauten Phase zurückdatiert (max. 1,5 s), da die Merkmale nach dem
Einschalten einige Frames brauchen. Unter *Diagnose* auf der Webseite gibt es einen kopierbaren Bericht mit
Profil, Schwellen und den Messwerten der letzten 10 Erkennungen.

Im seriellen Monitor (115200 Baud) werden Teach-Kennwerte sowie jedes Ein-/Ausschalten mit Messwerten ausgegeben.

## Projektstruktur

| Datei | Aufgabe |
|---|---|
| `src/config.h` | feste Parameter (Audio, Teach-Grenzwerte, Netzwerk) |
| `src/audio_features.*` | FFT, Pegel, Bandspektrum |
| `src/pump_detector.*` | Vergleich mit Profil, Entprellung |
| `src/teach_mode.*` | Teach-Ablauf und Qualitätsbewertung |
| `src/shot_timer.*` | Timer und Historie |
| `src/app.*` | Audio-Task, zentrale Logik, Mutex |
| `src/ui.*` | Display und Touch |
| `src/web_server.*`, `src/web_page.h` | Webseite und JSON-API |
| `src/net_manager.*` | WLAN, Access-Point, mDNS, NTP |
| `src/ota.*` | Firmware-Update über WLAN |
| `src/battery.*` | Akkustand (Display oben rechts, Webseite) |
| `src/scale.*` | optionale Felicita-Arc-Waage (Bluetooth LE, NimBLE) |
| `src/settings.*` | Speicherung im NVS |

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

Verwendete Bibliotheken und deren Lizenzen: siehe [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
