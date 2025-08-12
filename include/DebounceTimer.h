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

      last_ok_ = false;
      start_time_ = 0;
      return false;
    }
  }
  bool isOK() const { return last_ok_; }

  // Reset the timer
  // This is useful if you want to stop the timer without waiting for the full
  void reset() {
    start_time_ = 0;
    last_ok_ = false;
    DosSantosApplied_ = false;
  }
  // Overloaded version
  void reset(int64_t us) {
    reset();
    required_us_ = us;
  }
  void setRequiredUs(int64_t us) { required_us_ = us; }
  void setDosSantosMarginUs(int64_t us) { DosSantosMargin_ = us; }
  void applyDosSantosMarginUs(bool Update = true) {
    if (!DosSantosApplied_) {
      required_us_ -= DosSantosMargin_;
      if (Update) {
        update(true);
      }
      DosSantosApplied_ = true;
    }
  }

private:
  int64_t required_us_;
  int64_t start_time_;
  int64_t DosSantosMargin_;
  bool last_ok_;
  bool DosSantosApplied_ = false;
};