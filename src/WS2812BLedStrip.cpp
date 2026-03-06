// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#include "WS2812BLedStrip.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include <Preferences.h>

TaskHandle_t LedStripTask;
void LedStripHandler(void *parameter) {
  WS2812B_LedStrip &MyLocalLedStrip = WS2812B_LedStrip::getInstance();
  while (true) {
    MyLocalLedStrip.ProcessEventsBlocking();
    esp_task_wdt_reset();
  }
}

TaskHandle_t LedStripAnimationTask;
void WS2812B_LedStrip::LedStripAnimator(void *parameter) {
  WS2812B_LedStrip &MyLocalLedStrip = WS2812B_LedStrip::getInstance();
  uint32_t LastEvent;
  while (true) {
    if (xQueueReceive(MyLocalLedStrip.Animationqueue, &LastEvent,
                      4 / portTICK_PERIOD_MS) == pdPASS) {
      uint32_t event_data = LastEvent;
      MyLocalLedStrip.DoAnimation(event_data);
    }
  }
}

// 3x5 pixel font for digits 0-9 (each row is 3 bits, MSB = leftmost column)
static const uint8_t digitFont3x5[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
    {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
    {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
    {0b111, 0b001, 0b011, 0b001, 0b111}, // 3
    {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
    {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
    {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
    {0b111, 0b001, 0b010, 0b010, 0b010}, // 7
    {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
    {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
};

char Cross[] = {1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1,
                0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0,
                0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0,
                1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1};

WS2812B_LedStrip::WS2812B_LedStrip() {
  // ctor
  gpio_hold_dis((gpio_num_t)PIN);
  /*gpio_config_t io_conf = {};
  io_conf.pin_bit_mask = (1ULL << BUZZERPIN);
  io_conf.mode = GPIO_MODE_OUTPUT;
  gpio_config(&io_conf);
  gpio_set_level(BUZZERPIN, RELATIVE_LOW);*/
  pinMode(BUZZERPIN, OUTPUT);
  digitalWrite(BUZZERPIN, RELATIVE_LOW);
  /*m_pixels = new Adafruit_NeoPixel(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
  m_pixels->fill(m_pixels->Color(0, 0, 0),0,NUMPIXELS);
  SetBrightness(BRIGHTNESS_NORMAL);*/

  queue = xQueueCreate(60, sizeof(int));
  Animationqueue = xQueueCreate(30, sizeof(int));
}
void WS2812B_LedStrip::begin() {

  if (m_HasBegun)
    return;
  Preferences mypreferences;
  mypreferences.begin("scoringdevice", false);
  m_Loudness = !mypreferences.getBool("MuteBuzzer", false);

  mypreferences.end();
  // m_pixels = new Adafruit_NeoPixel(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
  m_pixels = new NeoPixelRMT(NUMPIXELS, (gpio_num_t)PIN);
  m_pixels->begin();
  m_pixels->clear();
  m_pixels->show();
  // m_pixels->fill(m_pixels->Color(0, 0, 0),0,NUMPIXELS);
  SetBrightness(BRIGHTNESS_NORMAL);
  // m_pixels->fill(m_pixels->Color(0, 0, 0),0,NUMPIXELS);
  m_pixels->show();
  xTaskCreatePinnedToCore(LedStripAnimator,   /* Task function. */
                          "LedStripAnimator", /* String with name of task. */
                          16384,              /* Stack size in words. */
                          NULL, /* Parameter passed as input of the task */
                          0,    /* Priority of the task. */
                          &LedStripAnimationTask, /* Task handle. */
                          1);
  xTaskCreatePinnedToCore(LedStripHandler,   /* Task function. */
                          "LedStripHandler", /* String with name of task. */
                          16384,             /* Stack size in words. */
                          NULL, /* Parameter passed as input of the task */
                          6,    /* Priority of the task. */
                          &LedStripTask, /* Task handle. */
                          1);
  esp_task_wdt_add(LedStripTask);
  startAnimation(EVENT_WS2812_WELCOME);
  m_HasBegun = true;
}

void WS2812B_LedStrip::SetBrightness(uint8_t val) {
  m_Brightness = val;
  m_Red = NeoPixelRMT::Color(255, 0, 0, m_Brightness);
  m_Green = NeoPixelRMT::Color(0, 255, 0, m_Brightness);
  m_White = NeoPixelRMT::Color(200, 200, 200, m_Brightness);
  m_Orange = NeoPixelRMT::Color(160, 60, 0, m_Brightness);
  m_Yellow = NeoPixelRMT::Color(204, 168, 0, m_Brightness);
  m_Blue = NeoPixelRMT::Color(0, 0, 255, m_Brightness);
  m_Off = NeoPixelRMT::Color(0, 0, 0, m_Brightness);
}

WS2812B_LedStrip::~WS2812B_LedStrip() {
  // dtor
  delete m_pixels;
}

void WS2812B_LedStrip::ShowPowerFailure() {
  ClearAll();
  m_pixels->setPixelColor(27, m_Red);
  m_pixels->setPixelColor(28, m_Red);
  m_pixels->setPixelColor(27 + 8, m_Red);
  m_pixels->setPixelColor(28 + 8, m_Red);
  m_pixels->setPixelColor(27 + 64, m_Red);
  m_pixels->setPixelColor(28 + 64, m_Red);
  m_pixels->setPixelColor(27 + 8 + 64, m_Red);
  m_pixels->setPixelColor(28 + 8 + 64, m_Red);
  m_pixels->show();
}

void WS2812B_LedStrip::setRed(bool Value, bool bReverse) {
  uint32_t FillColor = m_Red;
  if (bReverse)
    FillColor = m_Green;

  if (Value) {
    // m_pixels->fill(m_pixels->Color(0, 120, 0),0,NUMPIXELS/3);
    // m_pixels->fill(m_Red,0,NUMPIXELS/2);
    for (int i = 0; i < 64; i++) {

      m_pixels->setPixelColor(i, FillColor); // Moderately bright green color.
    }
  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), 0, 64);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}
constexpr int priostartpostion = 0;
void WS2812B_LedStrip::setRedPrio(bool Value, bool bReverse) {
  uint32_t FillColor = m_Red;
  if (bReverse) {
    FillColor = m_Green;
  }

  if (Value) {
    m_pixels->fill(FillColor, priostartpostion, 8);
  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), priostartpostion, 8);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setGreen(bool Value, bool bReverse) {
  uint32_t FillColor = m_Green;
  if (bReverse)
    FillColor = m_Red;
  if (Value) {
    m_pixels->fill(FillColor, 64, 64);

  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), 64, 64);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setGreenPrio(bool Value, bool bReverse) {
  uint32_t FillColor = m_Green;
  if (bReverse) {
    FillColor = m_Red;
  }
  if (Value) {
    m_pixels->fill(FillColor, priostartpostion + 64, 8);
  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), priostartpostion + 64, 8);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setWhiteLeft(bool Value, bool inverse) {
  uint32_t theFillColor = m_White;
  uint32_t theNotFillColor = m_Off;

  if (Value) {
    if (inverse) {
      theFillColor = m_Off;
      theNotFillColor = m_Blue;
    }

    for (int i = 0; i < 64; i++) {
      if (Cross[i])
        m_pixels->setPixelColor(i,
                                theFillColor); // Moderately bright green color.
      else
        m_pixels->setPixelColor(
            i, theNotFillColor); // Moderately bright green color.
    }

  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), 0, 64);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setWhiteRight(bool Value, bool inverse) {
  uint32_t theFillColor = m_White;
  uint32_t theNotFillColor = m_Off;

  if (Value) {
    if (inverse) {
      theFillColor = m_Off;
      theNotFillColor = m_Blue;
    }
    for (int i = 64; i < 128; i++) {
      if (Cross[i - 64])
        m_pixels->setPixelColor(i,
                                theFillColor); // Moderately bright green color.
      else
        m_pixels->setPixelColor(
            i, theNotFillColor); // Moderately bright green color.
    }

  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), 64, 64);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setOrangeLeft(bool Value) {
  if (Value) {
    m_pixels->fill(m_Orange, 40 - 16, 16);
  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), 40 - 16, 16);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setOrangeRight(bool Value) {
  if (Value) {
    m_pixels->fill(m_Orange, 104 - 16, 16);
  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), 104 - 16, 16);
  }
  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setParry(bool Value) {
  return;
  if (Value) {
    m_pixels->fill(m_Blue, 40 - 16, 16);
    m_pixels->fill(m_Blue, 104 - 16, 16);
  } else {
    m_pixels->fill(m_pixels->Color(0, 0, 0), 40 - 16, 16);
    m_pixels->fill(m_pixels->Color(0, 0, 0), 104 - 16, 16);
  }
}
void WS2812B_LedStrip::setYellowCardRight(bool Value) {
  uint32_t theFillColor = m_Off;
  if (Value) {
    theFillColor = m_Yellow;
  }
  // m_pixels->fill(theFillColor, 70 + 5 * 8, 2);
  m_pixels->fill(theFillColor, 70 + 6 * 8, 2);
  m_pixels->fill(theFillColor, 70 + 7 * 8, 2);
}

void WS2812B_LedStrip::setYellowCardLeft(bool Value) {
  uint32_t theFillColor = m_Off;
  if (Value) {
    theFillColor = m_Yellow;
  }
  // m_pixels->fill(theFillColor, 0 + 5 * 8, 2);
  m_pixels->fill(theFillColor, 0 + 6 * 8, 2);
  m_pixels->fill(theFillColor, 0 + 7 * 8, 2);

  // m_pixels->show();   // Send the updated pixel colors to the hardware.
}

void WS2812B_LedStrip::setRedCardRight(bool Value) {
  uint32_t theFillColor = m_Off;
  if (Value) {
    theFillColor = m_Red;
  }
  // m_pixels->fill(theFillColor, 68 + 5 * 8, 2);
  m_pixels->fill(theFillColor, 68 + 6 * 8, 2);
  m_pixels->fill(theFillColor, 68 + 7 * 8, 2);
}

void WS2812B_LedStrip::setRedCardLeft(bool Value) {
  uint32_t theFillColor = m_Off;
  if (Value) {
    theFillColor = m_Red;
  }
  // m_pixels->fill(theFillColor, 2 + 5 * 8, 2);
  m_pixels->fill(theFillColor, 2 + 6 * 8, 2);
  m_pixels->fill(theFillColor, 2 + 7 * 8, 2);
}

void WS2812B_LedStrip::update(FencingStateMachine *subject,
                              uint32_t eventtype) {
  updateHelper(eventtype);
}

void WS2812B_LedStrip::update(RepeaterReceiver *subject, uint32_t eventtype) {
  updateHelper(eventtype);
}

void WS2812B_LedStrip::startAnimation(uint32_t eventtype) {
  xQueueSend(Animationqueue, &eventtype, portMAX_DELAY);
}

void WS2812B_LedStrip::updateHelper(uint32_t eventtype) {
  uint32_t event_data = eventtype & SUB_TYPE_MASK;
  uint32_t maineventtype = eventtype & MAIN_TYPE_MASK;
  if (EVENT_LIGHTS == maineventtype) {
    // SetLedStatus(event_data);
    xQueueSend(queue, &eventtype, portMAX_DELAY);
  }
  switch (maineventtype) {
  case EVENT_UI_INPUT:
    switch (event_data) {
    case UI_CYCLE_BRIGHTNESS:
      switch (m_Brightness) {
      case BRIGHTNESS_LOW:
        SetBrightness(BRIGHTNESS_NORMAL);
        break;
      case BRIGHTNESS_NORMAL:
        SetBrightness(BRIGHTNESS_HIGH);
        break;
      case BRIGHTNESS_HIGH:
        SetBrightness(BRIGHTNESS_ULTRAHIGH);
        break;
      case BRIGHTNESS_ULTRAHIGH:
        SetBrightness(BRIGHTNESS_LOW);
        break;

      default:
        SetBrightness(BRIGHTNESS_NORMAL);
      }
      break;
    }

    break;
  case EVENT_SCORE_LEFT:
    SetLeftScore(event_data);
    break;

  case EVENT_SCORE_RIGHT:
    SetRightScore(event_data);
    break;

  case EVENT_PRIO:
    // StartPrioAnimation(event_data);
    switch (event_data) {
    case 0:

      startAnimation(EVENT_WS2812_PRIO_NONE);
      break;

    case 1:

      startAnimation(EVENT_WS2812_PRIO_LEFT);

      break;

    case 2:

      startAnimation(EVENT_WS2812_PRIO_RIGHT);

      break;
    }

    break;

  case EVENT_YELLOW_CARD_LEFT:
    if (event_data) {
      m_YellowCardLeft = true;

    } else {
      m_YellowCardLeft = false;
    }
    setYellowCardLeft(m_YellowCardLeft);
    SetLedStatus(0xff);
    break;

  case EVENT_YELLOW_CARD_RIGHT:
    if (event_data) {
      m_YellowCardRight = true;
    } else {
      m_YellowCardRight = false;
    }
    setYellowCardRight(m_YellowCardRight);
    SetLedStatus(0xff);
    break;

  case EVENT_RED_CARD_LEFT:

    if (event_data) {
      m_RedCardLeft = true;
    } else {
      m_RedCardLeft = false;
    }
    setRedCardLeft(m_RedCardLeft);
    SetLedStatus(0xff);
    break;

  case EVENT_RED_CARD_RIGHT:

    if (event_data) {
      m_RedCardRight = true;
    } else {
      m_RedCardRight = false;
    }
    setRedCardRight(m_RedCardRight);
    SetLedStatus(0xff);
    break;

  case EVENT_BLACK_CARD_RIGHT:

    if (event_data) {
      m_BlackCardRight = true;
    } else {
      m_BlackCardRight = false;
    }
    setWhiteRight(true, true);
    SetLedStatus(0xff);
    break;

  case EVENT_BLACK_CARD_LEFT:

    if (event_data) {
      m_BlackCardLeft = true;
    } else {
      m_BlackCardLeft = false;
    }
    setWhiteLeft(true, true);
    SetLedStatus(0xff);
    break;

  case EVENT_TOGGLE_BUZZER:
    m_Loudness = !m_Loudness;
    break;

  case EVENT_UW2F_TIMER:
    // m_UW2FSeconds/60)<<16 | (m_UW2FSeconds%60)<<8);

    m_UW2Ftens = (event_data >> 8) / 10;

    mix_t TimeInfo;
    TimeInfo.theDWord = event_data & DATA_24BIT_MASK;
    // m_seconds = TimeInfo.theBytes[1];
    // m_minutes = TimeInfo.theBytes[2];
    // m_hundredths = TimeInfo.theBytes[0];
    m_UW2Ftens = (TimeInfo.theBytes[2] * 60 + TimeInfo.theBytes[1]) / 10;
    setUWFTimeLeft(m_UW2Ftens);
    setUWFTimeRight(m_UW2Ftens);
    SetLedStatus(0xff);
    break;

  case EVENT_TIMER:
    if (!event_data)
      // StartWarning(11);
      startAnimation(EVENT_WS2812_WARNING | 0x0000000b);
    break;

  case EVENT_P_CARD:
    // StateChanged(EVENT_P_CARD |  m_PCardLeft | m_PCardRight << 8);
    mix_t PCardInfo;
    PCardInfo.theDWord = event_data;
    switch (PCardInfo.theBytes[0]) {
    case 0:
      m_YellowPCardLeft = false;
      m_RedPCardLeft = 0;
      break;

    case 1:
      m_YellowPCardLeft = true;
      m_RedPCardLeft = 0;
      break;

    case 2:
      m_YellowPCardLeft = true;
      m_RedPCardLeft = 1;
      break;

    case 4:
      m_YellowPCardLeft = true;
      m_RedPCardLeft = 2;
      break;
    }

    switch (PCardInfo.theBytes[1]) {
    case 0:
      m_YellowPCardRight = false;
      m_RedPCardRight = 0;
      break;

    case 1:
      m_YellowPCardRight = true;
      m_RedPCardRight = 0;
      break;

    case 2:
      m_YellowPCardRight = true;
      m_RedPCardRight = 1;
      break;

    case 4:
      m_YellowPCardRight = true;
      m_RedPCardRight = 2;
      break;
    }

    SetLedStatus(0xff);
    break;
  }
}

void WS2812B_LedStrip::ProcessEvents() {
  if (queue == NULL)
    return;
  if (uxQueueMessagesWaiting(queue) == 0)
    return;

  xQueueReceive(queue, &m_LastEvent, portMAX_DELAY);
  uint32_t event_data = m_LastEvent & SUB_TYPE_MASK;

  SetLedStatus(event_data);
}

void WS2812B_LedStrip::ProcessEventsBlocking() {
  if (xQueueReceive(queue, &m_LastEvent, 4 / portTICK_PERIOD_MS) == pdPASS) {
    uint32_t event_data = m_LastEvent & SUB_TYPE_MASK;
    SetLedStatus(event_data);
  }
}

void WS2812B_LedStrip::SetLedStatus(uint32_t val) {
  if (val != 0xff) {
    if (m_LedStatus == val)
      return;
    m_LedStatus = val;
  }
  if (m_LedStatus & MASK_POWER_PROBLEM) {
    ShowPowerFailure();
    return;
  }
  bool ColoredOn = m_LedStatus & MASK_RED;
  bool ReverseColor = m_LedStatus & MASK_REVERSE_COLORS;
  bool ClearRightPanel =
      (m_LedStatus & MASK_RED) || (m_LedStatus & MASK_WHITE_L);
  bool ClearLeftPanel =
      (m_LedStatus & MASK_GREEN) || (m_LedStatus & MASK_WHITE_R);
  setRed(ColoredOn, ReverseColor);
  if (m_BlackCardLeft) {
    setWhiteLeft(true, true);
  } else {
    if (!ColoredOn) {
      setWhiteLeft(m_LedStatus & MASK_WHITE_L);
      if (!(m_LedStatus & MASK_WHITE_L)) // This is needed because I'm re-using
                                         // the "white part" to show orange
      {
        if (ClearLeftPanel) {
          setRed(false, m_ReverseColors);
        } else {
          showNumberLeft(m_LeftScore); // score first
          if (m_LedStatus & MASK_ORANGE_L)
            setOrangeLeft(true); // orange on top
          if (m_PrioLeft)
            setRedPrio(true, m_ReverseColors); // prio on top
          setYellowCardLeft(m_YellowCardLeft);
          setRedCardLeft(m_RedCardLeft);
          setUWFTimeLeft(m_UW2Ftens);
          setYellowPCardLeft(m_YellowPCardLeft);
          setRedPCardLeft(m_RedPCardLeft);
        }
      }
    }
  }

  ColoredOn = m_LedStatus & MASK_GREEN;
  setGreen(ColoredOn, m_ReverseColors);
  if (m_BlackCardRight) {
    setWhiteRight(true, true);
  } else {
    if (!ColoredOn) {
      setWhiteRight(m_LedStatus & MASK_WHITE_R);
      if (!(m_LedStatus & MASK_WHITE_R)) {
        if (ClearRightPanel) {
          setGreen(false, m_ReverseColors);
        } else {
          showNumberRight(m_RightScore); // score first
          if (m_LedStatus & MASK_ORANGE_R)
            setOrangeRight(true); // orange on top
          if (m_PrioRight)
            setGreenPrio(true, m_ReverseColors); // prio on top
          setYellowCardRight(m_YellowCardRight);
          setRedCardRight(m_RedCardRight);
          setUWFTimeRight(m_UW2Ftens);
          setYellowPCardRight(m_YellowPCardRight);
          setRedPCardRight(m_RedPCardRight);
        }
      }
    }
  }

  setBuzz(m_LedStatus & MASK_BUZZ);
  uint32_t maskedLedstatus =
      m_LedStatus & (MASK_GREEN | MASK_WHITE_R | MASK_WHITE_L | MASK_RED |
                     MASK_ORANGE_L | MASK_ORANGE_R);
  if (!maskedLedstatus) {
    setParry(m_LedStatus & MASK_PARRY);
  }
  m_pixels->show();
}

void WS2812B_LedStrip::ClearAll() {
  setRed(false);
  setWhiteLeft(false);
  setOrangeLeft(false);
  setOrangeRight(false);
  setWhiteRight(false);
  setGreen(false);
  setBuzz(false);
  myShow();
}

#define WARNING_TIME_ON 150
#define WARNING_TIME_Off 40

void WS2812B_LedStrip::AnimateWarning() {
  while (m_WarningOngoing) {

    if (m_warningcounter & 1) {
      m_NextTimeToToggleBuzzer = WARNING_TIME_Off;
      setBuzz(false);
    } else {
      m_NextTimeToToggleBuzzer = WARNING_TIME_ON;
      setBuzz(true);
    }
    m_warningcounter--;
    if (!m_warningcounter) {
      setBuzz(false);
      m_WarningOngoing = false;
    }

    vTaskDelay((m_NextTimeToToggleBuzzer) / portTICK_PERIOD_MS);
  }
}

void WS2812B_LedStrip::StartWarning(uint32_t nr_beeps) {
  m_warningcounter = (nr_beeps * 2) + 1;
  m_NextTimeToToggleBuzzer = WARNING_TIME_ON;
  m_WarningOngoing = true;
}

void WS2812B_LedStrip::AnimateEngardePretsAllez() {
  while (m_EGAOngoing) {
    vTaskDelay((EGPATiming[m_EngardePretsAllezCounter]) / portTICK_PERIOD_MS);
    if (m_EGAOBuzzing) {
      setBuzz(false);
      setWhiteLeft(false);
      setWhiteRight(false);
      myShow();
      m_EGAOBuzzing = false;
      if (m_EngardePretsAllezCounter < 12) {
        m_EngardePretsAllezCounter++;
      } else {
        m_EGAOngoing = false;
      }
    } else {
      if (m_EngardePretsAllezCounter < 11) {
        setBuzz(true);
        setWhiteLeft(true);
        setWhiteRight(true);
        myShow();
        m_EGAOBuzzing = true;
        m_EngardePretsAllezCounter++;
      } else {
        m_EGAOngoing = false;
      }
    }
  }
}
void WS2812B_LedStrip::StartEngardePretsAllezSequence() {
  m_EngardePretsAllezCounter = 0;
  setBuzz(true);
  m_EGAOngoing = true;
  m_EGAOBuzzing = true;
}

void WS2812B_LedStrip::setBuzz(bool Value) {
  if (m_Loudness) {
    if (Value) {
      digitalWrite(BUZZERPIN, RELATIVE_HIGH);
    } else {
      digitalWrite(BUZZERPIN, RELATIVE_LOW);
    }
  }
}

void WS2812B_LedStrip::setUWFTime(uint8_t tens, uint8_t bottom) {
  if (tens > 8)
    tens = 8;
  if (tens == 0) {
    for (int i = 0; i < 8; i++) {
      m_pixels->setPixelColor(bottom - 8 * i, m_Off);
    }
  } else {
    for (int i = 0; i < tens; i++) {
      m_pixels->setPixelColor(bottom - 8 * i, m_Blue);
    }
    for (int i = 5; i < tens; i++) {
      {
        m_pixels->setPixelColor(bottom - i * 8, m_Red);
      }
    }
  }
}

void WS2812B_LedStrip::setUWFTimeLeft(uint8_t tens) { setUWFTime(tens, 63); }

void WS2812B_LedStrip::setUWFTimeRight(uint8_t tens) { setUWFTime(tens, 120); }

// Draw a single 3x5 digit on an 8x8 panel.
// panelOffset: 0 = left panel, 64 = right panel.
// startRow/startCol: top-left corner of the 3x5 glyph (row- and
// col-within-panel). Pixel addressing: panelOffset + row * 8 + col  (same
// convention as rest of code).
void WS2812B_LedStrip::drawDigit3x5(uint8_t panelOffset, uint8_t digit,
                                    uint8_t startRow, uint8_t startCol,
                                    uint32_t color) {
  if (digit > 9)
    digit = 9;
  for (uint8_t r = 0; r < 5; r++) {
    uint8_t row = startRow + r;
    for (uint8_t c = 0; c < 3; c++) {
      uint8_t col = startCol + c;
      bool lit = (digitFont3x5[digit][r] >> (2 - c)) & 1;
      m_pixels->setPixelColor(panelOffset + row * 8 + col, lit ? color : m_Off);
    }
  }
}

// Display a number (0-45) as two 3x5 digits on one 8x8 panel.
// startCol: first column of the 7-wide number rectangle.
// Left panel : startCol=0  → cols 0-6, col 7 reserved.
// Right panel: startCol=1  → cols 1-7, col 0 reserved.
// Rows 0-4 used; rows 5-7 are never touched.
void WS2812B_LedStrip::showNumber(uint8_t panelOffset, uint8_t number,
                                  uint32_t color, uint8_t startCol) {
  if (number > 45)
    number = 45;
  uint8_t tens = number / 10;
  uint8_t units = number % 10;
  // Clear only the 7x5 number rectangle (startCol..startCol+6, rows 0-4)
  for (uint8_t r = 0; r < 5; r++) {
    for (uint8_t c = 0; c < 7; c++) {
      m_pixels->setPixelColor(panelOffset + r * 8 + startCol + c, m_Off);
    }
  }
  if (tens == 0) {
    // Single digit: center it in the 7-wide rectangle (col startCol+2)
    drawDigit3x5(panelOffset, units, 0, startCol + 2, color);
  } else {
    // Two digits: tens flush left, units flush right, gap at startCol+3
    drawDigit3x5(panelOffset, tens, 0, startCol, color);
    drawDigit3x5(panelOffset, units, 0, startCol + 4, color);
  }
  // Note: no show() here - caller (SetLedStatus) calls show() after
  // drawing orange/prio on top, so those always have visual priority.
}

void WS2812B_LedStrip::showNumberLeft(uint8_t number) {
  showNumber(0, number, m_Orange, 0);
}

void WS2812B_LedStrip::showNumberRight(uint8_t number) {
  showNumber(64, number, m_Orange, 1);
}

void WS2812B_LedStrip::setYellowPCardRight(bool Value) {
  uint32_t theFillColor = m_Off;
  if (Value) {
    theFillColor = m_Yellow;
  }
  m_pixels->setPixelColor(121, theFillColor);
  m_pixels->setPixelColor(121 - 8, theFillColor);
}

void WS2812B_LedStrip::setYellowPCardLeft(bool Value) {
  uint32_t theFillColor = m_Off;
  if (Value) {
    theFillColor = m_Yellow;
  }
  m_pixels->setPixelColor(62, theFillColor);
  m_pixels->setPixelColor(62 - 8, theFillColor);
}

void WS2812B_LedStrip::setRedPCardRight(uint8_t nr) {
  uint32_t theFillColor1 = m_Off;
  uint32_t theFillColor2 = m_Off;

  if (nr == 2) {
    // theFillColor1 = m_Red;
    // theFillColor2 = m_Red;
    setWhiteRight(true, true);
    return;
  }
  if (nr == 1) {
    theFillColor1 = m_Red;
  }

  // Red2
  m_pixels->setPixelColor(122 + 1, theFillColor2);
  m_pixels->setPixelColor(122 + 1 - 8, theFillColor2);
  // Red1
  m_pixels->setPixelColor(122, theFillColor1);
  m_pixels->setPixelColor(122 - 8, theFillColor1);
}

void WS2812B_LedStrip::setRedPCardLeft(uint8_t nr) {
  uint32_t theFillColor1 = m_Off;
  uint32_t theFillColor2 = m_Off;

  if (nr == 2) {
    // theFillColor1 = m_Red;
    // theFillColor2 = m_Red;
    setWhiteLeft(true, true);
    return;
  }
  if (nr == 1) {
    theFillColor1 = m_Red;
  }
  m_pixels->setPixelColor(61, theFillColor1);
  m_pixels->setPixelColor(61 - 8, theFillColor1);
  m_pixels->setPixelColor(61 - 1, theFillColor2);
  m_pixels->setPixelColor(61 - 1 - 8, theFillColor2);
}
#define WELCOME_ANIMATION_SPEED 70
const TickType_t xDelay = 5 * WELCOME_ANIMATION_SPEED / portTICK_PERIOD_MS;

void WS2812B_LedStrip::ShowWelcomeLights() {
  for (int i = 0; i < 50; i++) {
    ClearAll();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  setWhiteLeft(true);
  myShow();
  esp_task_wdt_reset();
  vTaskDelay(xDelay);
  setWhiteLeft(false);
  myShow();
  setRed(true);
  myShow();
  esp_task_wdt_reset();
  vTaskDelay(xDelay);
  setRed(false);
  myShow();
  setWhiteRight(true);
  myShow();
  esp_task_wdt_reset();
  vTaskDelay(xDelay);
  setWhiteRight(false);
  myShow();
  setGreen(true);
  myShow();
  esp_task_wdt_reset();
  vTaskDelay(xDelay);
  ClearAll();
  setUWFTimeLeft(1);
  setUWFTimeRight(1);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(2);
  setUWFTimeRight(2);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(3);
  setUWFTimeRight(3);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(4);
  setUWFTimeRight(4);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(5);
  setUWFTimeRight(5);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(6);
  setUWFTimeRight(6);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(7);
  setUWFTimeRight(7);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(8);
  setUWFTimeRight(8);
  myShow();
  vTaskDelay(xDelay);
  setUWFTimeLeft(0);
  setUWFTimeRight(0);
  myShow();
  vTaskDelay(xDelay);
  ClearAll();
  /* for (int i = 0; i < 46; i++) {
     showNumberLeft(i);
     showNumberRight(i);
     vTaskDelay(xDelay);
     ClearAll();
   }*/
  for (int i = 0; i < 10; i++) {
    ClearAll();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
  StateChanged(1);
}

void WS2812B_LedStrip::DoAnimation(uint32_t type) {

  switch (type & 0xffff0000) {
  case EVENT_WS2812_WELCOME:
    ShowWelcomeLights();
    break;

  case EVENT_WS2812_PRIO_NONE:

    setGreenPrio(false, m_ReverseColors);
    setRedPrio(false, m_ReverseColors);
    m_pixels->show(); // clear directly, no animation needed
    m_PrioLeft = false;
    m_PrioRight = false;
    StateChanged(END_OF_PRIO_ANIMATION);
    SetLedStatus(0xff);

    break;

  case EVENT_WS2812_PRIO_RIGHT:

    m_PrioLeft = false;
    m_PrioRight = true;
    m_targetprio = 1;
    m_counter = 17 + 1;
    NewAnimatePrio();
    StateChanged(END_OF_PRIO_ANIMATION);
    break;

  case EVENT_WS2812_PRIO_LEFT:

    m_PrioLeft = true;
    m_PrioRight = false;
    m_targetprio = 2;
    m_counter = 17 + 2;

    NewAnimatePrio();
    StateChanged(END_OF_PRIO_ANIMATION);
    break;

  case EVENT_WS2812_WARNING:
    StartWarning(type & 0x0000ffff);
    AnimateWarning();

    break;

  case EVENT_WS2812_ENGARDE_PRETS_ALLEZ:
    StartEngardePretsAllezSequence();
    AnimateEngardePretsAllez();
    break;
  }
}

void WS2812B_LedStrip::NewAnimatePrio() {
  long sleeptime;
  ClearAll();
  m_NextTimeToTogglePrioLights = millis() + 100 + m_counter * 15;
  m_Animating = true;
  vTaskDelay((100 + m_counter * 15) / portTICK_PERIOD_MS);
  while (m_Animating) {
    if (m_counter & 1) {
      setGreenPrio(true, m_ReverseColors);
      setRedPrio(false, m_ReverseColors);
    } else {
      setGreenPrio(false, m_ReverseColors);
      setRedPrio(true, m_ReverseColors);
    }
    m_pixels->show();
    m_counter--;
    if (m_counter < m_targetprio) {
      m_Animating = false;
    }
    sleeptime = m_NextTimeToTogglePrioLights = 60 + m_counter * 15;
    vTaskDelay(sleeptime / portTICK_PERIOD_MS);
  }
  vTaskDelay(1500 / portTICK_PERIOD_MS);
  // Below is added because the prio is shown in the text display. This is
  // animation only
  setGreenPrio(false, m_ReverseColors);
  setRedPrio(false, m_ReverseColors);
  m_PrioLeft = false;
  m_PrioRight = false;
  SetLedStatus(0xff);
}
