#ifndef ABSOLUTE_TIME_H
#define ABSOLUTE_TIME_H

#include <mutex>
#include <stdint.h>
#include <string>

class AbsoluteTime {

public:
  // Get the singleton instance
  static AbsoluteTime &getInstance();

  // Initialize and start synchronization
  void begin(const std::string &serverAddress = "openpiste",
             uint32_t syncIntervalSecs = 10,
             const std::string &fallbackIp = "");

  // Set the NTP server address (before begin)
  void setServerAddress(const std::string &address);

  // Set the sync interval in seconds (before begin)
  void setSyncInterval(uint32_t seconds);

  // Get current 64-bit timestamp (NTP or fallback)
  uint64_t getTimestamp();

  // Returns true if synced to server, false if using fallback
  bool isSynced();

  // Drift statistics getters
  int64_t getLastDriftMs() const;
  double getAverageDriftMs() const;

private:
  AbsoluteTime();
  ~AbsoluteTime();
  AbsoluteTime(const AbsoluteTime &) = delete;
  AbsoluteTime &operator=(const AbsoluteTime &) = delete;

  void fallbackToMillis();
  void resolveServerAddress();

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
  // Debug/Info builds only: callback and statistics for monitoring
  static void sntpSyncNotificationCallback(struct timeval *tv);
#endif

  std::string fallbackIp_;
  std::string serverAddress_;
  uint32_t syncIntervalSecs_;
  bool synced_;
  uint64_t lastTimestamp_;
  bool fallbackMode_;
  std::mutex mutex_;

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
  // Sync statistics (debug/info builds only)
  uint32_t syncCount_ = 0;
  uint64_t lastSyncHwMillis_ = 0;  // Hardware timer at last sync
  uint64_t lastSyncNtpMillis_ = 0; // NTP time at last sync
  int64_t lastHwDriftMs_ = 0;      // Hardware crystal drift
#endif
};

#endif // ABSOLUTE_TIME_H
