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

class DoubleDebouncer {
public:
  DoubleDebouncer()
      : current_state_(WAITING_ON), start_time_(0), last_ok_(false) {}

  // Call this every loop with your condition
  bool update(bool condition) {
    int64_t now = esp_timer_get_time();

    switch (current_state_) {
    case WAITING_ON:
      if (condition) {
        if (start_time_ == 0)
          start_time_ = now;
        if ((now - start_time_) >= required_on_us_) {
          current_state_ = WAITING_OFF;
          last_ok_ = true;
          start_time_ = 0; // Reset for next phase
        }
      } else {
        start_time_ = 0;
        last_ok_ = false;
      }
      break;

    case WAITING_OFF:
      if (!condition) {
        if (start_time_ == 0)
          start_time_ = now;
        if ((now - start_time_) >= required_off_us_) {
          current_state_ = WAITING_ON;
          last_ok_ = false;
          start_time_ = 0; // Reset for next phase
        }
      } else {
        start_time_ = 0; // Reset timer, stay in WAITING_OFF
      }
      break;
    }

    return last_ok_;
  }

  bool isOK() const { return last_ok_; }

  // Reset to initial state
  void reset() {
    current_state_ = WAITING_ON;
    start_time_ = 0;
    last_ok_ = false;
  }

  void setRequiredOnUs(int64_t us) { required_on_us_ = us; }
  void setRequiredOffUs(int64_t us) { required_off_us_ = us; }

private:
  enum State { WAITING_ON, WAITING_OFF };
  State current_state_;
  int64_t required_on_us_;
  int64_t required_off_us_;
  int64_t start_time_;
  bool last_ok_;
};