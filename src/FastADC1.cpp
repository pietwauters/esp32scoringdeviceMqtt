#include "FastADC1.h"
#include "driver/adc.h"
#include "esp32/rom/ets_sys.h"
#include "esp_adc_cal.h"
#include "soc/sens_reg.h"
#include "soc/sens_struct.h"
#include <stdint.h>
#include <string.h>

// Static storage for per-channel attenuation and per-attenuation calibration
adc_atten_t FastADC1::channel_atten[8] = {
    ADC_ATTEN_DB_0, ADC_ATTEN_DB_0, ADC_ATTEN_DB_0, ADC_ATTEN_DB_0,
    ADC_ATTEN_DB_0, ADC_ATTEN_DB_0, ADC_ATTEN_DB_0, ADC_ATTEN_DB_0};
esp_adc_cal_characteristics_t FastADC1::cal_chars[4];
bool FastADC1::cal_inited[4] = {false, false, false, false};

static int atten_to_idx(adc_atten_t atten) {
  switch (atten) {
  case ADC_ATTEN_DB_0:
    return 0;
  case ADC_ATTEN_DB_2_5:
    return 1;
  case ADC_ATTEN_DB_6:
    return 2;
  case ADC_ATTEN_DB_11:
    return 3;
  default:
    return 0;
  }
}

void FastADC1::configure_channel(adc1_channel_t channel, adc_atten_t atten) {
  adc1_config_channel_atten(channel, atten);
  channel_atten[channel] = atten;
}

uint16_t FastADC1::read(adc1_channel_t channel) {
  SENS.sar_meas_start1.sar1_en_pad = (1 << channel);
  SENS.sar_meas_start1.meas1_start_sar = 1;
  while (SENS.sar_meas_start1.meas1_done_sar == 0) {
  }
  uint16_t result = SENS.sar_meas_start1.meas1_data_sar & 0xFFF;
  SENS.sar_meas_start1.meas1_done_sar = 0;
  return result;
}

void FastADC1::ensure_calibration(adc_atten_t atten) {
  int idx = atten_to_idx(atten);
  if (!cal_inited[idx]) {
    memset(&cal_chars[idx], 0, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t cal_type = esp_adc_cal_characterize(
        ADC_UNIT_1, atten, ADC_WIDTH_BIT_12, 0, &cal_chars[idx]);
    cal_inited[idx] = true;
    (void)cal_type;
  }
}

esp_adc_cal_characteristics_t *FastADC1::get_cal(adc_atten_t atten) {
  int idx = atten_to_idx(atten);
  return &cal_chars[idx];
}

uint32_t FastADC1::read_calibrated_mv(adc1_channel_t channel) {
  adc_atten_t atten = channel_atten[channel];
  ensure_calibration(atten);
  uint16_t raw = read(channel);
  return esp_adc_cal_raw_to_voltage(raw, get_cal(atten));
}

void FastADC1::configure_all_channels(adc_atten_t atten) {
  for (int ch = 0; ch < 8; ++ch) {
    configure_channel(static_cast<adc1_channel_t>(ch), atten);
  }
}