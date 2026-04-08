// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#pragma once

#include "DebounceTimer.h"
#include "SubjectObserverTemplate.h"
#include <stdint.h>

// LongHitDetector monitors up to four contact channels (valid left/right,
// invalid left/right) and fires an EVENT_LONGHIT notification once any channel
// has been continuously active for the configured duration.
//
// Double-hit detection: if a second channel fires within the configurable
// overlap window after the first, a single event with LONGHIT_IS_DOUBLE is
// emitted instead of two separate events.
//
// Usage:
//   - Epee / Sabre: pass invalidL = false, invalidR = false (default).
//   - Foil:        pass all four channels; invalid = off-target contact.
//   - Call update() every sensor loop iteration.
//   - Register observers via attach(); they receive EVENT_LONGHIT | <flags>.
//   - In the observer, query getLastXxx() for the full hit details.

class LongHitDetector : public Subject<LongHitDetector> {
public:
  LongHitDetector();

  // --- Configuration ---
  void
  setDurationUs(int64_t us); // minimum hold time; default 3 000 000 µs (3 s)

  // --- Per-loop call ---
  // invalidL / invalidR are only relevant for foil; leave as false for
  // epee/sabre.
  void update(bool validL, bool validR, bool invalidL = false,
              bool invalidR = false);

  // --- Control ---
  void reset();

  // --- Debug ---
  // Decode and print an EVENT_LONGHIT event value to stdout.
  static void printEvent(uint32_t eventtype);

  // --- Accessors for observer use after notification ---
  bool getLastValidLeft() const { return last_validL_; }
  bool getLastValidRight() const { return last_validR_; }
  bool getLastInvalidLeft() const { return last_invalidL_; }
  bool getLastInvalidRight() const { return last_invalidR_; }
  bool getLastIsDouble() const { return last_isDouble_; }

private:
  DebounceTimer timer_validL_;
  DebounceTimer timer_validR_;
  DebounceTimer timer_invalidL_;
  DebounceTimer timer_invalidR_;

  // Per-channel latch: set when the threshold is crossed, cleared only when
  // the contact is physically broken. Prevents re-firing during a held contact.
  bool latch_validL_ = false;
  bool latch_validR_ = false;
  bool latch_invalidL_ = false;
  bool latch_invalidR_ = false;

  enum class State { IDLE, ONE_FIRED, SINGLE_EMITTED };
  State state_ = State::IDLE;
  bool first_validL_ = false;
  bool first_validR_ = false;
  bool first_invalidL_ = false;
  bool first_invalidR_ = false;
  int64_t first_fired_at_ = 0;

  bool last_validL_ = false;
  bool last_validR_ = false;
  bool last_invalidL_ = false;
  bool last_invalidR_ = false;
  bool last_isDouble_ = false;

  void emitHit(bool validL, bool validR, bool invalidL, bool invalidR,
               bool isDouble);
  void handleFired(bool firedVL, bool firedVR, bool firedIL, bool firedIR,
                   bool rawVL, bool rawVR, bool rawIL, bool rawIR, int64_t now);
};
