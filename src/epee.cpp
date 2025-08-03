// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
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
inline bool HitOnGuard_l() {
  Set_IODirectionAndValue(IODirection_al_br, IOValues_al_br);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)br_analog);
  return (tempADValue > AxMaxValue);
};
inline bool HitOnPiste_l() {
  Set_IODirectionAndValue(IODirection_al_piste, IOValues_al_piste);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)piste_analog);
  return (tempADValue > AxMaxValue);
};

inline bool WeaponLeak_l() {
  Set_IODirectionAndValue(IODirection_al_bl, IOValues_al_bl);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)bl_analog);
  return (tempADValue > BCMaxValue);
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
inline bool HitOnGuard_r() {
  Set_IODirectionAndValue(IODirection_ar_bl, IOValues_ar_bl);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)bl_analog);
  return (tempADValue > AxMaxValue);
};
inline bool HitOnPiste_r() {
  Set_IODirectionAndValue(IODirection_ar_piste, IOValues_ar_piste);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)piste_analog);
  return (tempADValue > AxMaxValue);
};

inline bool WeaponLeak_r() {
  Set_IODirectionAndValue(IODirection_ar_br, IOValues_ar_br);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)br_analog);
  return (tempADValue > BCMaxValue);
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

enum EpeeState { IDLE, DEBOUNCING, DEBOUNCED, LOCKING, LOCKED };

void MultiWeaponSensor::DoEpee(void) {
  bool cl, cr;
  static EpeeState state = IDLE;
  static int SubsampleCounter = 0;

  if (!SignalLeft) { // No need to check again if we already have a signal on
                     // this side
    Set_IODirectionAndValue(IODirection_al_cl, IOValues_al_cl);
    tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cl_analog);
    cl = (tempADValue > AxMaxValue);
  }

  if (!SignalRight) { // No need to check again if we already have a signal on
                      // this side
    Set_IODirectionAndValue(IODirection_ar_cr, IOValues_ar_cr);
    tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cr_analog);
    cr = (tempADValue > AxMaxValue);
  }

  Debounce_c1.update(cl);
  Debounce_c2.update(cr);

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
        if (Debounce_b1.update(WeaponLeak_l())) {
          OrangeL = true;
        } else {
          OrangeL = false;
        }
        break;
      case 1:
        SubsampleCounter = 2;
        if (Debounce_b2.update(WeaponLeak_r())) {
          OrangeR = true;
        } else {
          OrangeR = false;
        }
        break;
      case 2:
        SubsampleCounter = 3;
        DebounceLong_al_cr.update(HitOnLame_l());
        break;
      case 3:
        SubsampleCounter = 0;
        DebounceLong_ar_cl.update(HitOnLame_r());
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
      Debounce_c2.setRequiredUs(EpeeContactTime_us -
                                Epee_DosSantosCorrection_us);
      Debounce_c2.update(cr);
    }
    if (Debounce_c2.isOK()) {
      Debounce_c1.setRequiredUs(EpeeContactTime_us -
                                Epee_DosSantosCorrection_us);
      Debounce_c1.update(cl);
    }

    if (Debounce_c1.isOK()) {

      // check validity
      if (HitOnGuard_l()) {
        Debounce_c1.reset();
        // Serial.println("Guard");
      } else {
        if (HitOnPiste_l()) {
          Debounce_c1.reset();
          // Serial.println("Piste");
        } else {
          // Serial.println("WhiteL");
          Red = true;
          Buzz = true;
          SignalLeft = true;
          StartLock(EPEE_LOCK_TIME);
        }
      }
    }
    if (Debounce_c2.isOK()) {

      // check validity
      if (HitOnGuard_r()) {
        Debounce_c2.reset();
        // Serial.println("Guard");
      } else {
        if (HitOnPiste_r()) {
          Debounce_c2.reset();
          // Serial.println("Piste");
        } else {
          // Serial.println("WhiteL");
          Green = true;
          Buzz = true;
          SignalRight = true;
          StartLock(EPEE_LOCK_TIME);
        }
      }
    }

    break;
  }
}