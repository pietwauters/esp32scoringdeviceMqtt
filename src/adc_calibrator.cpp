#include "adc_calibrator.h"
#include "FastADC1.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <driver/uart.h>
#include <math.h>
#include <string.h>

int ResistorDividerCalibrator::voltage_to_adc_raw(float voltage) {
  int low = 0, high = 4095, result = 0;
  while (low <= high) {
    int mid = (low + high) / 2;
    float v = esp_adc_cal_raw_to_voltage(mid, &adc_chars) / 1000.0f;
    if (v < voltage) {
      low = mid + 1;
      result = mid; // best so far
    } else {
      high = mid - 1;
    }
  }
  return result;
}
// Helper: Get average voltage (for all calculations except threshold sdev)
float ResistorDividerCalibrator::read_voltage_average(adc1_channel_t channel,
                                                      int samples) {
  uint32_t total = 0;
  for (int i = 0; i < samples; ++i) {
    total += fast_adc1_get_raw_inline(channel);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  uint32_t avg_raw = total / samples;
  return esp_adc_cal_raw_to_voltage(avg_raw, &adc_chars) / 1000.0f;
}

// Helper: Get mean and sdev in ADC units (for threshold only)
void calc_mean_stddev_adc(adc1_channel_t channel, int samples, float &mean_adc,
                          float &sdev_adc) {
  float sum = 0, sum_sq = 0;
  for (int i = 0; i < samples; ++i) {
    int raw = fast_adc1_get_raw_inline(channel);
    sum += raw;
    sum_sq += raw * raw;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  mean_adc = sum / samples;
  float var = (sum_sq / samples) - (mean_adc * mean_adc);
  sdev_adc = sqrtf(var > 0 ? var : 0);
}

float ResistorDividerCalibrator::read_voltage(adc1_channel_t channel) {
  uint32_t raw = fast_adc1_get_raw_inline(channel);
  return esp_adc_cal_raw_to_voltage(raw, &adc_chars) / 1000.0f;
}

bool ResistorDividerCalibrator::calibrate_interactively(float R_known) {
  printf("\n[Step 1] Disconnect the unknown resistor (open input) and press "
         "ENTER...\n");
  wait_for_enter();

  float v_top_open = read_voltage_average(channel_top, 100);
  float v_bottom_open = read_voltage_average(channel_bottom, 100);

  printf("Open Circuit Measurements:\n");
  printf("  V_TOP = %.3f V\n", v_top_open);
  printf("  V_BOTTOM = %.3f V\n", v_bottom_open);

  float vcc_est = v_top_open; // assume V_TOP is close to Vcc
  v_gpio = vcc_est;

  printf("\n[Step 2] Connect known resistor (%.1f Ohm), then press ENTER...\n",
         R_known);
  wait_for_enter();

  float v_top = read_voltage_average(channel_top, 1000);
  float v_bottom = read_voltage_average(channel_bottom, 1000);

  printf("Known Resistor Measurements:\n");

  float denom = v_top - v_bottom;
  if (denom <= 0.0f) {
    printf("Invalid voltage readings. Calibration failed.\n");
    return false;
  }

  float r_total = R_known * vcc_est / denom;
  r1_eff = r_total - R_known - (R_known * (v_bottom / denom));
  r3_eff = r_total - R_known - r1_eff;

  printf("  Estimated R1_eff = %.2f Ω\n", r1_eff);
  printf("  Estimated R3_eff = %.2f Ω\n", r3_eff);

  // --- ADC threshold and sdev ---
  float mean_adc, sdev_adc;

  int threshold = get_adc_threshold_for_resistance_NonTip(R_known);

  printf("Modelled ADC threshold for %.1f Ohm: %d\n", R_known, threshold);
  calc_mean_stddev_adc(channel_bottom, 1000, mean_adc, sdev_adc);
  printf("Measured ADC threshold for %.1f Ohm: %.1f\n", R_known, mean_adc);
  printf("Measured Stddev at threshold: %.2f ADC units\n", sdev_adc);

  // Verification loop
  printf("\n[Step 3] Connect another known resistor to verify calibration.\n");
  while (true) {
    char key = wait_for_key();
    if (key == 'q')
      break;

    float v_top_test = read_voltage(channel_top);
    float v_bottom_test = read_voltage_average(channel_bottom, 8);

    float v_diff = v_top_test - v_bottom_test;
    if (v_top_test <= v_bottom_test || v_bottom_test <= 0.0f) {
      printf("Invalid reading. Skipping...\n");
      continue;
    }

    // Single-ADC estimate (runtime)
    float R_est_single = ((v_gpio / v_bottom_test) - 1.0f) * r3_eff - r1_eff;

    int threshold = get_adc_threshold_for_resistance_NonTip(R_est_single);

    printf("Modelled ADC threshold for %.1f Ohm: %d\n", R_est_single,
           threshold);
    calc_mean_stddev_adc(channel_bottom, 1000, mean_adc, sdev_adc);
    printf("Measured ADC threshold for %.1f Ohm: %.1f\n", R_est_single,
           mean_adc);
    printf("Measured Stddev at threshold: %.2f ADC units\n", sdev_adc);
  }

  return true;
}

int ResistorDividerCalibrator::get_adc_threshold_for_resistance_NonTip(
    float R) {
  if (r1_eff <= 0 || r3_eff <= 0)
    return -1;
  float v_meas = v_gpio * r3_eff / (r1_eff + R + r3_eff);
  return voltage_to_adc_raw(v_meas);
}

int ResistorDividerCalibrator::get_adc_threshold_for_resistance_Tip(float R) {
  if (r1_Ax_eff <= 0 || r3_eff <= 0)
    return -1;
  float v_meas = v_gpio * r3_eff / (r1_Ax_eff + R + r3_eff);
  return voltage_to_adc_raw(v_meas);
}

bool ResistorDividerCalibrator::calibrate_r1_only(float R_known,
                                                  float r3_eff_override) {
  // Determine which R3 to use
  float r3_to_use = (r3_eff_override > 0.0f) ? r3_eff_override : r3_eff;

  if (r3_to_use <= 0.0f) {
    printf("Error: No valid R3_eff available. Run full calibration first or "
           "provide R3.\n");
    return false;
  }

  printf("Estimated Vcc = %.3f V from open-bottom\n", v_gpio);

  printf("\n[Step 2 - R1-only] Connect known resistor (%.1f Ohm), then press "
         "ENTER...\n",
         R_known);
  wait_for_enter();

  float v_bottom = read_voltage_average(channel_bottom, 1000);
  if (v_bottom <= 0.0f || v_bottom >= v_gpio) {
    printf("Invalid voltage reading. Calibration failed.\n");
    return false;
  }

  float r1_calc = ((v_gpio / v_bottom) - 1.0f) * r3_to_use - R_known;

  r1_Ax_eff = r1_calc;
  r3_eff = r3_to_use; // Store the one used

  printf("Measured V_BOTTOM = %.3f V\n", v_bottom);
  printf("Estimated R1_eff = %.2f Ω    Using R3 = = %.2f Ω\n", r1_Ax_eff,
         r3_eff);

  // Verification loop
  printf("\n[Step 3] Connect another known resistor to verify calibration.\n");
  float mean_adc, sdev_adc;

  while (true) {
    char key = wait_for_key();
    if (key == 'q')
      break;

    float v_top_test = read_voltage(channel_top);
    float v_bottom_test = read_voltage_average(channel_bottom, 8);

    float v_diff = v_top_test - v_bottom_test;
    if (v_top_test <= v_bottom_test || v_bottom_test <= 0.0f) {
      printf("Invalid reading. Skipping...\n");
      continue;
    }

    // Single-ADC estimate (runtime)
    float R_est_single = ((v_gpio / v_bottom_test) - 1.0f) * r3_eff - r1_Ax_eff;

    int threshold = get_adc_threshold_for_resistance_Tip(R_est_single);

    printf("Modelled ADC threshold for %.1f Ohm: %d\n", R_est_single,
           threshold);
    calc_mean_stddev_adc(channel_bottom, 1000, mean_adc, sdev_adc);
    printf("Measured ADC threshold for %.1f Ohm: %.1f\n", R_est_single,
           mean_adc);
    printf("Measured Stddev at threshold: %.2f ADC units\n", sdev_adc);
  }

  return true;
}

bool ResistorDividerCalibrator::begin(adc1_channel_t adc_channel_top,
                                      adc1_channel_t adc_channel_bottom) {
  channel_top = adc_channel_top;
  channel_bottom = adc_channel_bottom;
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(channel_top, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(channel_bottom, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100,
                           &adc_chars);
  return load_calibration_from_nvs();
}

bool ResistorDividerCalibrator::load_calibration_from_nvs(
    const char *nvs_namespace) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READONLY, &handle);
  if (err != ESP_OK)
    return false;

  size_t required_size = sizeof(float);
  err |= nvs_get_blob(handle, "v_gpio", &v_gpio, &required_size);
  err |= nvs_get_blob(handle, "r1_eff", &r1_eff, &required_size);
  err |= nvs_get_blob(handle, "r3_eff", &r3_eff, &required_size);
  err |= nvs_get_blob(handle, "r1_Ax_eff", &r1_Ax_eff, &required_size);

  nvs_close(handle);
  return err == ESP_OK;
}

bool ResistorDividerCalibrator::save_calibration_to_nvs(
    const char *nvs_namespace) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return false;

  err |= nvs_set_blob(handle, "v_gpio", &v_gpio, sizeof(float));
  err |= nvs_set_blob(handle, "r1_eff", &r1_eff, sizeof(float));
  err |= nvs_set_blob(handle, "r3_eff", &r3_eff, sizeof(float));
  err |= nvs_set_blob(handle, "r1_Ax_eff", &r1_Ax_eff, sizeof(float));
  err |= nvs_commit(handle);

  nvs_close(handle);
  return err == ESP_OK;
}

void ResistorDividerCalibrator::wait_for_enter() {
  printf("Press ENTER to continue...\n");
  uart_flush_input(UART_NUM_0); // Flush RX buffer
  uint8_t c;
  while (true) {
    int len = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(10));
    if (len > 0 && (c == '\n' || c == '\r'))
      break;
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_task_wdt_reset();
  }
}

char ResistorDividerCalibrator::wait_for_key() {
  printf("Press a key (q to quit, ENTER to continue):\n");
  uart_flush_input(UART_NUM_0); // Flush RX buffer
  uint8_t c;
  while (true) {
    int len = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(10));
    if (len > 0) {
      if (c == '\n' || c == '\r')
        return '\n';
      return (char)c;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    esp_task_wdt_reset();
  }
}