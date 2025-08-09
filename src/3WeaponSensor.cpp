// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#include "3WeaponSensor.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "soc/sens_reg.h"
#include "soc/sens_struct.h"

#include "FastADC1.h"
#include "TimeScoreDisplay.h"
#include "WS2812BLedStrip.h"
#include "adc_calibrator.h"
#include "driver/gpio.h" // Required for gpio_pad_select_gpio()
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "soc/io_mux_reg.h" // For IO_MUX register definitions
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <iostream>
// ResistorDividerCalibrator calibrator;
#include "FastGPIOSettings.h"
#include "ResistorSetting.h"
#include "adc_calibrator.h"

// #define adc1_get_raw FastADC1::read

static const char *CORE_SCORING_MACHINE_TAG = "Core Scoring machine";

ResistorDividerCalibrator MyCalibrator;
void initializeResistorThresholds() {
  // MyCalibrator.begin((adc1_channel_t)br_analog, (adc1_channel_t)cr_analog);
  bool success = true;
  if (!MyCalibrator.begin((adc1_channel_t)br_analog,
                          (adc1_channel_t)cr_analog)) {
    printf("\nCalibration will be done on the connector of the right fencer\n");
    printf("First test: between the outer pins\n");
    printf("    O          0     0     \n");
    printf("    |                |     \n");
    printf("    ______  100 Ω ____     \n");
    success &= MyCalibrator.calibrate_interactively(100.0);
    Set_IODirectionAndValue(IODirection_ar_cr, IOValues_ar_cr);
    printf("Second test: between the central and close pin\n");
    printf("    O          0     0     \n");
    printf("               |     |    \n");
    printf("                100 Ω      \n");
    if (success) {
      success &= MyCalibrator.calibrate_r1_only(100.0);
    }
    // Only save result is calibration was successful
    if (success) {
      MyCalibrator.save_calibration_to_nvs();
    }
  }

  // Values for Epee
  AxXy_100_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(100) - 25;
  AxXy_250_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(250) - 25;

  // Values for Sabre
  AxXy_280_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(280) - 25;
  BxCy_280_Ohm = MyCalibrator.get_adc_threshold_for_resistance_NonTip(280) - 25;
  // Values for Foil
  AxXy_125_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(125) -
                 25; // Hit on Guard
  AxXy_200_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(200) - 25;
  AxXy_300_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(300) -
                 25; // Normally closed circuit up to 300 Ohm
  AxXy_430_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(430) -
                 25; // Colored lights
  AxXy_450_Ohm = MyCalibrator.get_adc_threshold_for_resistance_Tip(450) -
                 25; // Hit on Piste
  BxCy_450_Ohm = MyCalibrator.get_adc_threshold_for_resistance_NonTip(450) -
                 25; // Yellow lights
}

constexpr int scanloop_us = 140;
// #define MEASURE_TIMING

#ifndef MEASURE_TIMING
// Timer callback (runs in timer task context, not ISR)
void scan_timer_callback(void *arg) {
  MultiWeaponSensor &MyLocalSensor = MultiWeaponSensor::getInstance();
  MyLocalSensor.DoFullScan();
  vTaskDelay(0);
}
#else
void scan_timer_callback(void *arg) {
  static int64_t last_time = 0;
  static int errorcounter = 0;
  int64_t now = esp_timer_get_time();
  if (last_time != 0) {
    int64_t dt = now - last_time;
    if (dt > scanloop_us + 50) {
      errorcounter++;
      if (errorcounter > 1) {
        Serial.printf("Interval: %lld us\n", dt);
        errorcounter = 0;
      }
    }
  }
  last_time = now;

  MultiWeaponSensor &MyLocalSensor = MultiWeaponSensor::getInstance();
  MyLocalSensor.DoFullScan();
  vTaskDelay(0);
}
#endif

MultiWeaponSensor::MultiWeaponSensor() {
  // ctor

  Const_FOIL_PARRY_ON_TIME = 5;
  Const_FOIL_PARRY_OFF_TIME = 43;

  // Init long debouncers
  DebounceLong_b1.setRequiredUs(2500000);
  DebounceLong_b2.setRequiredUs(2500000);
  DebounceLong_c1.setRequiredUs(2500000);
  DebounceLong_c2.setRequiredUs(2500000);
  DebounceLong_al_cr.setRequiredUs(2500000);
  DebounceLong_ar_cl.setRequiredUs(2500000);
  DebounceLong_al_cl.setRequiredUs(2500000);
  DebounceLong_ar_cr.setRequiredUs(2500000);

  Debounce_NotConnected.setRequiredUs(120000000);
  Debounce_AtLeastOneNotConnected.setRequiredUs(10000);
  gpio_pad_select_gpio(GPIO_NUM_33); // Route pin to GPIO (not peripheral)
  gpio_set_direction(GPIO_NUM_33, GPIO_MODE_INPUT_OUTPUT);
}

