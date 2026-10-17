/**
 * @file shot_timer.cpp
 * @brief Implementierung von Timer und Historie.
 */
#include "shot_timer.h"

void ShotTimer::start(uint32_t startMs) {
  running_ = true;
  startMs_ = startMs;
}

bool ShotTimer::stop(uint32_t endMs, uint32_t minHistoryMs) {
  if (!running_) return false;
  running_ = false;
  lastDurationMs_ = endMs - startMs_;
  lastStopRecorded_ = lastDurationMs_ >= minHistoryMs;

  if (lastStopRecorded_) {
    ShotRecord& r = history_[historyHead_];
    r.durationMs = lastDurationMs_;
    r.weightG = NAN;
    r.doseG = NAN;
    const time_t now = time(nullptr);
    r.endEpoch = now > cfg::kValidEpoch ? now : 0;
    historyHead_ = (historyHead_ + 1) % cfg::kHistorySize;
    if (historyCount_ < cfg::kHistorySize) ++historyCount_;
  }
  return true;
}

void ShotTimer::setLatestWeight(float g) {
  if (historyCount_ == 0) return;
  history_[(historyHead_ + cfg::kHistorySize - 1) % cfg::kHistorySize].weightG = g;
}

void ShotTimer::setLatestDose(float g) {
  if (historyCount_ == 0) return;
  history_[(historyHead_ + cfg::kHistorySize - 1) % cfg::kHistorySize].doseG = g;
}

void ShotTimer::reset() {
  running_ = false;
  lastDurationMs_ = 0;
}

void ShotTimer::clearHistory() {
  historyHead_ = 0;
  historyCount_ = 0;
}

uint32_t ShotTimer::displayMs(uint32_t nowMs) const {
  return running_ ? nowMs - startMs_ : lastDurationMs_;
}

const ShotRecord& ShotTimer::history(size_t i) const {
  // Neuester Eintrag liegt direkt vor historyHead_.
  const size_t idx = (historyHead_ + cfg::kHistorySize - 1 - i) % cfg::kHistorySize;
  return history_[idx];
}
