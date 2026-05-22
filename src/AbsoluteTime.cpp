#include "AbsoluteTime.h"

#include <chrono>
#include <esp_log.h>
#include <esp_sntp.h>
#include <mdns.h>
#include <string.h>
#include <sys/time.h>
#include <thread>

#define TAG "AbsoluteTime"

AbsoluteTime &AbsoluteTime::getInstance() {
  static AbsoluteTime instance;
  return instance;
}

AbsoluteTime::AbsoluteTime()
    : serverAddress_("cyranbroker"), syncIntervalSecs_(10), synced_(false),
      lastTimestamp_(0), lastMillis_(0), fallbackMode_(false),
      syncThreadHandle_(nullptr), smoothedDriftRate_(0.0) {}

AbsoluteTime::~AbsoluteTime() { stopSyncThread(); }

void AbsoluteTime::setServerAddress(const std::string &address) {
  std::lock_guard<std::mutex> lock(mutex_);
  serverAddress_ = address;
}

void AbsoluteTime::setSyncInterval(uint32_t seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  syncIntervalSecs_ = seconds;
}

void AbsoluteTime::begin(const std::string &serverAddress,
                         uint32_t syncIntervalSecs,
                         const std::string &fallbackIp) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    serverAddress_ = serverAddress;
    syncIntervalSecs_ = syncIntervalSecs;
    fallbackIp_ = fallbackIp;
  }
  resolveServerAddress();
  startSyncThread();
}

void AbsoluteTime::resolveServerAddress() {
  // Try mDNS resolution if address ends with .local
  ESP_LOGW(TAG, "Attempting to resolve server address: %s",
           serverAddress_.c_str());
  const char *fallbackIp = fallbackIp_.empty() ? nullptr : fallbackIp_.c_str();
  bool resolved = false;
  if (serverAddress_.size() > 6 &&
      serverAddress_.substr(serverAddress_.size() - 6) == ".local") {
    esp_ip4_addr_t addr;
    ESP_LOGW(TAG, "Using mDNS to resolve: %s", serverAddress_.c_str());
    esp_err_t err = mdns_query_a(serverAddress_.c_str(), 1000, &addr);
    if (err == ESP_OK) {
      const char *ip = ip4addr_ntoa((const ip4_addr_t *)&addr);
      ESP_LOGW(TAG, "mDNS resolved %s to %s", serverAddress_.c_str(), ip);
      serverAddress_ = ip;
      resolved = true;
    } else {
      ESP_LOGE(TAG, "mDNS resolution failed for %s (err=%d)",
               serverAddress_.c_str(), err);
      // Try fallback IP
      if (fallbackIp) {
        ESP_LOGW(TAG, "Trying fallback IP: %s", fallbackIp);
        serverAddress_ = fallbackIp;
        // Optionally, check if fallback IP is reachable (not implemented
        // here)
        resolved = true; // Assume fallback IP is valid for SNTP
      } else {
        ESP_LOGE(TAG, "No fallback IP provided, cannot resolve NTP server.");
        resolved = false;
      }
    }
  } else {
    ESP_LOGW(TAG, "Using direct address: %s", serverAddress_.c_str());
    resolved = true;
  }
  if (!resolved) {
    ESP_LOGE(
        TAG,
        "Could not resolve NTP server address, falling back to local time!");
    fallbackToMillis();
  }
}

void AbsoluteTime::startSyncThread() {
  stopSyncThread();
  fallbackMode_ = false;
  synced_ = false;
  // Start a new thread for periodic sync
  syncThreadHandle_ = new std::thread([this]() { this->syncTask(); });
}

void AbsoluteTime::stopSyncThread() {
  if (syncThreadHandle_) {
    std::thread *t = static_cast<std::thread *>(syncThreadHandle_);
    if (t->joinable())
      t->join();
    delete t;
    syncThreadHandle_ = nullptr;
  }
}

