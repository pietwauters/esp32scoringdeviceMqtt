// Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#ifndef AUTOREF_H
#define AUTOREF_H

#include "FencingStateMachine.h"
#include "Singleton.h"
#include "SubjectObserverTemplate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Timing constants (ms)
#define AUTOREF_CONFIRMATION_TIMEOUT_MS 15000
#define AUTOREF_POST_AWARD_DELAY_MS 12000
#define AUTOREF_POST_OFFTARGET_DELAY_MS 8000
#define AUTOREF_EGPA_DURATION_MS 4000
#define AUTOREF_LONG_PRESS_WINDOW_MS                                           \
  300 // re-hit within this window = long press

enum AutoRefState_t {
  AR_ARMED,
  AR_WAITING_FOR_LIGHTS_OFF, // lights on, accumulating, waiting for them to
                             // clear
  AR_AWAITING_CONFIRMATION,  // double/mixed hit, waiting for single-side
                             // confirmation
  AR_AWARDING,               // point awarded, waiting before EGPA
  AR_EGPA,                   // EGPA animation running
  AR_MATCH_OVER              // match ended, waiting for double-long-press reset
};

class AutoRef : public Observer<FencingStateMachine>,
                public SingletonMixin<AutoRef> {
public:
  virtual ~AutoRef();
  void begin();
  void update(FencingStateMachine *subject, uint32_t eventtype);
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
  void handleLongPress(bool longL, bool longR, uint32_t now);
  void checkTimeouts(uint32_t now);
  void award(int deltaLeft, int deltaRight, uint32_t now);
  void continueMatch(uint32_t now);
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
  uint32_t m_lightsOffAt =
      0; // timestamp when lights last went to 0 (Option A long-press)
};

#endif // AUTOREF_H
