// Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#include "3WeaponSensor.h"
#include "FastADC1.h"
#include "FastGPIOSettings.h"

constexpr int AxMaxValue = 1800;
constexpr int BCMaxValue = 1200;
inline bool HitOnLame_l() {
  Set_IODirectionAndValue(IODirection_al_cr, IOValues_al_cr);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cr_analog);
  return (tempADValue > AxMaxValue);
};
inline bool WireOK_l() {
  Set_IODirectionAndValue(IODirection_al_bl, IOValues_al_bl);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)bl_analog);
  return (tempADValue < AxMaxValue);
};

inline bool EpeeHit_l() {
  Set_IODirectionAndValue(IODirection_al_cl, IOValues_al_cl);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cl_analog);
  return (tempADValue > BCMaxValue);
};

inline bool HitOnLame_r() {
  Set_IODirectionAndValue(IODirection_ar_cl, IOValues_ar_cl);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cl_analog);
  return (tempADValue > AxMaxValue);
};
inline bool WireOK_r() {
  Set_IODirectionAndValue(IODirection_ar_br, IOValues_ar_br);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)br_analog);
  return (tempADValue < AxMaxValue);
};

inline bool EpeeHit_r() {
  Set_IODirectionAndValue(IODirection_ar_cr, IOValues_ar_cr);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cr_analog);
  return (tempADValue > AxMaxValue);
};

inline bool Parry() {
  Set_IODirectionAndValue(IODirection_br_bl, IOValues_br_bl);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)bl_analog);
  return (tempADValue > AxMaxValue);
};

enum SabreState { IDLE, DEBOUNCING, DEBOUNCED, LOCKING, LOCKED };

void MultiWeaponSensor::DoSabre(void) {
  bool cl, cr;
  static SabreState state = IDLE;
  static int SubsampleCounter = 0;
  bool tempRed = false;
  bool tempGreen = false;

  Set_IODirectionAndValue(IODirection_al_cr, IOValues_al_cr);
  tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cr_analog);
  cl = (tempADValue > AxMaxValue);

  Set_IODirectionAndValue(IODirection_ar_cl, IOValues_ar_cl);
  tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cl_analog);
  cr = (tempADValue > AxMaxValue);

  Debounce_c1.update(cl);
  Debounce_c2.update(cr);
  DebounceLong_al_cr.update(cr);
  DebounceLong_ar_cl.update(cl);

  switch (state) {
  case IDLE:

    if (cl || cr) {
      state = DEBOUNCING;
      break;
    } else {
      // Do one of the optional checks
      vTaskDelay(0);
      switch (SubsampleCounter) {
      case 0:
        SubsampleCounter = 1;
        if (Debounce_b1.update(WireOK_l())) {
          WhiteL = true;
        } else {
          WhiteL = false;
        }
        break;
      case 1:
        SubsampleCounter = 2;
        if (Debounce_b2.update(WireOK_r())) {
          WhiteR = true;
        } else {
          WhiteR = false;
        }
        break;
      case 2:
        // You can also show Yellow here
        SubsampleCounter = 3;
        DebounceLong_al_cl.update(EpeeHit_l());

        break;
      case 3:
        // You can also show Yellow here
        SubsampleCounter = 4;
        DebounceLong_ar_cr.update(EpeeHit_r());
        break;
      case 4:
        SubsampleCounter = 0;
        // Debounce_Parry.update(Parry());
        break;
      }
    }
    break;

  case DEBOUNCING:

    if (!cl && !cr) {
      // no longer conditions to debounce-> go back to IDLE
      state = IDLE;
      break;
    }
    // Trick to satisfy Dos Santos. Not Sure if still needed
    if (Debounce_c1.isOK()) {
      Debounce_c2.setRequiredUs(SabreContactTime_us -
                                Sabre_DosSantosCorrection_us);
      Debounce_c2.update(cr);
    }
    if (Debounce_c2.isOK()) {
      Debounce_c1.setRequiredUs(SabreContactTime_us -
                                Sabre_DosSantosCorrection_us);
      Debounce_c1.update(cl);
    }

    if (Debounce_c1.isOK() && !SignalLeft) {
      // reduce required time for b2
      // check validity

      // Serial.println("Red");
      Red = true;
      Buzz = true;
      SignalLeft = true;
      StartLock(SABRE_LOCK_TIME);
    }
    if (Debounce_c2.isOK() && !SignalRight) {
      // reduce required time for b2
      // check validity

      // Serial.println("Green");
      Green = true;
      Buzz = true;
      SignalRight = true;
      StartLock(SABRE_LOCK_TIME);
    }

    break;
  }
}