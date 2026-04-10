// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#pragma once

#include "SubjectObserverTemplate.h"
#include <stdint.h>

// DoubleHitDetector monitors up to four contact channels (valid left/right,
// invalid left/right) and fires an EVENT_DOUBLEHIT notification when any
// channel is pressed, released, and pressed again within configured time
// windows — i.e. a "double-click" at the hardware level.
//
// Timing parameters (all configurable, defaults shown):
//   minHitUs  — minimum press duration for it to count as a tap (noise
//               filter). Default: 5 000 µs (5 ms).
//   maxHitUs  — maximum press duration for it to still count as a tap;
//               presses held longer are ignored and the detector resets.
//               Default: 500 000 µs (500 ms).
//   maxGapUs  — maximum time between the first release and the start of the
//               second press. Default: 400 000 µs (400 ms).
//
// Usage:
//   - Epee / Sabre: pass invalidL = false, invalidR = false (default).
//   - Foil:         pass all four channels; invalid = off-target contact.
//   - Call update() every sensor loop iteration.
//   - Register observers via attach(); they receive EVENT_DOUBLEHIT | <flags>.
//   - In the observer, query getLastXxx() for which channels were involved.

class DoubleHitDetector : public Subject<DoubleHitDetector> {
public:
  DoubleHitDetector();

  // --- Configuration ---
  void setMinHitUs(int64_t us); // minimum press duration; default 5 000 µs
  void setMaxHitUs(int64_t us); // maximum tap duration;   default 500 000 µs
  void setMaxGapUs(int64_t us); // gap window;             default 400 000 µs

  // --- Per-loop call ---
  // invalidL / invalidR are only relevant for foil; leave as false for
  // epee/sabre.
  void update(bool validL, bool validR, bool invalidL = false,
              bool invalidR = false);

  // --- Control ---
  void reset();

  // --- Debug ---
  // Decode and print an EVENT_DOUBLEHIT event value to stdout.
  static void printEvent(uint32_t eventtype);

  // --- Accessors for observer use after notification ---
  bool getLastValidLeft() const { return last_validL_; }
  bool getLastValidRight() const { return last_validR_; }
  bool getLastInvalidLeft() const { return last_invalidL_; }
  bool getLastInvalidRight() const { return last_invalidR_; }

private:
  int64_t min_hit_us_;
  int64_t max_hit_us_;
  int64_t max_gap_us_;

  // Internal state machine
  enum class State {
    IDLE,
    FIRST_PRESSED,
    FIRST_RELEASED,
    SECOND_PRESSED,
    ABORT
  };
  State state_ = State::IDLE;

  int64_t press_start_ = 0;  // timestamp of the current press start
  int64_t release_time_ = 0; // timestamp of first-press release

  // Channels snapshot taken at the start of each press
  bool first_validL_ = false;
  bool first_validR_ = false;
  bool first_invalidL_ = false;
  bool first_invalidR_ = false;

  bool second_validL_ = false;
  bool second_validR_ = false;
  bool second_invalidL_ = false;
  bool second_invalidR_ = false;

  // Last emitted event channels
  bool last_validL_ = false;
  bool last_validR_ = false;
  bool last_invalidL_ = false;
  bool last_invalidR_ = false;

  void emitDoubleHit(bool validL, bool validR, bool invalidL, bool invalidR);
};
