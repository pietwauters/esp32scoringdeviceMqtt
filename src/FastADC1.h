#pragma once
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <stdint.h>

class FastADC1 {
public:
  // Call once at startup for each channel you will use
  static void configure_channel(adc1_channel_t channel, adc_atten_t atten);
  // Set the same attenuation for all channels (convenience)
  static void configure_all_channels(adc_atten_t atten);
  // Fast, blocking read (returns 12-bit value)
  static uint16_t read(adc1_channel_t channel);
  // Calibrated read (returns mV), uses correct attenuation per channel
  static uint32_t read_calibrated_mv(adc1_channel_t channel);

private:
  static void ensure_calibration(adc_atten_t atten);
  static esp_adc_cal_characteristics_t *get_cal(adc_atten_t atten);
  static adc_atten_t channel_atten[8];
  static esp_adc_cal_characteristics_t
      cal_chars[4]; // 0:0dB, 1:2.5dB, 2:6dB, 3:11dB
  static bool cal_inited[4];
};
