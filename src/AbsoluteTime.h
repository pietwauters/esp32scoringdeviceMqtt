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

  void syncTask();
  void startSyncThread();
  void stopSyncThread();
  void updateTimeFromServer();
  void fallbackToMillis();
  void resolveServerAddress();

  std::string fallbackIp_;

  std::string serverAddress_;
  uint32_t syncIntervalSecs_;
  bool synced_;
  uint64_t lastTimestamp_;
  uint64_t lastMillis_;
  bool fallbackMode_;
  std::mutex mutex_;
  void *syncThreadHandle_; // Opaque handle for sync thread/task

  // Drift statistics
  uint64_t lastNtpSyncMillis_ = 0;
  uint64_t lastNtpSyncTime_ = 0;
  int64_t lastDriftMs_ = 0;
  double driftSum_ = 0;
  uint32_t driftCount_ = 0;
  double smoothedDriftRate_ =
      0.0; // Exponential moving average of drift rate (ms/ms)
};

#endif // ABSOLUTE_TIME_H
