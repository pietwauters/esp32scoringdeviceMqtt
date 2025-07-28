#pragma once
#include <esp_timer.h>
#include <stdint.h>

class DebounceTimer {
public:
  DebounceTimer() : start_time_(0) {}

  // Call this every loop with your condition
  bool update(bool condition) {
    uint64_t now = esp_timer_get_time();
    if (condition) {
      if (start_time_ == 0)
        start_time_ = now;
      return (now - start_time_) >= required_us_;
    } else {
      start_time_ = 0;
      return false;
    }
  }
  bool isOK() {
    if (start_time_ == 0) {
      return false;
    }
    uint64_t now = esp_timer_get_time();
    return (now - start_time_) >= required_us_;
  }

  // Reset the timer
  // This is useful if you want to stop the timer without waiting for the full
  void reset() { start_time_ = 0; }
  void setRequiredUs(uint64_t us) { required_us_ = us; }

private:
  uint64_t required_us_;
  uint64_t start_time_;
};
