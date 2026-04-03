// Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#include "AutoRef.h"
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

    if (!ar.m_enabled)
      continue;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (got && (event & MAIN_TYPE_MASK) == EVENT_LIGHTS) {
      uint32_t lights = event & DATA_24BIT_MASK;
      ar.processLights(lights, now);
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

void AutoRef::update(FencingStateMachine *subject, uint32_t eventtype) {
  if ((eventtype & MAIN_TYPE_MASK) != EVENT_LIGHTS)
    return;
  uint32_t lights = eventtype & DATA_24BIT_MASK;
  // Only enqueue when lights actually change — prevents 100/s queue flooding
  if (lights == m_lastQueuedLights)
    return;
  m_lastQueuedLights = lights;
  xQueueSend(m_queue, &eventtype, 0);
}

void AutoRef::processLights(uint32_t lights, uint32_t now) {
  // Option A long-press detection: re-hit on same side within window after
  // lights-off
  if (m_lightsOffAt > 0 &&
      (now - m_lightsOffAt) < AUTOREF_LONG_PRESS_WINDOW_MS) {
    uint32_t risingEdge = lights & ~m_prevLights;
    bool longL = (risingEdge & MASK_RED) && (m_peakLights & MASK_RED);
    bool longR = (risingEdge & MASK_GREEN) && (m_peakLights & MASK_GREEN);
    if (longL || longR) {
      handleLongPress(longL, longR, now);
      m_prevLights = lights;
      return;
    }
  }

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
  // Centralized: stamp lights-off timestamp whenever lights go to zero
  if (lights == 0 && m_prevLights != 0)
    m_lightsOffAt = now;
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

void AutoRef::handleLongPress(bool longL, bool longR, uint32_t now) {
  m_lightsOffAt = 0; // consume the window
  printf("AutoRef: long press L=%d R=%d state=%d\n", longL, longR, m_state);

  // Clear the corresponding hit light(s) immediately and refresh the display
  auto &strip = WS2812B_LedStrip::getInstance();
  uint32_t newStatus = strip.GetLedStatus();
  if (longL)
    newStatus &= ~(MASK_RED | MASK_WHITE_L);
  if (longR)
    newStatus &= ~(MASK_GREEN | MASK_WHITE_R);
  strip.SetLedStatus(newStatus);
  strip.SetLedStatus(0xff);

  if (longL && longR) {
    // Double long press
    if (m_state == AR_MATCH_OVER) {
      sendToFSM(EVENT_UI_INPUT | UI_INPUT_RESET);
      m_state = AR_ARMED;
      m_prevLights = 0;
      m_peakLights = 0;
    }
    // double long press in other states: TBD per user instruction
  } else if (longL) {
    sendToFSM(EVENT_UI_INPUT | UI_INPUT_DECR_SCORE_LEFT);
  } else {
    sendToFSM(EVENT_UI_INPUT | UI_INPUT_DECR_SCORE_RIGHT);
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
      m_lightsOffAt = 0;
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
  // m_peakLights intentionally NOT cleared — needed for long press detection
  // m_lightsOffAt intentionally NOT cleared — long press window must survive
  // Both are cleared in checkTimeouts when AR_EGPA → AR_ARMED
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
