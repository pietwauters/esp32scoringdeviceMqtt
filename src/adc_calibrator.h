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
  bool begin(adc1_channel_t adc_channel_top, adc1_channel_t adc_channel_bottom);
  bool calibrate_interactively(float known_resistor);
  int get_adc_threshold_for_resistance_NonTip(float R);
  int get_adc_threshold_for_resistance_Tip(float R);
  void set_gpio_voltage(float v) { v_gpio = v; }
  bool calibrate_r1_only(float R_known, float r3_eff_override = -1.0f);

  // NVS support
  bool load_calibration_from_nvs(const char *nvs_namespace = "calib");
  bool save_calibration_to_nvs(const char *nvs_namespace = "calib");

private:
  float v_gpio = 3.3f;
  float r1_eff = -1;
  float r1_Ax_eff = -1;
  float r3_eff = -1;
  esp_adc_cal_characteristics_t adc_chars;
  adc1_channel_t channel_top;
  adc1_channel_t channel_bottom;

  int voltage_to_adc_raw(float voltage);
  float read_voltage(adc1_channel_t channel);
  float read_voltage_average(adc1_channel_t channel, int samples);
  void wait_for_enter();
  char wait_for_key();

  float sdev_v_top_open = 0;
  float sdev_v_bottom_open = 0;
  float sdev_v_top = 0;
  float sdev_v_bottom = 0;
  float sdev_adc_threshold = 0;
};
