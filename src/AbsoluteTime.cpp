#include "AbsoluteTime.h"

#include <esp_log.h>
#include <esp_sntp.h>
#include <mdns.h>
#include <string.h>
#include <sys/time.h>

#define TAG "AbsoluteTime"

AbsoluteTime &AbsoluteTime::getInstance() {
  static AbsoluteTime instance;
  return instance;
}

AbsoluteTime::AbsoluteTime()
    : serverAddress_("cyranbroker"), syncIntervalSecs_(10), synced_(false),
      lastTimestamp_(0), fallbackMode_(false) {}

AbsoluteTime::~AbsoluteTime() {
  // Stop SNTP if running
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
}

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

  // Don't proceed if we're in fallback mode
  if (fallbackMode_) {
    return;
  }

  // ══════════════════════════════════════════════════════════════
  // Initialize SNTP using the STANDARD ESP-IDF approach
  // ══════════════════════════════════════════════════════════════

  ESP_LOGI(TAG, "Initializing SNTP with server: %s, interval: %u seconds",
           serverAddress_.c_str(), syncIntervalSecs_);

  // Stop any existing SNTP instance
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP operating mode
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

  // Configure server
  esp_sntp_setservername(0, serverAddress_.c_str());

  // Set sync mode to SMOOTH (uses adjtime() to gradually adjust clock)
  sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
  // Set callback to monitor sync events (debug/info builds only)
  sntp_set_time_sync_notification_cb(sntpSyncNotificationCallback);
#endif

  // Start SNTP - it will run continuously in the background!
  esp_sntp_init();

  // Set sync interval AFTER init (in milliseconds)
  // Must be set after init for it to take effect properly
  sntp_set_sync_interval(syncIntervalSecs_ * 1000);
  uint32_t actualInterval = sntp_get_sync_interval();

  ESP_LOGI(TAG, "SNTP started - requested %u seconds, actual interval: %u ms",
           syncIntervalSecs_, actualInterval);
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

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
// ══════════════════════════════════════════════════════════════
// SNTP Sync Callback - called by ESP-IDF when time is synchronized
// Only compiled in debug/info builds for monitoring
// ══════════════════════════════════════════════════════════════
void AbsoluteTime::sntpSyncNotificationCallback(struct timeval *tv) {
  AbsoluteTime &instance = getInstance();
  std::lock_guard<std::mutex> lock(instance.mutex_);

  // Get current NTP time (system clock, already adjusted by SNTP)
  uint64_t ntpTimeMs = (uint64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000;

  // Get current hardware timer value (monotonic, never adjusted)
  uint64_t hwTimerMs = esp_timer_get_time() / 1000;

  // Mark as synced - this persists between sync events!
  instance.synced_ = true;
  instance.lastTimestamp_ = ntpTimeMs;

  // Calculate hardware drift if we have a previous sync
  if (instance.syncCount_ > 0) {
    // How much time passed according to NTP
    int64_t ntpDelta = ntpTimeMs - instance.lastSyncNtpMillis_;

    // How much time passed according to hardware timer
    int64_t hwDelta = hwTimerMs - instance.lastSyncHwMillis_;

    // Difference = hardware drift
    instance.lastHwDriftMs_ = hwDelta - ntpDelta;

    ESP_LOGI(TAG, "Sync #%u: HW drift=%lld ms over %lld ms (%.1f ppm)",
             instance.syncCount_, instance.lastHwDriftMs_, ntpDelta,
             instance.lastHwDriftMs_ * 1e6 / (double)ntpDelta);
  } else {
    ESP_LOGI(TAG, "Initial SNTP sync complete");
  }

  // Update references for next sync
  instance.lastSyncNtpMillis_ = ntpTimeMs;
  instance.lastSyncHwMillis_ = hwTimerMs;
  instance.syncCount_++;

  ESP_LOGI(TAG, "Time synced: %llu (SNTP smooth mode)", ntpTimeMs);
}
#endif // LOG_LOCAL_LEVEL >= ESP_LOG_INFO

void AbsoluteTime::fallbackToMillis() {
  std::lock_guard<std::mutex> lock(mutex_);
  fallbackMode_ = true;
  synced_ = false;
  uint64_t hwMillis = esp_timer_get_time() / 1000;
  lastTimestamp_ = ((uint64_t)0x01 << 56) | hwMillis;
  ESP_LOGW(TAG, "Using fallback mode (hardware timer only)");
}

uint64_t AbsoluteTime::getTimestamp() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (fallbackMode_) {
    // Fallback: use hardware timer
    uint64_t nowMillis = esp_timer_get_time() / 1000;
    uint64_t ts = ((uint64_t)0x01 << 56) | nowMillis;

    // Only clamp if last timestamp was also in fallback mode
    if ((lastTimestamp_ & ((uint64_t)0x01 << 56)) && ts < lastTimestamp_)
      ts = lastTimestamp_;

    lastTimestamp_ = ts;
    return ts;
  } else {
    // ══════════════════════════════════════════════════════════════
    // Standard approach: Just read the SNTP-adjusted system clock!
    // SNTP continuously adjusts it via adjtime() in smooth mode
    // ══════════════════════════════════════════════════════════════
    time_t now = 0;
    time(&now);
    uint64_t ts = (uint64_t)now * 1000;

    // Validate: time() should return > year 2020 (1577836800 seconds)
    // If invalid, return last valid timestamp (must stay in NTP format)
    const uint64_t MIN_VALID_TIME_MS = 1577836800000ULL; // Jan 1, 2020
    if (ts < MIN_VALID_TIME_MS) {
      ESP_LOGW(TAG, "Invalid system time (%llu), using last timestamp", ts);
      // If we have a valid cached timestamp, use it
      if (lastTimestamp_ > MIN_VALID_TIME_MS &&
          !(lastTimestamp_ & ((uint64_t)0x01 << 56))) {
        return lastTimestamp_;
      }
      // Otherwise, keep trying - return current invalid value and let SNTP fix
      // it DO NOT switch to fallback format - that would corrupt the timestamp
      // stream
      ESP_LOGE(TAG, "No valid cached timestamp, returning invalid time");
      return ts; // Will be clamped below if needed
    }

    // Only clamp if last timestamp was also in NTP mode (no high bit set)
    if (!(lastTimestamp_ & ((uint64_t)0x01 << 56)) && ts < lastTimestamp_)
      ts = lastTimestamp_;

    lastTimestamp_ = ts;
    return ts;
  }
}

bool AbsoluteTime::isSynced() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (fallbackMode_) {
    return false;
  }

  // Return cached sync status - set to true by SNTP callback
  // Once synced, we stay synced (SNTP maintains time automatically)
  return synced_;
}

int64_t AbsoluteTime::getLastDriftMs() const {
#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
  return lastHwDriftMs_;
#else
  return 0; // Drift monitoring not available in release builds
#endif
}

double AbsoluteTime::getAverageDriftMs() const {
#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
  return (double)lastHwDriftMs_;
#else
  return 0.0; // Drift monitoring not available in release builds
#endif
}
