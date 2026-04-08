// Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#include "AutoRef.h"
#include "DoubleHitDetector.h"
#include "EventDefinitions.h"
#include "WS2812BLedStrip.h"
#include "esp_task_wdt.h"

static TaskHandle_t AutoRefTask = NULL;

AutoRef::AutoRef() { m_queue = xQueueCreate(30, sizeof(uint32_t)); }

AutoRef::~AutoRef() {}

void AutoRef::AutoRefHandler(void *parameter) {
  AutoRef &ar = AutoRef::getInstance();
  uint32_t event;
  printf("AutoRef task started\n");
  while (true) {
    bool got =
        xQueueReceive(ar.m_queue, &event, 40 / portTICK_PERIOD_MS) == pdPASS;
    esp_task_wdt_reset();

    if (!ar.m_enabled) {
      if (got) {
        uint32_t mainType = event & MAIN_TYPE_MASK;
        if (mainType == EVENT_LONGHIT) {
          uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
          ar.handleLongPress(event, now);
        }
        // Single double-hits ignored when AutoRef is disabled.
      }
      continue;
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (got) {
      uint32_t mainType = event & MAIN_TYPE_MASK;
      if (mainType == EVENT_LIGHTS) {
        uint32_t lights = event & DATA_24BIT_MASK;
        ar.processLights(lights, now);
      } else if (mainType == EVENT_LONGHIT) {
        ar.handleLongPress(event, now);
      } else if (mainType == EVENT_DOUBLEHIT) {
        ar.handleDoubleHit(event, now);
      } else if (mainType == AUTOREF_TIMER_ZERO) {
        ar.handleTimerZero(event, now);
      }
    }
    ar.checkTimeouts(now);
  }
}

void AutoRef::begin() {
  if (m_HasBegun)
    return;
  xTaskCreatePinnedToCore(AutoRefHandler, "AutoRefHandler", 4096, NULL, 1,
                          &AutoRefTask, 1);
  esp_task_wdt_add(AutoRefTask);
  m_HasBegun = true;
  printf("Autoref launched\n");
}

void AutoRef::update(LongHitDetector *subject, uint32_t eventtype) {
  LongHitDetector::printEvent(eventtype);
  xQueueSend(m_queue, &eventtype, 0);
}

void AutoRef::update(DoubleHitDetector *subject, uint32_t eventtype) {
  DoubleHitDetector::printEvent(eventtype);
  xQueueSend(m_queue, &eventtype, 0);
}

void AutoRef::update(FencingStateMachine *subject, uint32_t eventtype) {
  uint32_t mainType = eventtype & MAIN_TYPE_MASK;
  if (mainType == EVENT_TIMER) {
    if ((eventtype & DATA_24BIT_MASK) == 0) {
      // Snapshot pre-transition context synchronously (FSM state not yet
      // changed)
      uint32_t ctx =
          AUTOREF_TIMER_ZERO | ((uint32_t)subject->GetTimerstate() << 8) |
          (subject->GetCurrentRound() >= subject->GetNrOfRounds() ? 0x02
                                                                  : 0x00) |
          (subject->GetScoreLeft() == subject->GetScoreRight() ? 0x01 : 0x00);
      xQueueSend(m_queue, &ctx, 0);
    }
    return;
  }
  if (mainType != EVENT_LIGHTS)
    return;
  uint32_t lights = eventtype & DATA_24BIT_MASK;
  // Only enqueue when lights actually change — prevents 100/s queue flooding
  if (lights == m_lastQueuedLights)
    return;
  m_lastQueuedLights = lights;
  xQueueSend(m_queue, &eventtype, 0);
}

void AutoRef::processLights(uint32_t lights, uint32_t now) {
  switch (m_state) {
  case AR_ARMED:
    processArmed(lights);
    break;
  case AR_WAITING_FOR_LIGHTS_OFF:
    processWaitingForLightsOff(lights, now);
    break;
  case AR_AWAITING_CONFIRMATION:
    processConfirmation(lights, now);
    break;
  case AR_AWARDING:
    m_peakLights |= lights;
    break;
  default:
    break;
  }
  m_prevLights = lights;
}

void AutoRef::processArmed(uint32_t lights) {
  bool redOn = lights & MASK_RED;
  bool greenOn = lights & MASK_GREEN;
  bool whiteL = lights & MASK_WHITE_L;
  bool whiteR = lights & MASK_WHITE_R;
  if (!redOn && !greenOn && !whiteL && !whiteR)
    return;
  // Any light turned on — start accumulating
  m_peakLights = lights;
  m_state = AR_WAITING_FOR_LIGHTS_OFF;
}

void AutoRef::processWaitingForLightsOff(uint32_t lights, uint32_t now) {
  // Accumulate all lights seen while weapons are in contact
  m_peakLights |= lights;
  // Wait until all lights are off before taking a decision
  if (lights == 0) {
    processDecision(m_peakLights, now);
  }
}

void AutoRef::processDecision(uint32_t peakLights, uint32_t now) {
  bool redOn = peakLights & MASK_RED;
  bool greenOn = peakLights & MASK_GREEN;
  bool whiteL = peakLights & MASK_WHITE_L;
  bool whiteR = peakLights & MASK_WHITE_R;

  weapon_t weapon = FencingStateMachine::getInstance().GetMachineWeapon();

  switch (weapon) {
  case EPEE:
    // Single or double — award immediately (both get point on double)
    if (redOn || greenOn)
      award(redOn ? 1 : 0, greenOn ? 1 : 0, now);
    else
      m_state = AR_ARMED; // no valid hit
    break;

  case FOIL:
    if (!redOn && !greenOn) {
      // Off-target only — no point, but still trigger EGPA and restart timer
      m_isOffTargetContinue = true;
      continueMatch(now);
    } else if ((redOn && greenOn) || (redOn && whiteR) || (greenOn && whiteL)) {
      // Double valid, or valid+off-target mix → need confirmation
      m_state = AR_AWAITING_CONFIRMATION;
      m_stateEnteredAt = now;
    } else if (redOn) {
      award(1, 0, now);
    } else {
      award(0, 1, now);
    }
    break;

  case SABRE:
    // White is irrelevant on sabre
    if (redOn && greenOn) {
      m_state = AR_AWAITING_CONFIRMATION;
      m_stateEnteredAt = now;
    } else if (redOn) {
      award(1, 0, now);
    } else if (greenOn) {
      award(0, 1, now);
    } else {
      m_state = AR_ARMED;
    }
    break;

  default:
    m_state = AR_ARMED;
    break;
  }
}

void AutoRef::processConfirmation(uint32_t lights, uint32_t now) {
  // Any light from either side confirms — including white/off-target
  // The confirming side is determined by which side newly lit up
  // Whether a point is awarded depends on what that side had in the original
  // action (m_peakLights)
  uint32_t risingEdge = lights & ~m_prevLights;
  bool newLeft = risingEdge & (MASK_RED | MASK_WHITE_L);
  bool newRight = risingEdge & (MASK_GREEN | MASK_WHITE_R);

  if (newLeft && !newRight) {
    // Left confirms: award point only if left had a valid hit in the original
    // action
    if (m_peakLights & MASK_RED)
      award(1, 0, now);
    else
      continueMatch(now); // left had priority but off-target — no point
  } else if (newRight && !newLeft) {
    // Right confirms: award point only if right had a valid hit in the original
    // action
    if (m_peakLights & MASK_GREEN)
      award(0, 1, now);
    else
      continueMatch(now); // right had priority but off-target — no point
  }
  // Both sides again → stay in AWAITING_CONFIRMATION
}

void AutoRef::handleLongPress(uint32_t lhdEvent, uint32_t now) {
  bool isDouble = (lhdEvent & LONGHIT_IS_DOUBLE) != 0;
  printf("AutoRef: long press double=%d\n", isDouble);

  // Single long hits are intentionally ignored; only a double long press
  // (both fencers holding for the full duration) triggers an action.
  if (!isDouble)
    return;

  if (!m_enabled) {
    // Double long press while disabled: enter AutoRef mode.
    auto &strip = WS2812B_LedStrip::getInstance();
    strip.ClearAll();
    strip.startAnimation(EVENT_WS2812_AUTOREF_MODE);
    setEnabled(true);
    m_state = AR_AWARDING;
    m_stateEnteredAt = now;
    m_prevLights = 0;
    m_peakLights = 0;
    return;
  }

  if (m_state != AR_AWARDING)
    return;

  // Double long press while enabled: full match reset.
  auto &strip = WS2812B_LedStrip::getInstance();
  strip.ClearAll();
  strip.startAnimation(EVENT_WS2812_AUTOREF_MODE);
  sendToFSM(EVENT_UI_INPUT | UI_INPUT_RESET);
  m_stateEnteredAt = now;
  m_prevLights = 0;
  m_peakLights = 0;
}

void AutoRef::handleDoubleHit(uint32_t dhdEvent, uint32_t now) {
  bool hitL = (dhdEvent & DOUBLEHIT_VALID_LEFT) != 0;
  bool hitR = (dhdEvent & DOUBLEHIT_VALID_RIGHT) != 0;
  printf("AutoRef: double-hit L=%d R=%d\n", hitL, hitR);

  if (!m_enabled)
    return;
  if (m_state != AR_AWARDING)
    return;

  auto &strip = WS2812B_LedStrip::getInstance();

  if (hitL) {
    uint32_t newStatus = strip.GetLedStatus();
    newStatus &= ~(MASK_RED | MASK_WHITE_L);
    strip.SetLedStatus(newStatus);
    strip.SetLedStatus(0xff);
    sendToFSM(EVENT_UI_INPUT | UI_INPUT_DECR_SCORE_LEFT);
  }
  if (hitR) {
    uint32_t newStatus = strip.GetLedStatus();
    newStatus &= ~(MASK_GREEN | MASK_WHITE_R);
    strip.SetLedStatus(newStatus);
    strip.SetLedStatus(0xff);
    sendToFSM(EVENT_UI_INPUT | UI_INPUT_DECR_SCORE_RIGHT);
  }
}

void AutoRef::handleTimerZero(uint32_t ctx, uint32_t now) {
  // Unpack snapshot captured synchronously in update() — no FSM getters needed
  TimerState_t timerState = (TimerState_t)((ctx >> 8) & 0xff);
  bool isLastRound = (ctx & 0x02) != 0;
  bool scoresEqual = (ctx & 0x01) != 0;
  printf("AutoRef: timer zero timerState=%d lastRound=%d tied=%d\n", timerState,
         isLastRound, scoresEqual);

  auto &strip = WS2812B_LedStrip::getInstance();

  // Always give an audible signal
  strip.startAnimation(EVENT_WS2812_WARNING | 0x00000003); // 3 beeps

  switch (timerState) {
  case FIGHTING:
    if (!isLastRound) {
      // Not last round: FSM auto-advances to BREAK, start the break timer
      sendToFSM(EVENT_UI_INPUT | UI_INPUT_START_TIMER);
    } else if (scoresEqual) {
      // Last round, tied: draw priority, play EGPA, then start overtime
      sendToFSM(EVENT_UI_INPUT | UI_INPUT_PRIO);
      strip.startAnimation(EVENT_WS2812_ENGARDE_PRETS_ALLEZ);
      m_state = AR_PERIOD_END;
      m_stateEnteredAt = now;
    }
    // Last round, not tied: match ended in FSM, nothing to do
    break;

  case BREAK:
    // Break ended: play EGPA then start next fighting period
    strip.startAnimation(EVENT_WS2812_ENGARDE_PRETS_ALLEZ);
    m_state = AR_PERIOD_END;
    m_stateEnteredAt = now;
    break;

  case ADDITIONAL_MINUTE:
    // Overtime ended; FSM sets MATCH_ENDED, nothing else for AutoRef
    break;

  default:
    break;
  }
}

void AutoRef::checkTimeouts(uint32_t now) {
  switch (m_state) {
  case AR_AWAITING_CONFIRMATION:
    if (now - m_stateEnteredAt >= AUTOREF_CONFIRMATION_TIMEOUT_MS)
      continueMatch(now); // no points, restart
    break;
  case AR_AWARDING: {
    uint32_t delay = m_isOffTargetContinue ? AUTOREF_POST_OFFTARGET_DELAY_MS
                                           : AUTOREF_POST_AWARD_DELAY_MS;
    if (now - m_stateEnteredAt >= delay) {
      WS2812B_LedStrip::getInstance().startAnimation(
          EVENT_WS2812_ENGARDE_PRETS_ALLEZ);
      m_state = AR_EGPA;
      m_stateEnteredAt = now;
    }
    break;
  }
  case AR_EGPA:
    if (now - m_stateEnteredAt >= AUTOREF_EGPA_DURATION_MS) {
      weapon_t w = FencingStateMachine::getInstance().GetMachineWeapon();
      if (w != SABRE)
        sendToFSM(EVENT_UI_INPUT | UI_INPUT_START_TIMER);
      m_state = AR_ARMED;
      m_prevLights = 0;
      m_peakLights = 0;
    }
    break;
  case AR_PERIOD_END:
    if (now - m_stateEnteredAt >= AUTOREF_PERIOD_END_DELAY_MS) {
      sendToFSM(EVENT_UI_INPUT | UI_INPUT_START_TIMER);
      m_state = AR_ARMED;
      m_prevLights = 0;
      m_peakLights = 0;
    }
    break;
  default:
    break;
  }
}

void AutoRef::award(int deltaLeft, int deltaRight, uint32_t now) {
  if (deltaLeft > 0)
    sendToFSM(EVENT_UI_INPUT | UI_INPUT_INCR_SCORE_LEFT);
  if (deltaRight > 0)
    sendToFSM(EVENT_UI_INPUT | UI_INPUT_INCR_SCORE_RIGHT);

  if (isMatchOver())
    m_state = AR_MATCH_OVER;
  else {
    m_isOffTargetContinue = false;
    continueMatch(now);
  }
}

void AutoRef::continueMatch(uint32_t now) {
  m_state = AR_AWARDING;
  m_stateEnteredAt = now;
  m_prevLights = 0;
  m_peakLights = 0;
}

bool AutoRef::isMatchOver() {
  auto &fsm = FencingStateMachine::getInstance();
  int maxScore = getMaxScore();
  return (int)fsm.GetScoreLeft() >= maxScore ||
         (int)fsm.GetScoreRight() >= maxScore;
}

int AutoRef::getMaxScore() {
  // nrOfRounds: 1=pool(5pts), 3=DE(15pts), 9=teams(45pts)
  return FencingStateMachine::getInstance().GetNrOfRounds() * 5;
}

void AutoRef::sendToFSM(uint32_t cmd) {
  FencingStateMachine::getInstance().update((UDPIOHandler *)nullptr, cmd);
}
