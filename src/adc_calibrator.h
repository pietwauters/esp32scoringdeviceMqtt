// adc_calibrator.h
#pragma once
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <algorithm> // Add this for std::sort
#include <cstring>
#include <stdio.h>
#include <vector> // Add this
constexpr int CALIBRATION_VERSION = 6;

// Add the ADCStatistics structure definition
struct ADCStatistics {
  float mean;
  float median;
  float stddev;
  float skewness;
  float kurtosis;
  float min_val;
  float max_val;
  int outlier_count;
  bool is_normal_distribution;
};

class ResistorDividerCalibrator {
public:
  bool begin(adc1_channel_t adc_channel_top, adc1_channel_t adc_channel_bottom);
  bool calibrate_interactively(float known_resistor);
  int get_adc_threshold_for_resistance_NonTip(float R);
  int get_adc_threshold_for_resistance_Tip(float R);
  void set_gpio_voltage(float v) { v_gpio = v; }
  bool calibrate_r1_only(float R_known, float r3_eff_override = -1.0f);
  float get_reference_resistor_from_user(
      const char *prompt = "Enter reference resistor value (100-500 Ohms): ");

  // NVS support
  bool load_calibration_from_nvs(const char *nvs_namespace = "calib");
  bool save_calibration_to_nvs(int version = CALIBRATION_VERSION,
                               const char *nvs_namespace = "calib");
  void
  calc_enhanced_adc_stats(adc1_channel_t channel, int samples,
                          ADCStatistics &stats); // Now ADCStatistics is defined
  float
  read_voltage_robust(adc1_channel_t channel, int samples,
                      bool use_median); // Remove default argument from header

  float read_voltage_trimmed_average(adc1_channel_t channel, int samples,
                                     float trim_percent = 10.0f);

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
  int CalVersion = 0;
};
