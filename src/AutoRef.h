// Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#ifndef AUTOREF_H
#define AUTOREF_H

#include "DoubleHitDetector.h"
#include "FencingStateMachine.h"
#include "LongHitDetector.h"
#include "Singleton.h"
#include "SubjectObserverTemplate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Timing constants (ms)
#define AUTOREF_CONFIRMATION_TIMEOUT_MS 15000
#define AUTOREF_POST_AWARD_DELAY_MS 5000
#define AUTOREF_POST_OFFTARGET_DELAY_MS 1000
#define AUTOREF_EGPA_DURATION_MS 4000
#define AUTOREF_PERIOD_END_DELAY_MS                                            \
  6000 // warning + EGPA before starting next period
#define AUTOREF_UW2F_PCARD_DELAY_MS 2000  // delay before issuing P-card
#define AUTOREF_UW2F_RESUME_DELAY_MS 2000 // delay after P-card before EGPA

// UW2F timer fires at 60 s elapsed: 1 min, 0 sec = 0x00010000 in lower 24 bits
#define UW2F_TIMER_60S_MARK 0x00010000

enum AutoRefState_t {
  AR_ARMED,
  AR_WAITING_FOR_LIGHTS_OFF, // lights on, accumulating, waiting for them to
                             // clear
  AR_AWAITING_CONFIRMATION,  // double/mixed hit, waiting for single-side
                             // confirmation
  AR_AWARDING,               // point awarded, waiting before EGPA
  AR_EGPA,                   // EGPA animation running
  AR_PERIOD_END, // timer reached zero: warning+EGPA playing, waiting to start
                 // next period
  AR_UW2F_PCARD_WAIT,  // UW2F 60s: waiting before issuing P-card
  AR_UW2F_RESUME_WAIT, // UW2F 60s: waiting after P-card before EGPA
  AR_MATCH_OVER        // match ended, waiting for double-long-press reset
};
class LongHitDetector;
class DoubleHitDetector;

class AutoRef : public Observer<FencingStateMachine>,
                public Observer<LongHitDetector>,
                public Observer<DoubleHitDetector>,
                public SingletonMixin<AutoRef> {
public:
  virtual ~AutoRef();
  void begin();
  void update(FencingStateMachine *subject, uint32_t eventtype);
  void update(LongHitDetector *subject, uint32_t eventtype);
  void update(LongHitDetector *subject, std::string eventtype) { return; };
  void update(DoubleHitDetector *subject, uint32_t eventtype);
  void update(DoubleHitDetector *subject, std::string eventtype) { return; };
  void setEnabled(bool enabled) { m_enabled = enabled; }
  bool isEnabled() const { return m_enabled; }

private:
  friend class SingletonMixin<AutoRef>;
  AutoRef();
  static void AutoRefHandler(void *parameter);

  void processLights(uint32_t lights, uint32_t now);
  void processArmed(uint32_t lights);
  void processWaitingForLightsOff(uint32_t lights, uint32_t now);
  void processDecision(uint32_t peakLights, uint32_t now);
  void processConfirmation(uint32_t lights, uint32_t now);
  void handleLongPress(uint32_t lhdEvent, uint32_t now);
  void handleDoubleHit(uint32_t dhdEvent, uint32_t now);
  void handleTimerZero(uint32_t ctx, uint32_t now);
  void handleUW2FTimerZero(uint32_t now);
  void checkTimeouts(uint32_t now);
  void award(int deltaLeft, int deltaRight, uint32_t now);
  void continueMatch(uint32_t now);
  void GoImmediatelyToArmed(uint32_t now) ;
  bool isMatchOver();
  int getMaxScore();
  void sendToFSM(uint32_t cmd);

  bool m_HasBegun = false;
  bool m_enabled = false;
  QueueHandle_t m_queue = NULL;
  AutoRefState_t m_state = AR_ARMED;
  uint32_t m_stateEnteredAt = 0;
  uint32_t m_prevLights = 0; // last lights value seen (for change detection)
  uint32_t m_peakLights =
      0; // accumulated lights seen during WAITING_FOR_LIGHTS_OFF
  uint32_t m_lastQueuedLights = 0xFFFFFFFF; // for flood prevention in update()
  bool m_isOffTargetContinue =
      false; // true when AR_AWARDING was triggered by off-target (no point)
};

#endif // AUTOREF_H
