# Verwendete Fremdsoftware und Lizenzen

Der Coffee Timer selbst steht unter der **GNU General Public License v3.0 oder später** (siehe [LICENSE](LICENSE)).

Die folgenden Bibliotheken sind **nicht** Teil dieses Repositorys. PlatformIO lädt sie beim Bauen automatisch
herunter (siehe `lib_deps` in `platformio.ini`). Wer eine fertig gebaute Firmware (`firmware.bin`) weitergibt,
verteilt diese Bibliotheken jedoch mit und muss deren Lizenzhinweise beilegen – dafür ist diese Datei gedacht.
Alle genannten Lizenzen sind mit der GPL-3.0 vereinbar.

| Komponente | Lizenz | Copyright | Quelle |
|---|---|---|---|
| M5Unified | MIT | © 2021 M5Stack | https://github.com/m5stack/M5Unified |
| M5GFX | MIT | © 2021 M5Stack | https://github.com/m5stack/M5GFX |
| LovyanGFX (in M5GFX enthalten) | FreeBSD (BSD-2-Clause) | © lovyan03 | https://github.com/lovyan03/LovyanGFX |
| efont-Schriften (in M5GFX enthalten) | BSD-3-Clause | © 2000–2001 /efont/ The Electronic Font Open Laboratory | http://openlab.ring.gr.jp/efont/ |
| 7-Segment-Schrift „Font 7“ (in M5GFX enthalten) | BSD-artig, siehe M5GFX/LovyanGFX | © Bodmer (TFT_eSPI) bzw. Adafruit | https://github.com/Bodmer/TFT_eSPI |
| ArduinoJson | MIT | © 2014–2026 Benoît Blanchon | https://github.com/bblanchon/ArduinoJson |
| NimBLE-Arduino | Apache-2.0 | © 2020 Ryan Powell; teilweise abgeleitet von esp32-snippets, © 2017 Neil Kolban | https://github.com/h2zero/NimBLE-Arduino |
| Arduino-ESP32 (Core) | LGPL-2.1 | © Espressif Systems und Beitragende | https://github.com/espressif/arduino-esp32 |
| ESP-IDF | Apache-2.0 | © Espressif Systems | https://github.com/espressif/esp-idf |
| littlefs / esp_littlefs | BSD-3-Clause / MIT | © The littlefs authors; © Brian Pugh | https://github.com/littlefs-project/littlefs, https://github.com/joltwallet/esp_littlefs |

Die vollständigen Lizenztexte liegen jeweils in den genannten Projekten (nach dem ersten Build auch lokal unter
`.pio/libdeps/` bzw. im PlatformIO-Paket des Frameworks).

## Hinweis zur LGPL (Arduino-ESP32)

Die Firmware ist statisch mit dem Arduino-ESP32-Core gelinkt. Die LGPL verlangt, dass Empfänger einer
Firmware diese mit einer geänderten Version des Cores neu bauen können. Das ist erfüllt: Der vollständige
Quellcode dieses Projekts ist öffentlich und lässt sich mit PlatformIO bauen.

## Waagen-Protokolle

Die Bluetooth-Protokolle der Waagen wurden anhand öffentlicher Dokumentation und Open-Source-Projekten
nachimplementiert; es wurde kein fremder Code übernommen. Quellen:

- Decent Scale API – https://decentespresso.com/decentscale_api
- AcaiaArduinoBLE (Acaia, Bookoo, Felicita) – https://github.com/tatemazer/AcaiaArduinoBLE
- Beanconqueror (Felicita u. a.) – https://github.com/graphefruit/Beanconqueror