void MultiWeaponSensor::begin() {
  Preferences mypreferences;
  mypreferences.begin("scoringdevice", false);
  LightsDuration = mypreferences.getInt("LIGHTS_MS", 0);
  if (!LightsDuration) {
    mypreferences.putInt("LIGHTS_MS", LIGHTS_DURATION_MS);
    LightsDuration = LIGHTS_DURATION_MS;
  }
  uint8_t storedweapon = mypreferences.getUChar("START_WEAPON", 99);
  if (99 == storedweapon) {
    mypreferences.putUChar("START_WEAPON", 0);
    storedweapon = 0;
  }
  switch (storedweapon) {
  case 0:
    m_ActualWeapon = FOIL;
    break;

  case 1:
    m_ActualWeapon = EPEE;
    break;

  case 2:
    m_ActualWeapon = SABRE;
    break;
  default:
    m_ActualWeapon = EPEE;
  }
  mypreferences.end();

  adc1_fast_register_channel(ADC1_CHANNEL_0);
  adc1_fast_register_channel(ADC1_CHANNEL_3);
  adc1_fast_register_channel(ADC1_CHANNEL_4);
  adc1_fast_register_channel(ADC1_CHANNEL_6);
  adc1_fast_register_channel(ADC1_CHANNEL_7);

  adc1_fast_begin_unsafe();
  gpio_reset_pin(GPIO_NUM_33); // Reset function to digital
  gpio_set_direction(GPIO_NUM_33, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_33, 0); // or 1, as needed
  gpio_reset_pin(GPIO_NUM_2);     // Reset function to digital
  gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);

  Set_IODirectionAndValue(IODirection_br_cr, IOValues_br_cr);

  initializeResistorThresholds();

  // int fast_raw = fast_adc1_get_raw(ADC1_CHANNEL_3);
  /*int64_t t0 = esp_timer_get_time();
  volatile int sum1 = 0;
  int samples = 10000; // Number of samples to take
  FullScanCounter = 1;
  for (int i = 0; i < samples; ++i) {
    if (FullScanCounter)
      FullScanCounter--;
    else
      FullScanCounter = SABRE_SCANCOUNTER_INIT;
    DoFoil();
  }
  // fast_adc1_get_raw_inline(ADC1_CHANNEL_6);
  int64_t t1 = esp_timer_get_time();
  printf("Total time: %lld us\n", t1 - t0);
  printf("Total samples: %d\n", samples);
  printf("Average time per sample: %lld us\n", (t1 - t0) / samples);*/

  DoReset();

  // Timer config
  static esp_timer_handle_t scan_timer;
  const esp_timer_create_args_t scan_timer_args = {
      .callback = &scan_timer_callback,
      .arg = nullptr,
      .dispatch_method =
          ESP_TIMER_TASK, // Use ESP_TIMER_TASK for longer callbacks
      .name = "scan_timer"};
  esp_timer_create(&scan_timer_args, &scan_timer);
  // calibrator.begin(ADC1_CHANNEL_6);
  // calibrator.calibrate_interactively(ADC1_CHANNEL_6);
  //  Start timer: period in microseconds (e.g., 250 us = 0.25 ms)
  esp_timer_start_periodic(scan_timer, scanloop_us); // 250 us interval
}

adc1_channel_t ADC1_CHANNELS[] = {
    ADC1_CHANNEL_0, ADC1_CHANNEL_1, ADC1_CHANNEL_2, ADC1_CHANNEL_3,
    ADC1_CHANNEL_4, ADC1_CHANNEL_5, ADC1_CHANNEL_6, ADC1_CHANNEL_7};

MultiWeaponSensor::~MultiWeaponSensor() {
  // dtor
}