void AbsoluteTime::syncTask() {
  while (!fallbackMode_) {
    updateTimeFromServer();

    // ── Wait for next sync using HARDWARE timer (not system clock) ────────
    // Record when we synced using esp_timer (monotonic, not affected by SNTP)
    uint64_t syncStartMicros = esp_timer_get_time();
    uint64_t nextSyncMicros =
        syncStartMicros + (syncIntervalSecs_ * 1000000ULL);

    // Poll hardware timer until interval elapses
    while (!fallbackMode_) {
      uint64_t nowMicros = esp_timer_get_time();
      if (nowMicros >= nextSyncMicros) {
        break; // Time for next sync
      }
      // Sleep 100ms, but timing is based on hardware timer, not sleep accuracy
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void AbsoluteTime::updateTimeFromServer() {
  ESP_LOGI(TAG, "Setting up SNTP with server: %s", serverAddress_.c_str());
  sntp_stop();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, serverAddress_.c_str());
  sntp_init();
  // Wait for sync (max 20s, check every 200ms)
  for (int i = 0; i < 100; ++i) {
    time_t now = 0;
    time(&now);
    // Reduced logging in tight loop - only log failures
    if (now > 100000) {
      uint64_t newNtpTime = (uint64_t)now * 1000;

      // ── Calculate drift: compare our compensated prediction vs actual NTP ──
      std::lock_guard<std::mutex> lock(mutex_);

      if (newNtpTime < lastTimestamp_)
        newNtpTime = lastTimestamp_; // Clamp backward jumps

      uint64_t nowMillis = esp_timer_get_time() / 1000;

      if (lastNtpSyncMillis_ != 0 && lastNtpSyncTime_ != 0) {
        // ── What does our compensated system think the time is RIGHT NOW? ──
        int64_t elapsedMillis = nowMillis - lastNtpSyncMillis_;
        int64_t driftCompensation =
            (int64_t)(elapsedMillis * smoothedDriftRate_);
        uint64_t ourTime = lastNtpSyncTime_ + elapsedMillis - driftCompensation;

        // ── Error = how far off we are from actual NTP ──
        int64_t error = (int64_t)(ourTime - newNtpTime);
        lastDriftMs_ = error;
        driftSum_ += error;
        driftCount_++;

        if (driftCount_ == 1) {
          // First sync: use raw hardware drift as initial rate
          int64_t rawDrift =
              elapsedMillis - (int64_t)(newNtpTime - lastNtpSyncTime_);
          smoothedDriftRate_ = (double)rawDrift / (double)elapsedMillis;
          lastError_ = error;
          ESP_LOGI(TAG, "Initial drift: error=%lld ms, rate=%.3f ppm", error,
                   smoothedDriftRate_ * 1e6);
        } else {
          // ── PD Controller for stable, fast convergence ──
          // P (proportional): responds to current error
          // D (derivative): dampens oscillations

          const double Kp =
              0.8; // Proportional gain (0.7-1.0 for fast convergence)
          const double Kd = 0.4; // Derivative gain (0.3-0.5 for damping)

          // Proportional term: error / elapsed
          double proportional = (double)error / (double)elapsedMillis;

          // Derivative term: (error - lastError) / elapsed
          double derivative =
              (double)(error - lastError_) / (double)elapsedMillis;

          // PD control: rate adjustment
          double rateAdjustment = Kp * proportional + Kd * derivative;

          smoothedDriftRate_ = smoothedDriftRate_ + rateAdjustment;
          lastError_ = error;

          ESP_LOGI(TAG, "Error: %lld ms, P=%.3f, D=%.3f, rate=%.3f ppm", error,
                   proportional * 1e6, derivative * 1e6,
                   smoothedDriftRate_ * 1e6);
        }
      }
      lastNtpSyncMillis_ = nowMillis;
      lastNtpSyncTime_ = newNtpTime;
      lastTimestamp_ = newNtpTime;
      lastMillis_ = nowMillis;
      synced_ = true;
      ESP_LOGI(TAG, "Time synced: %llu", newNtpTime);
      // Note: Current error shown above in PD controller log

      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  ESP_LOGE(TAG, "NTP sync timeout after 20 seconds, falling back to millis()");
  fallbackToMillis();
}

void AbsoluteTime::fallbackToMillis() {
  std::lock_guard<std::mutex> lock(mutex_);
  fallbackMode_ = true;
  synced_ = false;
  lastMillis_ = esp_timer_get_time() / 1000;
  lastTimestamp_ = ((uint64_t)0x01 << 56) | (uint64_t)lastMillis_;
}

uint64_t AbsoluteTime::getTimestamp() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fallbackMode_) {
    uint64_t nowMillis = esp_timer_get_time() / 1000;
    uint64_t ts = ((uint64_t)0x01 << 56) | (uint64_t)nowMillis;
    if (ts < lastTimestamp_)
      ts = lastTimestamp_; // Clamp backward
    lastTimestamp_ = ts;
    return ts;
  } else {
    // ── Drift-compensated timestamp interpolation ───────────────────────
    uint64_t nowMillis = esp_timer_get_time() / 1000;

    if (lastNtpSyncMillis_ == 0 || driftCount_ == 0) {
      // No drift data yet, fall back to system time
      time_t now = 0;
      time(&now);
      uint64_t ts = (uint64_t)now * 1000;
      if (ts < lastTimestamp_)
        ts = lastTimestamp_;
      lastTimestamp_ = ts;
      return ts;
    }

    // Time elapsed since last NTP sync (using local esp_timer)
    int64_t elapsedMillis = nowMillis - lastNtpSyncMillis_;

    // Apply drift compensation using smoothed drift rate
    // Positive drift = local clock runs fast → SUBTRACT to correct
    // Negative drift = local clock runs slow → ADD (subtract negative)
    int64_t driftCompensation = (int64_t)(elapsedMillis * smoothedDriftRate_);

    // Interpolated timestamp = last NTP time + elapsed time - drift correction
    uint64_t ts = lastNtpSyncTime_ + elapsedMillis - driftCompensation;

    // Clamp backward jumps (safety)
    if (ts < lastTimestamp_)
      ts = lastTimestamp_;
    lastTimestamp_ = ts;
    return ts;
  }
}

bool AbsoluteTime::isSynced() {
  std::lock_guard<std::mutex> lock(mutex_);
  return synced_;
}

int64_t AbsoluteTime::getLastDriftMs() const { return lastDriftMs_; }

double AbsoluteTime::getAverageDriftMs() const {
  if (driftCount_ == 0) {
    return 0.0;
  }
  return driftSum_ / driftCount_;
}
