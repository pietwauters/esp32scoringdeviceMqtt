// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#include "3WeaponSensor.h"
// Below are are all the settings needed per phase: IO direction
// (input/outpout), IO values (High or Low), analog channel, Threashold These
// are machine constants. It might be usefull to move this to flash and make it
// calibratable

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

// For the leak I test both al-cl and bl-cl. This allows me to re-use this test
// to check if I should switch to epee
inline bool LameLeak_l() {
  Set_IODirectionAndValue(IODirection_bl_cl & IODirection_al_cl,
                          IOValues_bl_cl | IOValues_al_cl);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cl_analog);
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

inline bool LameLeak_r() {
  Set_IODirectionAndValue(IODirection_br_cr & IODirection_ar_cr,
                          IOValues_br_cr | IOValues_ar_cr);
  int tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)cr_analog);
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

enum FoilState { IDLE, DEBOUNCING, DEBOUNCED, LOCKING, LOCKED };

void MultiWeaponSensor::DoFoil(void) {
  bool bl, br;
  bool leak;
  bool Valid_l, Valid_r;
  static FoilState state = IDLE;
  static int SubsampleCounter = 0;

  if (!SignalLeft) { // No need to check again if we already have a signal on
                     // this side
    Set_IODirectionAndValue(IODirection_al_bl, IOValues_al_bl);
    tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)bl_analog);
    bl = (tempADValue < AxMaxValue);
    NotConnectedLeft = bl;
    Valid_l = HitOnLame_l();
    DebounceLong_al_cr.update(Valid_l);
  }

  if (!SignalRight) { // No need to check again if we already have a signal on
                      // this side
    Set_IODirectionAndValue(IODirection_ar_br, IOValues_ar_br);
    tempADValue = fast_adc1_get_raw_inline((adc1_channel_t)br_analog);
    br = (tempADValue < AxMaxValue);
    NotConnectedRight = br;
    Valid_r = HitOnLame_r();
    DebounceLong_ar_cl.update(Valid_r);
  }

  Debounce_b1.update(bl);
  Debounce_b2.update(br);

  switch (state) {
  case IDLE:

    if (bl || br) {
      state = DEBOUNCING;
      break;
    } else {
      // Do one of the optional checks
      vTaskDelay(0);
      switch (SubsampleCounter) {
      case 0:
        SubsampleCounter = 1;
        leak = LameLeak_l();
        DebounceLong_al_cl.update(leak);
        if (Debounce_c1.update(leak)) {
          OrangeL = true;
        } else {
          OrangeL = false;
        }
        break;
      case 1:
        SubsampleCounter =
            0; //  <-------------------- Skipping next 2 checks!!!!!
        leak = LameLeak_r();
        DebounceLong_ar_cr.update(leak);
        if (Debounce_c2.update(leak)) {
          OrangeR = true;
        } else {
          OrangeR = false;
        }
        break;
      case 2:
        // this is not needed if I combine ax-bx and ax-cx for leak detection
        SubsampleCounter = 3;
        // DebounceLong_c1.update(EpeeHit_l());
        break;
      case 3:
        // this is not needed if I combine ax-bx and ax-cx for leak detection
        SubsampleCounter = 4;
        // DebounceLong_c2.update(EpeeHit_r());
        break;
      case 4:
        SubsampleCounter = 0;
        Debounce_Parry.update(Parry());
        break;
      }
    }
    break;

  case DEBOUNCING:

    if (!bl && !br) {
      // no longer conditions to debounce-> go back to IDLE
      state = IDLE;
      break;
    }
    // Trick to satisfy Dos Santos. Not Sure if still needed
    if (Debounce_b1.isOK()) {
      Debounce_b2.setRequiredUs(FoilContactTime_us -
                                Foil_DosSantosCorrection_us);
      Debounce_b2.update(br);
    }
    if (Debounce_b2.isOK()) {
      Debounce_b1.setRequiredUs(FoilContactTime_us -
                                Foil_DosSantosCorrection_us);
      Debounce_b1.update(bl);
    }

    if (Debounce_b1.isOK()) {
      // reduce required time for b2
      // check validity
      if (Valid_l) {
        // Serial.println("Red");
        Red = true;
        Buzz = true;
        SignalLeft = true;
        StartLock(FOIL_LOCK_TIME);
      } else {
        if (HitOnGuard_l()) {
          Debounce_b1.reset();
          // Serial.println("Guard");
        } else {
          if (HitOnPiste_l()) {
            Debounce_b1.reset();
            // Serial.println("Piste");
          } else {
            // Serial.println("WhiteL");
            WhiteL = true;
            Buzz = true;
            SignalLeft = true;
            StartLock(FOIL_LOCK_TIME);
          }
        }
      }
    }
    if (Debounce_b2.isOK()) {
      // reduce required time for b2
      // check validity
      if (Valid_r) {
        // Serial.println("Green");
        Green = true;
        Buzz = true;
        SignalRight = true;
        StartLock(FOIL_LOCK_TIME);
      } else {
        if (HitOnGuard_r()) {
          Debounce_b1.reset();
          // Serial.println("Guard");
        } else {
          if (HitOnPiste_r()) {
            Debounce_b1.reset();
            // Serial.println("Piste");
          } else {
            // Serial.println("WhiteR");
            WhiteR = true;
            Buzz = true;
            SignalRight = true;
            StartLock(FOIL_LOCK_TIME);
          }
        }
      }
    }

    break;
  }
}