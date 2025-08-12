// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#ifndef WS2812B_LEDSTRIP_H
#define WS2812B_LEDSTRIP_H
#include "EventDefinitions.h"
#include "FencingStateMachine.h"
#include "NeoPixelRMT.h"
#include "RepeaterReceiver.h"
#include "Singleton.h"
#include "SubjectObserverTemplate.h"
#include "hardwaredefinition.h"

////////////////////////////////////////////////////////////////////////////////////
// Which pin on the Arduino is connected to the NeoPixels?

// #define PIN 12 // On Trinket or Gemma, suggest changing this to 1
#ifdef FIRST_PROTO
#define PIN 2 // On Trinket or Gemma, suggest changing this to 1
#define BUZZERPIN 0
#define RELATIVE_HIGH LOW
#define RELATIVE_LOW HIGH
#endif

#ifdef SECOND_PROTO
#define PIN 26
#define BUZZERPIN 22
#define RELATIVE_HIGH HIGH
#define RELATIVE_LOW LOW
#endif

// How many NeoPixels are attached to the Arduino?
#define NUMPIXELS 128

// When setting up the NeoPixel library, we tell it how many pixels,
// and which pin to use to send signals. Note that for older NeoPixel
// strips you might need to change the third parameter -- see the
// strandtest example for more information on possible values.

/////////////////////////////////////////////////////////////////////////////////////

#define MASK_RED 0x80
#define MASK_WHITE_L 0x40
#define MASK_ORANGE_L 0x20
#define MASK_ORANGE_R 0x10
#define MASK_WHITE_R 0x08
#define MASK_GREEN 0x04
#define MASK_BUZZ 0x02
#define MASK_REVERSE_COLORS 0x01

#define BRIGHTNESS_LOW 15
#define BRIGHTNESS_NORMAL 30
#define BRIGHTNESS_HIGH 75
#define BRIGHTNESS_ULTRAHIGH 125

enum AnimationType_t { WELCOME, PRIO, ENGARED_PRETS_ALLEZ, WARNING };

class WS2812B_LedStrip : public Observer<FencingStateMachine>,
                         public Observer<RepeaterReceiver>,
                         public SingletonMixin<WS2812B_LedStrip> {
public:
  /** Default destructor */
  virtual ~WS2812B_LedStrip();

  /** Access m_LedStatus
   * \return The current value of m_LedStatus
   */
  unsigned char GetLedStatus() { return m_LedStatus; }
  /** Set m_LedStatus
   * \param val New value to set
   */
  void ClearAll();
  void SetLedStatus(unsigned char val);
  void setRed(bool Value, bool bReverse = false);
  void setWhiteLeft(bool Value, bool inverse = false);
  void setOrangeLeft(bool Value);
  void setOrangeRight(bool Value);
  void setWhiteRight(bool Value, bool inverse = false);
  void setGreen(bool Value, bool bReverse = false);
  void setBuzz(bool Value);
  void myShow() { m_pixels->show(); };
  void SetBrightness(uint8_t val);
  void update(FencingStateMachine *subject, uint32_t eventtype);
  void update(RepeaterReceiver *subject, uint32_t eventtype);
  void ProcessEvents();
  void ProcessEventsBlocking();
  void setGreenPrio(bool Value, bool bReverse = false);
  void setRedPrio(bool Value, bool bReverse = false);
  void AnimateWarning();
  void StartWarning(uint32_t prio);
  void AnimateEngardePretsAllez();
  void StartEngardePretsAllezSequence();
  void setYellowCardLeft(bool Value);
  void setYellowCardRight(bool Value);
  void setRedCardLeft(bool Value);
  void setRedCardRight(bool Value);
  void setYellowPCardLeft(bool Value);
  void setYellowPCardRight(bool Value);
  void setRedPCardLeft(uint8_t nr);
  void setRedPCardRight(uint8_t nr);
  void begin();
  void setUWFTimeLeft(uint8_t tens);
  void setUWFTimeRight(uint8_t tens);
  void SetMirroring(bool value) { m_ReverseColors = value; }
  void ShowWelcomeLights();
  void DoAnimation(uint32_t type);
  /*void StartAsyncWelcomeAnimation();
  bool DoAsyncWelcomeAnimation();*/
  void static LedStripAnimator(void *parameter);
  void startAnimation(uint32_t eventtype);
  void NewAnimatePrio();

protected:
private:
  /** Default constructor */

  friend class SingletonMixin<WS2812B_LedStrip>;
  WS2812B_LedStrip();

  void updateHelper(uint32_t eventtype);
  void setUWFTime(uint8_t tens, uint8_t bottom);
  unsigned char m_LedStatus; //!< Member variable "m_LedStatus"
  NeoPixelRMT *m_pixels;
  bool m_HasBegun = false;
  uint8_t m_Brightness = BRIGHTNESS_NORMAL;
  bool m_Loudness = true;
  uint32_t m_Red;
  uint32_t m_Green;
  uint32_t m_White;
  uint32_t m_Orange;
  uint32_t m_Yellow;
  uint32_t m_Blue;
  uint32_t m_Off;
  uint32_t m_LastEvent = 0;
  bool m_ReverseColors = false;
  bool m_PrioLeft = false;
  bool m_PrioRight = false;
  bool m_YellowCardLeft = false;
  bool m_YellowCardRight = false;
  bool m_RedCardLeft = false;
  bool m_RedCardRight = false;
  uint8_t m_UW2Ftens = 0;
  bool m_YellowPCardLeft = false;
  bool m_YellowPCardRight = false;
  uint8_t m_RedPCardLeft = 0;
  uint8_t m_RedPCardRight = 0;

  QueueHandle_t queue = NULL;
  QueueHandle_t Animationqueue = NULL;
  uint32_t m_NextTimeToTogglePrioLights;
  bool m_Animating = false;
  uint32_t m_counter = 0;
  uint32_t m_targetprio = 0;
  bool m_WarningOngoing = false;
  uint32_t m_NextTimeToToggleBuzzer;
  uint32_t m_warningcounter = 0;
  int m_EngardePretsAllezCounter = 0;
  long m_NextTimeToToggleEGPA = 0;
  bool m_EGAOngoing = false;
  bool m_EGAOBuzzing = false;
  long EGPATiming[13] = {300, 60,  250, 60,  250, 750, 350,
                         750, 300, 100, 250, 100, 300};

  /*bool m_WelcomeAnimationStarted = false;
  long m_WelcomeAnimationNextChange;
  int m_WelcomeAnimationState = 0;*/
};

#endif // WS2812B_LedStrip_H
