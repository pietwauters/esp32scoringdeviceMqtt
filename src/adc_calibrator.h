// adc_calibrator.h
#pragma once
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstring>
#include <stdio.h>

class ResistorDividerCalibrator {
public:
  bool begin(adc1_channel_t adc_channel_r3);
  bool calibrate_interactively(adc1_channel_t adc_channel_r3);
  float get_adc_threshold_for_resistance(float R);

  void set_gpio_voltage(float v) { v_gpio = v; } // Optional override

private:
  float v_gpio = 3.3f;
  float r1_eff = -1;
  float r3_eff = -1;
  esp_adc_cal_characteristics_t adc_chars;
  adc1_channel_t channel_r3;

  bool load_from_nvs();
  void save_to_nvs();
  float read_voltage(adc1_channel_t channel);
};