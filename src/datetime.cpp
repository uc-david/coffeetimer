/**
 * @file datetime.cpp
 * @brief Implementierung von Uhrzeit / RTC.
 */
#include "datetime.h"

#include <M5Unified.h>
#include <esp_sntp.h>
#include <time.h>

#include <atomic>

#include "config.h"

namespace {

/// Wird vom SNTP-Task gesetzt, in loop() abgearbeitet (I2C nur aus einem Task).
std::atomic<bool> ntpSynced{false};

void onNtpSync(struct timeval*) { ntpSynced = true; }

}  // namespace

namespace datetime {

void begin() {
  // Zeitzone schon vor dem ersten NTP-Abgleich, damit die RTC-Zeit lokal angezeigt wird.
  setenv("TZ", cfg::kTimezone, 1);
  tzset();
  sntp_set_time_sync_notification_cb(onNtpSync);

  if (!M5.Rtc.isEnabled()) {
    Serial.println("[time] keine RTC gefunden");
  } else if (M5.Rtc.getVoltLow() || !valid()) {
    Serial.println("[time] RTC nicht gestellt – warte auf NTP");
  } else {
    char buf[24];
    format(buf, sizeof(buf));
    Serial.printf("[time] Zeit aus RTC: %s\n", buf);
  }
}

void loop() {
  if (!ntpSynced.exchange(false)) return;
  const time_t now = time(nullptr);
  if (now <= cfg::kValidEpoch || !M5.Rtc.isEnabled()) return;
  tm utc;
  gmtime_r(&now, &utc);
  M5.Rtc.setDateTime(&utc);  // RTC läuft in UTC (wie von M5.begin() erwartet)
  Serial.println("[time] NTP-Zeit in RTC übernommen");
}

bool valid() { return time(nullptr) > cfg::kValidEpoch; }

void format(char* buf, size_t size) {
  if (size == 0) return;
  buf[0] = '\0';
  if (!valid()) return;
  const time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  strftime(buf, size, "%d.%m.%Y %H:%M", &local);
}

}  // namespace datetime