void MultiWeaponSensor::HandleLights() {
  uint8_t temp = 0;
  if (millis() >
      ShortIndicatorsDebouncer) // this is needed to avoid too many events for
                                // broken wires or bad mass contacts
  {
    ShortIndicatorsDebouncer = millis() + 233;
    if (OrangeL)
      temp |= 0x20;
    if (OrangeR)
      temp |= 0x10;
    if (m_ActualWeapon == SABRE) {
      if (WhiteL)
        temp |= 0x40;
      if (WhiteR)
        temp |= 0x08;
    }
  } else {
    temp = Lights & 0x30;
    if (m_ActualWeapon == SABRE) {
      temp = Lights & 0x78;
    }
  }
  if (Red)
    temp |= 0x80;
  if (m_ActualWeapon == FOIL) {
    if (WhiteL)
      temp |= 0x40;
    if (WhiteR)
      temp |= 0x08;
  }
  if (Green)
    temp |= 0x04;

  /*if(Buzz && !bPreventBuzzer)
    temp |= 0x02;*/
  if (Lights != temp) {
    Lights = temp;
    SensorStateChanged(EVENT_LIGHTS | temp);
  }
}

bool MultiWeaponSensor::OKtoReset() {
  if (!WaitingForResetStarted) {
    WaitingForResetStarted = true;
    // InitLock();
    //  every weapon has a diferent lock time. The buzzer has to stop after 2
    //  sec
    switch (m_ActualWeapon) {
    case FOIL:
      TimeToReset = millis() + LightsDuration - FOIL_LOCK_TIME;
      break;

    case EPEE:
      TimeToReset = millis() + LightsDuration - EPEE_LOCK_TIME;
      break;

    case SABRE:
      TimeToReset = millis() + LightsDuration - SABRE_LOCK_TIME;
      break;
    }
    return false;
  }
  if (millis() > TimeToReset) {
    if (Buzz == false) {
      return (true);
    }

    // BUZZPIN = false;
    Buzz = false;
    /* here comes the extra time if you want to wait longer than 2 sec */
    TimeToReset = millis() + 500;
  }
  return (false);
}

void MultiWeaponSensor::DoReset() {
  WhiteL = false;
  OrangeL = false;
  OrangeR = false;
  WhiteR = false;
  Green = false;
  Red = false;
  Buzz = false;
  HandleLights();
  MaybeSignalRight = false;
  MaybeSignalLeft = false;
  BlockWhipover = false;
  WeaponContact = false;
  WaitingForResetStarted = false;
  // chronostatus = CHRONO_RUNNING;

  PossiblyRed = false;
  PossiblyGreen = false;
  WeHaveBlockedAhit = false;
  BlockedAHitCounter = 650;

  switch (m_ActualWeapon) {
  case FOIL:

    Debounce_b1.reset(FoilContactTime_us); // 14ms for foil
    Debounce_b2.reset(FoilContactTime_us);
    Debounce_c1.reset(Foil_LameLeak_us);
    Debounce_c2.reset(Foil_LameLeak_us);

    break;

  case EPEE:

    Debounce_c1.reset(EpeeContactTime_us); // 6 ms for epee
    Debounce_c2.reset(EpeeContactTime_us); // 6 ms for epee
    Debounce_c1.setDosSantosMarginUs(scanloop_us + 10);
    Debounce_c2.setDosSantosMarginUs(scanloop_us + 10);

    break;

  case SABRE:
    Debounce_b1.reset(1400);
    Debounce_b2.reset(1400);
    Debounce_c1.reset(110);
    Debounce_c2.reset(110);

    break;
  }

  SignalLeft = 0;
  SignalRight = 0;
  LockStarted = false;

  bParrySignal = false;
  Debounce_Parry.setRequiredUs(1500);

  return;
}

void MultiWeaponSensor::StartLock(int TimeToLock) {
  if (!LockStarted) {
    LockStarted = true;
    TimeOfLock = millis() + TimeToLock;
  }
}

bool MultiWeaponSensor::IsLocked() {
  if (LockStarted) {
    if (millis() > TimeOfLock)
      return true;
  }
  return false;
}

