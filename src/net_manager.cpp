/**
 * @file net_manager.cpp
 * @brief Implementierung der WLAN-Verwaltung.
 */
#include "net_manager.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"

namespace {

enum class State { Idle, Connecting, Connected, AccessPoint };

State state = State::Idle;
uint32_t connectStartMs = 0;
bool mdnsStarted = false;
bool ntpStarted = false;
String apName;

/// Startet mDNS (einmalig), sobald irgendein Netz verfügbar ist.
void startMdns() {
  if (mdnsStarted) return;
  if (MDNS.begin(cfg::kHostname)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
}

/// Startet den eigenen Access-Point; Heim-WLAN wird parallel weiter versucht.
void startAccessPoint() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "%s%02X%02X", cfg::kApSsidPrefix, mac[4], mac[5]);
  apName = name;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  state = State::AccessPoint;
  startMdns();
  Serial.printf("[wifi] Access-Point '%s' aktiv, IP %s\n", apName.c_str(),
                WiFi.softAPIP().toString().c_str());
}

}  // namespace

namespace net {

void begin(const WifiSettings& wifi) {
  WiFi.persistent(false);  // Zugangsdaten verwalten wir selbst (settings.cpp)
  WiFi.setHostname(cfg::kHostname);
  WiFi.setAutoReconnect(true);

  if (wifi.ssid.isEmpty()) {
    Serial.println("[wifi] kein WLAN konfiguriert");
    startAccessPoint();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi.ssid.c_str(), wifi.password.c_str());
  state = State::Connecting;
  connectStartMs = millis();
  Serial.printf("[wifi] verbinde mit '%s' …\n", wifi.ssid.c_str());
}

void loop() {
  const bool staUp = WiFi.status() == WL_CONNECTED;

  switch (state) {
    case State::Connecting:
      if (staUp) {
        state = State::Connected;
      } else if (millis() - connectStartMs > cfg::kWifiConnectTimeoutMs) {
        Serial.println("[wifi] Verbindung fehlgeschlagen -> Access-Point");
        startAccessPoint();
      }
      break;

    case State::AccessPoint:
      // Heim-WLAN doch noch erreichbar geworden -> AP abschalten.
      if (staUp) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        state = State::Connected;
      }
      break;

    default:
      break;
  }

  // Einmalige Dienste nach erfolgreicher Verbindung zum Heimnetz.
  if (state == State::Connected && staUp && !ntpStarted) {
    Serial.printf("[wifi] verbunden, IP %s\n", WiFi.localIP().toString().c_str());
    configTzTime(cfg::kTimezone, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
    startMdns();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

bool apActive() { return state == State::AccessPoint; }

String ip() {
  if (connected()) return WiFi.localIP().toString();
  if (apActive()) return WiFi.softAPIP().toString();
  return "-";
}

String networkName() {
  if (connected()) return WiFi.SSID();
  if (apActive()) return apName;
  return "";
}

}  // namespace net
