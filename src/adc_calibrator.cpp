#include "adc_calibrator.h"

bool ResistorDividerCalibrator::begin(adc1_channel_t adc_channel_r3) {
  channel_r3 = adc_channel_r3;
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(channel_r3, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100,
                           &adc_chars);
  return load_from_nvs();
}

float ResistorDividerCalibrator::read_voltage(adc1_channel_t channel) {
  uint32_t raw = adc1_get_raw(channel);
  return esp_adc_cal_raw_to_voltage(raw, &adc_chars) / 1000.0f;
}

bool ResistorDividerCalibrator::calibrate_interactively(
    adc1_channel_t adc_channel_r3) {
  channel_r3 = adc_channel_r3;
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(channel_r3, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100,
                           &adc_chars);

  printf("\n[Calibration Step 1] Disconnect the unknown resistor (open input) "
         "and press ENTER...\n");
  while (getchar() != '\n')
    ;

  float v_open = read_voltage(channel_r3);
  float k = v_open / (v_gpio - v_open); // R3_eff / R1_eff

  printf("Measured V (open): %.3f V\n", v_open);
  printf("k = R3_eff / R1_eff = %.3f\n", k);

  printf("\n[Calibration Step 2] Connect known resistor (e.g., 100 Ohm), then "
         "press ENTER...\n");
  while (getchar() != '\n')
    ;

  float v_known = read_voltage(channel_r3);
  float R_known = 100.0f;

  float r1_est = R_known / ((v_gpio * k / v_known) - (1 + k));
  float r3_est = k * r1_est;

  printf("Measured V (known): %.3f V\n", v_known);
  printf("Estimated R1_eff: %.2f Ω\n", r1_est);
  printf("Estimated R3_eff: %.2f Ω\n", r3_est);

  r1_eff = r1_est;
  r3_eff = r3_est;

  save_to_nvs();
  return true;
}

float ResistorDividerCalibrator::get_adc_threshold_for_resistance(float R) {
  if (r1_eff <= 0 || r3_eff <= 0)
    return -1;
  float v_meas = v_gpio * r3_eff / (r1_eff + R + r3_eff);
  uint32_t adc_raw = (uint32_t)((v_meas / 3.3f) * 4095);
  return (float)adc_raw;
}

bool ResistorDividerCalibrator::load_from_nvs() {
  nvs_handle_t handle;
  if (nvs_open("calibration", NVS_READONLY, &handle) != ESP_OK)
    return false;
  uint32_t r1_u32, r3_u32;
  if (nvs_get_u32(handle, "r1", &r1_u32) != ESP_OK)
    return false;
  if (nvs_get_u32(handle, "r3", &r3_u32) != ESP_OK)
    return false;
  memcpy(&r1_eff, &r1_u32, sizeof(float));
  memcpy(&r3_eff, &r3_u32, sizeof(float));
  nvs_close(handle);
  return true;
}

void ResistorDividerCalibrator::save_to_nvs() {
  nvs_handle_t handle;
  if (nvs_open("calibration", NVS_READWRITE, &handle) != ESP_OK)
    return;
  uint32_t r1_u32, r3_u32;
  memcpy(&r1_u32, &r1_eff, sizeof(float));
  memcpy(&r3_u32, &r3_eff, sizeof(float));
  nvs_set_u32(handle, "r1", r1_u32);
  nvs_set_u32(handle, "r3", r3_u32);
  nvs_commit(handle);
  nvs_close(handle);
}