void MultiWeaponSensor::DoFullScan() {

  weapon_t temp = GetWeapon();

  if (m_ActualWeapon != temp) {
    m_ActualWeapon = temp;
    bPreventBuzzer = false;
    SensorStateChanged(EVENT_WEAPON | temp);
    DoReset();
    vTaskDelay(0);
  }

  if (IsLocked()) {
    SignalLeft = 1;  // Now no changes will be registered for left
    SignalRight = 1; // Same for right

    if (OKtoReset()) {

      DoReset();
    }
    vTaskDelay(0);
  }
  HandleLights();

  switch (m_ActualWeapon) {
  case FOIL:
    DoFoil();
    break;

  case EPEE:
    DoEpee();
    break;

  case SABRE:
    DoSabre();
    break;
  }
}
void MultiWeaponSensor::resetLongDebouncers() {
  DebounceLong_al_cl.reset();
  DebounceLong_al_cr.reset();
  DebounceLong_ar_cl.reset();
  DebounceLong_ar_cr.reset();
}

weapon_t MultiWeaponSensor::GetWeapon() {

  if (m_ActualWeapon !=
      EPEE) { // In foil or sabre: check if both sides are disconnected
    if (NotConnectedRight || NotConnectedLeft) {
      bPreventBuzzer = Debounce_AtLeastOneNotConnected.update(true);

      if (NotConnectedRight && NotConnectedLeft) {
        Debounce_NotConnected.update(true);

      } else {
        Debounce_NotConnected.reset();
        // bPreventBuzzer = false;
      }
      if (Debounce_NotConnected.isOK()) // We've reached zero, so we switch back
                                        // to default Epee
      {

        bPreventBuzzer = false;
        Debounce_AtLeastOneNotConnected.reset();
        if (m_DectionMode != MANUAL) {
          m_DetectedWeapon = EPEE;

          return EPEE;
        }
      }
    } else {
      NotConnectedRight = false;
      NotConnectedLeft = false;
      bPreventBuzzer = false;
    }
  }
  if (m_DectionMode == MANUAL) {
    bAutoDetect = 0;
    return m_ActualWeapon;
  }

  m_DetectedWeapon = m_ActualWeapon;
  bAutoDetect = 1;

  switch (m_ActualWeapon) {
  case FOIL:
    // if (ax-cx) & !(ax-bx) -> switch to epee

    if ((DebounceLong_al_cl.isOK()) &&
        (DebounceLong_ar_cr.isOK())) { // certainly not foil anymore)
      if ((Debounce_b1.isOK()) && (Debounce_b2.isOK())) {
        m_DetectedWeapon = EPEE;
        bPreventBuzzer = false;
        resetLongDebouncers();
      }
    }
    // if (bx-cy) && (ax-bx) -> switch to sabre
    else {
      if ((DebounceLong_al_cr.isOK()) && (DebounceLong_ar_cl.isOK())) {
        if ((!Debounce_b1.isOK()) && (!Debounce_b2.isOK())) {
          m_DetectedWeapon = SABRE;
          resetLongDebouncers();
          bPreventBuzzer = false;
        }
      }
    }
    // in all other cases: -> keep foil
    return m_DetectedWeapon;

    break;

  case EPEE:
    // if (ax-cy) & !(ax-bx) -> switch to foil
    // if (ax-cy) & (ax-by) -> switch to sabre
    // keep epee
    if ((DebounceLong_al_cr.isOK()) &&
        (DebounceLong_ar_cl.isOK())) { // certainly not epee anymore)
      if ((OrangeR) && (OrangeL)) {
        m_DetectedWeapon = SABRE;
        bPreventBuzzer = false;
        Debounce_NotConnected.reset();
        resetLongDebouncers();

      } else {
        m_DetectedWeapon = FOIL;
        bPreventBuzzer = false;
        Debounce_NotConnected.reset();
        resetLongDebouncers();
      }

      return m_DetectedWeapon;
    } else
      return EPEE;
    break;

  case SABRE:
    // if (ax-cy) & !(ax-bx) -> switch to foil
    // if (ax-cx) & !(ax-bx) -> switch to epee
    // keep sabre
    {

      if ((DebounceLong_ar_cl.isOK()) && (DebounceLong_al_cr.isOK())) {
        if (WhiteR && WhiteL) {
          m_DetectedWeapon = FOIL;
          bPreventBuzzer = false;
          resetLongDebouncers();
        }
      }
      if ((DebounceLong_ar_cr.isOK()) && (DebounceLong_al_cl.isOK())) {
        if ((Debounce_b1.isOK()) && (Debounce_b2.isOK())) {
          m_DetectedWeapon = EPEE;
          bPreventBuzzer = false;
          resetLongDebouncers();
        }
      }
      return m_DetectedWeapon;
    }
    break;
  }
  return EPEE;
}
