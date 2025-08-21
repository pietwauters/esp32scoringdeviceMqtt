// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#ifndef WEAPONSENSOR_H
#define WEAPONSENSOR_H
#include "DebounceTimer.h"
#include "Singleton.h"
#include "SubjectObserverTemplate.h"
#include "TimingConstants.h"
#include "hardwaredefinition.h"
#include "weaponenum.h"
#include <Arduino.h>
#include <cinttypes>
#include <cstddef>
#include <cstdio>

#define MASK_RED 0x80
#define MASK_WHITE_L 0x40
#define MASK_ORANGE_L 0x20
#define MASK_ORANGE_R 0x10
#define MASK_WHITE_R 0x08
#define MASK_GREEN 0x04
#define MASK_BUZZ 0x02
#define LIGHTS_DURATION_MS 2000

// enum weapon_t {FOIL, EPEE, SABRE, UNKNOWN};
enum weapon_detection_mode_t { MANUAL, AUTO, HYBRID };

typedef struct MeasurementCtlStruct {
  uint8_t IODirection;
  uint8_t IOValues;
  uint8_t ADChannel;
  int ADThreashold;
} MeasurementCtl;

class MultiWeaponSensor : public Subject<MultiWeaponSensor>,
                          public SingletonMixin<MultiWeaponSensor> {
public:
  void begin();
  /** Default destructor */
  virtual ~MultiWeaponSensor();

  void SensorStateChanged(uint32_t eventtype) { notify(eventtype); }

  /** Access m_ActualWeapon
   * \return The current value of m_ActualWeapon
   */
  weapon_t GetActualWeapon() { return m_ActualWeapon; }
  /** Set m_ActualWeapon
   * \param val New value to set
   */
  void SetActualWeapon(weapon_t val) {
    m_ActualWeapon = val;
    DoReset();
  }
  /** Access m_DetectedWeapon
   * \return The current value of m_DetectedWeapon
   */
  weapon_t GetDetectedWeapon() { return m_DetectedWeapon; }

  void DoSabre();
  void DoEpee(void);
  void DoFoil(void);
  void Skip_phase();
  void DoFullScan();
  bool Wait_For_Next_Timer_Tick();
  unsigned char get_Lights() { return Lights; };
  void BlockAllNewHits() {
    SignalLeft = true;
    SignalRight = true;
  };
  void AllowAllNewHits() {
    SignalLeft = false;
    SignalRight = false;
  };
  void Setweapon_detection_mode(weapon_detection_mode_t mode) {
    m_DectionMode = mode;
  };

protected:
private:
  friend class SingletonMixin<MultiWeaponSensor>;
  /** Default constructor */
  MultiWeaponSensor();
  bool Do_Common_Start();
  // void Skip_phase();
  void HandleLights();
  // void DoEpee(void);
  // void DoFoil(void);
  // void DoSabre();
  void resetLongDebouncers();
  void DoReset(void);
  void StartLock(int TimeToLock);
  bool IsLocked();
  bool OKtoReset();
  weapon_t GetWeapon();
  int tempADValue = 0;

  weapon_t m_ActualWeapon = EPEE;   //!< Member variable "m_ActualWeapon"
  weapon_t m_DetectedWeapon = EPEE; //!< Member variable "m_DetectedWeapon"
  weapon_detection_mode_t m_DectionMode = AUTO;
  bool m_NoNewHitsAllowed = false;

  bool SignalLeft;
  bool SignalRight;
  bool WaitingForResetStarted;
  bool NotConnectedLeft;
  bool NotConnectedRight;
  bool bAutoDetect;
  bool bPreventBuzzer = false;
  // below are the counters used for "debouncing"
  // "normal" counters are used for contact duration of hits

  DebounceTimer Debounce_b1;
  DebounceTimer Debounce_b2;
  DebounceTimer Debounce_c1;
  DebounceTimer Debounce_c2;

  DoubleDebouncer Debounce_SabreWhite_l;
  DoubleDebouncer Debounce_SabreWhite_r;

  // below const values are used to make timing calibration possible without
  // having constants in the code

  int Const_FOIL_PARRY_ON_TIME;
  int Const_FOIL_PARRY_OFF_TIME;

  // "long" debouncers are used for automatic weapon detection
  DebounceTimer DebounceLong_b1;
  DebounceTimer DebounceLong_b2;
  DebounceTimer DebounceLong_c1;
  DebounceTimer DebounceLong_c2;

  // used to switch to Sabre mode when in Foil
  // or to switch to foil or Sabre when in Epee mode
  // These are checks from left to right or inverse
  DebounceTimer DebounceLong_al_cr;
  DebounceTimer DebounceLong_ar_cl;

  // used to switch to Epee mode when in Foil or Sabre
  // These are check from left to left and right to right
  DebounceTimer DebounceLong_al_cl;
  DebounceTimer DebounceLong_ar_cr;

  // counters introduced for automatic switch to epee if no foil or sabre
  // connected
  DebounceTimer Debounce_NotConnected;
  DebounceTimer Debounce_AtLeastOneNotConnected;

  unsigned char Lights;

  int BlockCounter;
  bool MaybeSignalRight;
  bool MaybeSignalLeft;
  bool BlockWhipover;
  bool WeaponContact;

  bool Red;
  bool WhiteL;
  bool OrangeR;
  bool Green;
  bool WhiteR;
  bool OrangeL;
  bool Buzz;
  bool PossiblyRed;
  bool PossiblyGreen;
  bool WeHaveBlockedAhit;

  int BlockedAHitCounter;

  DoubleDebouncer Debounce_Parry;
  bool previousParryState = false;

  int TimeOfLock;
  bool LockStarted;
  int TimeToReset;
  int LightsDuration = LIGHTS_DURATION_MS;

  uint32_t ShortIndicatorsDebouncer = 0;
};

extern int AxXy_100_Ohm;

#endif // WEAPONSENSOR_H
