// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "LongHitDetector.h"
#include "EventDefinitions.h"
#include "esp_timer.h"
#include <cinttypes>

static constexpr int64_t DEFAULT_DURATION_US = 3000000LL; // 3 s

LongHitDetector::LongHitDetector() {
  timer_validL_.setRequiredUs(DEFAULT_DURATION_US);
  timer_validR_.setRequiredUs(DEFAULT_DURATION_US);
  timer_invalidL_.setRequiredUs(DEFAULT_DURATION_US);
  timer_invalidR_.setRequiredUs(DEFAULT_DURATION_US);
}

void LongHitDetector::setDurationUs(int64_t us) {
  timer_validL_.setRequiredUs(us);
  timer_validR_.setRequiredUs(us);
  timer_invalidL_.setRequiredUs(us);
  timer_invalidR_.setRequiredUs(us);
}

void LongHitDetector::reset() {
  timer_validL_.reset();
  timer_validR_.reset();
  timer_invalidL_.reset();
  timer_invalidR_.reset();
  latch_validL_ = false;
  latch_validR_ = false;
  latch_invalidL_ = false;
  latch_invalidR_ = false;
  state_ = State::IDLE;
}

void LongHitDetector::update(bool validL, bool validR, bool invalidL,
                             bool invalidR) {
  // Clear the latch as soon as contact is broken so the channel can re-arm.
  if (!validL)
    latch_validL_ = false;
  if (!validR)
    latch_validR_ = false;
  if (!invalidL)
    latch_invalidL_ = false;
  if (!invalidR)
    latch_invalidR_ = false;

  // Feed each timer: suppress input while latched so they cannot re-fire
  // during a sustained contact that has already produced an event.
  bool okVL = timer_validL_.update(validL && !latch_validL_);
  bool okVR = timer_validR_.update(validR && !latch_validR_);
  bool okIL = timer_invalidL_.update(invalidL && !latch_invalidL_);
  bool okIR = timer_invalidR_.update(invalidR && !latch_invalidR_);

  // A channel fires exactly once per contact press (rising edge of okXx).
  // Set the latch immediately so the timer sees 'false' from the next cycle.
  bool firedVL = okVL;
  if (firedVL)
    latch_validL_ = true;
  bool firedVR = okVR;
  if (firedVR)
    latch_validR_ = true;
  bool firedIL = okIL;
  if (firedIL)
    latch_invalidL_ = true;
  bool firedIR = okIR;
  if (firedIR)
    latch_invalidR_ = true;

  bool anyFired = firedVL || firedVR || firedIL || firedIR;

  // Stay quiet when nothing happened and no pending window to watch.
  if (!anyFired && state_ == State::IDLE)
    return;

  handleFired(firedVL, firedVR, firedIL, firedIR, validL, validR, invalidL,
              invalidR, esp_timer_get_time());
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void LongHitDetector::handleFired(bool firedVL, bool firedVR, bool firedIL,
                                  bool firedIR, bool rawVL, bool rawVR,
                                  bool rawIL, bool rawIR, int64_t now) {
  bool anyFired = firedVL || firedVR || firedIL || firedIR;
  int count = (int)firedVL + (int)firedVR + (int)firedIL + (int)firedIR;

  switch (state_) {

  case State::IDLE:
    if (!anyFired)
      return;
    if (count >= 2) {
      // Two channels crossed the threshold in the same loop cycle.
      emitHit(firedVL, firedVR, firedIL, firedIR, true);
    } else {
      // One side fired. Emit single immediately UNLESS the other side is
      // already pressing (raw contact) — in that case wait: the second side
      // may still reach 3 s (→ double) or release early (→ single for first).
      bool otherPressing = (!firedVL && rawVL) || (!firedVR && rawVR) ||
                           (!firedIL && rawIL) || (!firedIR && rawIR);
      if (!otherPressing) {
        emitHit(firedVL, firedVR, firedIL, firedIR, false);
        // Stay watching in case the other side starts and reaches 3 s.
        first_validL_ = firedVL;
        first_validR_ = firedVR;
        first_invalidL_ = firedIL;
        first_invalidR_ = firedIR;
        state_ = State::SINGLE_EMITTED;
      } else {
        first_validL_ = firedVL;
        first_validR_ = firedVR;
        first_invalidL_ = firedIL;
        first_invalidR_ = firedIR;
        first_fired_at_ = now;
        state_ = State::ONE_FIRED;
      }
    }
    break;

  case State::ONE_FIRED: {
    // We entered because the second side was pressing when the first fired.
    // Wait for one of two outcomes:
    //   • second side fires            → double
    //   • second side releases early   → single for first
    //
    // "Second side" = any channel not in the first-fired set.
    bool secondRaw = (!first_validL_ && rawVL) || (!first_validR_ && rawVR) ||
                     (!first_invalidL_ && rawIL) || (!first_invalidR_ && rawIR);

    if (anyFired) {
      // Second side reached its threshold → double.
      emitHit(first_validL_ || firedVL, first_validR_ || firedVR,
              first_invalidL_ || firedIL, first_invalidR_ || firedIR, true);
      state_ = State::IDLE;
    } else if (!secondRaw) {
      // Second side released without reaching 3 s → single for first.
      // Stay in SINGLE_EMITTED: the second side could try again while first
      // is still held.
      emitHit(first_validL_, first_validR_, first_invalidL_, first_invalidR_,
              false);
      state_ = State::SINGLE_EMITTED;
    }
    // else: second side still pressing, keep waiting.
    break;
  }

  case State::SINGLE_EMITTED: {
    // A single was already emitted. Keep watching as long as anyone is still
    // pressing: the other side may yet reach 3 s and produce a double.
    bool anyRaw = rawVL || rawVR || rawIL || rawIR;
    if (anyFired) {
      // Other side reached 3 s while first was still active → double.
      emitHit(first_validL_ || firedVL, first_validR_ || firedVR,
              first_invalidL_ || firedIL, first_invalidR_ || firedIR, true);
      state_ = State::IDLE;
    } else if (!anyRaw) {
      // Everyone released → nothing more pending.
      state_ = State::IDLE;
    }
    // else: someone still pressing, keep watching.
    break;
  }
  }
}

void LongHitDetector::emitHit(bool validL, bool validR, bool invalidL,
                              bool invalidR, bool isDouble) {
  last_validL_ = validL;
  last_validR_ = validR;
  last_invalidL_ = invalidL;
  last_invalidR_ = invalidR;
  last_isDouble_ = isDouble;

  uint32_t evt = EVENT_LONGHIT;
  if (validL)
    evt |= LONGHIT_VALID_LEFT;
  if (validR)
    evt |= LONGHIT_VALID_RIGHT;
  if (invalidL)
    evt |= LONGHIT_INVALID_LEFT;
  if (invalidR)
    evt |= LONGHIT_INVALID_RIGHT;
  if (isDouble)
    evt |= LONGHIT_IS_DOUBLE;

  notify(evt);
}

void LongHitDetector::printEvent(uint32_t eventtype) {
  printf("LongHit event 0x%08" PRIx32 ":", eventtype);
  if (eventtype & LONGHIT_VALID_LEFT)
    printf(" VALID_LEFT");
  if (eventtype & LONGHIT_VALID_RIGHT)
    printf(" VALID_RIGHT");
  if (eventtype & LONGHIT_INVALID_LEFT)
    printf(" INVALID_LEFT");
  if (eventtype & LONGHIT_INVALID_RIGHT)
    printf(" INVALID_RIGHT");
  if (eventtype & LONGHIT_IS_DOUBLE)
    printf(" DOUBLE");
  printf("\n");
}
