// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "DoubleHitDetector.h"
#include "EventDefinitions.h"
#include "esp_timer.h"
#include <cinttypes>

static constexpr int64_t DEFAULT_MIN_HIT_US = 5000LL;   //   5 ms
static constexpr int64_t DEFAULT_MAX_HIT_US = 500000LL; // 500 ms
static constexpr int64_t DEFAULT_MAX_GAP_US = 400000LL; // 400 ms

DoubleHitDetector::DoubleHitDetector()
    : min_hit_us_(DEFAULT_MIN_HIT_US), max_hit_us_(DEFAULT_MAX_HIT_US),
      max_gap_us_(DEFAULT_MAX_GAP_US) {}

void DoubleHitDetector::setMinHitUs(int64_t us) { min_hit_us_ = us; }
void DoubleHitDetector::setMaxHitUs(int64_t us) { max_hit_us_ = us; }
void DoubleHitDetector::setMaxGapUs(int64_t us) { max_gap_us_ = us; }

void DoubleHitDetector::reset() {
  state_ = State::IDLE;
  press_start_ = 0;
  release_time_ = 0;
  first_validL_ = first_validR_ = first_invalidL_ = first_invalidR_ = false;
  second_validL_ = second_validR_ = second_invalidL_ = second_invalidR_ = false;
  last_validL_ = last_validR_ = last_invalidL_ = last_invalidR_ = false;
}

void DoubleHitDetector::update(bool validL, bool validR, bool invalidL,
                               bool invalidR) {
  bool anyActive = validL || validR || invalidL || invalidR;
  int64_t now = esp_timer_get_time();

  switch (state_) {

  case State::IDLE:
    if (anyActive) {
      press_start_ = now;
      first_validL_ = validL;
      first_validR_ = validR;
      first_invalidL_ = invalidL;
      first_invalidR_ = invalidR;
      state_ = State::FIRST_PRESSED;
    }
    break;

  case State::FIRST_PRESSED: {
    int64_t held = now - press_start_;
    if (held > max_hit_us_) {
      // Held too long to be a tap — drain until release.
      state_ = State::ABORT;
    } else if (!anyActive) {
      // Released.
      if (held >= min_hit_us_) {
        // Valid first tap — open the gap window.
        release_time_ = now;
        state_ = State::FIRST_RELEASED;
      } else {
        // Too short — treat as noise.
        state_ = State::IDLE;
      }
    }
    // else: still pressed within window, keep waiting.
    break;
  }

  case State::FIRST_RELEASED: {
    if ((now - release_time_) > max_gap_us_) {
      // Gap window expired without a second press.
      state_ = State::IDLE;
      break;
    }
    if (anyActive) {
      // Second press began.
      press_start_ = now;
      second_validL_ = validL;
      second_validR_ = validR;
      second_invalidL_ = invalidL;
      second_invalidR_ = invalidR;
      state_ = State::SECOND_PRESSED;
    }
    break;
  }

  case State::SECOND_PRESSED: {
    int64_t held = now - press_start_;
    if (held > max_hit_us_) {
      // Second press held too long — not a tap, abort.
      state_ = State::ABORT;
    } else if (!anyActive) {
      // Released.
      if (held >= min_hit_us_) {
        // Valid second tap — fire the double-hit event.
        emitDoubleHit(first_validL_ || second_validL_,
                      first_validR_ || second_validR_,
                      first_invalidL_ || second_invalidL_,
                      first_invalidR_ || second_invalidR_);
        state_ = State::IDLE;
      } else {
        // Too short — treat as noise, discard both taps.
        state_ = State::IDLE;
      }
    }
    // else: still pressed within window, keep waiting.
    break;
  }

  case State::ABORT:
    // Wait for all channels to be released before accepting new input.
    if (!anyActive)
      state_ = State::IDLE;
    break;
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void DoubleHitDetector::emitDoubleHit(bool validL, bool validR, bool invalidL,
                                      bool invalidR) {
  last_validL_ = validL;
  last_validR_ = validR;
  last_invalidL_ = invalidL;
  last_invalidR_ = invalidR;

  uint32_t evt = EVENT_DOUBLEHIT;
  if (validL)
    evt |= DOUBLEHIT_VALID_LEFT;
  if (validR)
    evt |= DOUBLEHIT_VALID_RIGHT;
  if (invalidL)
    evt |= DOUBLEHIT_INVALID_LEFT;
  if (invalidR)
    evt |= DOUBLEHIT_INVALID_RIGHT;

  notify(evt);
}

void DoubleHitDetector::printEvent(uint32_t eventtype) {
  printf("DoubleHit event 0x%08" PRIx32 ":", eventtype);
  if (eventtype & DOUBLEHIT_VALID_LEFT)
    printf(" VALID_LEFT");
  if (eventtype & DOUBLEHIT_VALID_RIGHT)
    printf(" VALID_RIGHT");
  if (eventtype & DOUBLEHIT_INVALID_LEFT)
    printf(" INVALID_LEFT");
  if (eventtype & DOUBLEHIT_INVALID_RIGHT)
    printf(" INVALID_RIGHT");
  printf("\n");
}
