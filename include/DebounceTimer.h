#pragma once
#include <esp_timer.h>
#include <stdint.h>

class DebounceTimer {
public:
  DebounceTimer() : start_time_(0), last_ok_(false) {}

  // Call this every loop with your condition
  bool update(bool condition) {
    int64_t now = esp_timer_get_time();
    if (condition) {
      if (start_time_ == 0)
        start_time_ = now;
      last_ok_ = (now - start_time_) >= required_us_;
      return last_ok_;
    } else {
      start_time_ = 0;
      last_ok_ = false;
      return false;
    }
  }
  bool isOK() const { return last_ok_; }

  // Reset the timer
  // This is useful if you want to stop the timer without waiting for the full
  void reset() {
    start_time_ = 0;
    last_ok_ = false;
  }
  // Overloaded version
  void reset(int64_t us) {
    start_time_ = 0;
    last_ok_ = false;
    required_us_ = us;
  }
  void setRequiredUs(int64_t us) { required_us_ = us; }

private:
  int64_t required_us_;
  int64_t start_time_;
  bool last_ok_;
};

/*
class SabreTimer {
public:
  SabreTimer() : start_time_(0), last_ok_(false) {}

  // Call this every loop with your condition
  bool update(bool condition) {
    int64_t now = esp_timer_get_time();
    if (condition) {
      if (start_time_ == 0)
        start_time_ = now;
      last_ok_ = (now - start_time_) >= required_us_;
      return last_ok_;
    } else {
      start_time_ = 0;
      last_ok_ = false;
      return false;
    }
  }
  bool isOK() const { return last_ok_; }

  // Reset the timer
  // This is useful if you want to stop the timer without waiting for the full
  void reset() {
    start_time_ = 0;
    last_ok_ = false;
  }


private:
  int64_t required_us_;
  int64_t start_time_;
  bool last_ok_;
};
*/