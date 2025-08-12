#include "adc_calibrator.h"
#include "FastADC1.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <algorithm> // Add this
#include <cmath>     // Add this for sqrtf
#include <driver/uart.h>
#include <math.h>
#include <string.h>
#include <vector> // Add this

// Enhanced statistics calculation function
void ResistorDividerCalibrator::calc_enhanced_adc_stats(adc1_channel_t channel,
                                                        int samples,
                                                        ADCStatistics &stats) {
  // Collect all samples
  std::vector<int> raw_values(samples);
  float sum = 0, sum_sq = 0;

  for (int i = 0; i < samples; ++i) {
    raw_values[i] = fast_adc1_get_raw_inline(channel);
    sum += raw_values[i];
    sum_sq += raw_values[i] * raw_values[i];
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  // Sort for median and percentile calculations
  std::sort(raw_values.begin(), raw_values.end());

  // Basic statistics
  stats.mean = sum / samples;
  stats.median =
      (samples % 2 == 0)
          ? (raw_values[samples / 2 - 1] + raw_values[samples / 2]) / 2.0f
          : raw_values[samples / 2];

  float variance = (sum_sq / samples) - (stats.mean * stats.mean);
  stats.stddev = sqrtf(variance > 0 ? variance : 0);

  stats.min_val = raw_values[0];
  stats.max_val = raw_values[samples - 1];

  // Skewness and Kurtosis
  float sum_cubed = 0, sum_fourth = 0;
  stats.outlier_count = 0;
  float outlier_threshold = 2.5f * stats.stddev; // 2.5 sigma rule

  for (int i = 0; i < samples; ++i) {
    float deviation = raw_values[i] - stats.mean;
    if (stats.stddev > 0) { // Avoid division by zero
      float normalized_dev = deviation / stats.stddev;

      sum_cubed += normalized_dev * normalized_dev * normalized_dev;
      sum_fourth +=
          normalized_dev * normalized_dev * normalized_dev * normalized_dev;
    }

    if (fabs(deviation) > outlier_threshold) {
      stats.outlier_count++;
    }
  }

  stats.skewness = (stats.stddev > 0) ? sum_cubed / samples : 0;
  stats.kurtosis =
      (stats.stddev > 0) ? (sum_fourth / samples) - 3.0f : 0; // Excess kurtosis

  // Normality test (simplified)
  stats.is_normal_distribution =
      (fabs(stats.skewness) < 0.5f) && (fabs(stats.kurtosis) < 0.5f) &&
      (stats.outlier_count < samples * 0.05f); // < 5% outliers
}

// Enhanced voltage reading with statistical choice
float ResistorDividerCalibrator::read_voltage_robust(
    adc1_channel_t channel, int samples,
    bool use_median) { // Remove default value here
  if (use_median || samples >= 100) {
    std::vector<int> raw_values(samples); // Now std::vector is available
    for (int i = 0; i < samples; ++i) {
      raw_values[i] = fast_adc1_get_raw_inline(channel);
      vTaskDelay(pdMS_TO_TICKS(2));
    }

    std::sort(raw_values.begin(),
              raw_values.end()); // Now std::sort is available
    int median_raw =
        (samples % 2 == 0)
            ? (raw_values[samples / 2 - 1] + raw_values[samples / 2]) / 2
            : raw_values[samples / 2];

    return esp_adc_cal_raw_to_voltage(median_raw, &adc_chars) / 1000.0f;
  } else {
    // Use existing mean-based approach
    return read_voltage_average(channel, samples);
  }
}

// Statistical analysis output function (add this)
void print_adc_analysis(const ADCStatistics &stats) {
  printf("ADC Statistical Analysis:\n");
  printf("  Mean: %.2f, Median: %.2f, StdDev: %.2f\n", stats.mean, stats.median,
         stats.stddev);
  printf("  Range: [%.0f, %.0f], Outliers: %d\n", stats.min_val, stats.max_val,
         stats.outlier_count);
  printf("  Skewness: %.3f, Kurtosis: %.3f\n", stats.skewness, stats.kurtosis);
  printf("  Distribution: %s\n",
         stats.is_normal_distribution ? "Normal" : "Non-normal");

  // Interpretation
  if (fabs(stats.skewness) > 0.5f) {
    printf("  -> %s skewed distribution\n",
           stats.skewness > 0 ? "Right" : "Left");
  }
  if (stats.kurtosis > 0.5f) {
    printf("  -> Heavy-tailed distribution (more outliers)\n");
  } else if (stats.kurtosis < -0.5f) {
    printf("  -> Light-tailed distribution (fewer outliers)\n");
  }

  if (!stats.is_normal_distribution) {
    printf("  -> Recommend using MEDIAN instead of MEAN\n");
  }
}

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
  printf("\n[Step 0] Short circuit the input (connect measurement points "
         "together) and press ENTER...\n");
  wait_for_enter();

  float v_short = read_voltage_average(channel_bottom, 1000);
  printf("Short Circuit Measurements:\n");
  printf("  V_SHORT = %.3f V\n", v_short);

  printf("\n[Step 1] Disconnect the unknown resistor (open input) and press "
         "ENTER...\n");
  wait_for_enter();

  float v_top_open = read_voltage_average(channel_top, 100);
  float v_bottom_open = read_voltage_average(channel_bottom, 100);

  printf("Open Circuit Measurements:\n");
  printf("  V_TOP = %.3f V\n", v_top_open);
  printf("  V_BOTTOM = %.3f V\n", v_bottom_open);

  printf("\n[Step 2] Connect known resistor (%.1f Ohm), then press ENTER...\n",
         R_known);
  wait_for_enter();

  float v_top = read_voltage_average(channel_top, 1000);
  float v_bottom = read_voltage_average(channel_bottom, 1000);

  printf("Known Resistor Measurements:\n");
  printf("  V_TOP = %.3f V\n", v_top);
  printf("  V_BOTTOM = %.3f V\n", v_bottom);

  float diff = v_top - v_bottom;
  if (diff <= 0.0f) {
    printf("Invalid voltage readings. Calibration failed.\n");
    return false;
  }

  // Step 1: Calculate R3 (VCC independent)
  float current = diff / R_known;
  r3_eff = v_bottom / current;

  // Step 2: Solve the two-equation system for Vgpio and R1
  // Equation 1: v_bottom = Vgpio * r3_eff / (r1_eff + R_known + r3_eff)
  // Equation 2: v_short = Vgpio * r3_eff / (r1_eff + r3_eff)

  // From equation 1: (r1_eff + R_known + r3_eff) = Vgpio * r3_eff / v_bottom
  // From equation 2: (r1_eff + r3_eff) = Vgpio * r3_eff / v_short
  // Subtracting: R_known = Vgpio * r3_eff * (1/v_bottom - 1/v_short)

  float vgpio_solved = R_known / (r3_eff * (1.0f / v_bottom - 1.0f / v_short));
  float r1_solved = (vgpio_solved * r3_eff / v_short) - r3_eff;

  // Update the calibration values
  v_gpio = vgpio_solved;
  r1_eff = r1_solved;

  printf("  Solved Vgpio = %.3f V\n", v_gpio);
  printf("  Estimated R1_eff = %.2f Ω\n", r1_eff);
  printf("  Estimated R3_eff = %.2f Ω\n", r3_eff);

  // Rest of your existing ADC threshold code...
  float mean_adc, sdev_adc;
  int threshold = get_adc_threshold_for_resistance_NonTip(R_known);
  printf("Modelled ADC threshold for %.1f Ohm: %d\n", R_known, threshold);
  calc_mean_stddev_adc(channel_bottom, 1000, mean_adc, sdev_adc);
  printf("Measured ADC threshold for %.1f Ohm: %.1f\n", R_known, mean_adc);
  printf("Measured Stddev at threshold: %.2f ADC units\n", sdev_adc);

  // Enhanced version with statistical analysis
  ADCStatistics adc_stats;
  calc_enhanced_adc_stats(channel_bottom, 1000, adc_stats);
  print_adc_analysis(adc_stats);

  // Choose appropriate central tendency measure
  float representative_adc =
      adc_stats.is_normal_distribution ? adc_stats.mean : adc_stats.median;

  printf("Representative ADC value: %.1f\n", representative_adc);

  // Verification loop (unchanged)
  printf("\n[Step 3] Connect another known resistor to verify calibration.\n");
  while (true) {
    char key = wait_for_key();
    if (key == 'q')
      break;

    float v_top_test = read_voltage(channel_top);
    float v_bottom_test =
        read_voltage_average(channel_bottom, 1000); // Use consistent sampling

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

    // In verification loop
    ADCStatistics stats;
    calc_enhanced_adc_stats(channel_bottom, 1000, stats);
    print_adc_analysis(stats);

    // Use robust measurement
    float v_bottom_robust =
        read_voltage_robust(channel_bottom, 1000, false); // Use mean
    // or
    // float v_bottom_robust = read_voltage_robust(channel_bottom, 1000, true);
    // // Use median
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

  printf("Using Vgpio = %.3f V from main calibration\n", v_gpio);

  printf("\n[Step 1 - R1-only] Connect known resistor (%.1f Ohm), then press "
         "ENTER...\n",
         R_known);
  wait_for_enter();

  float v_bottom =
      read_voltage_average(channel_bottom, 1000); // Consistent sampling
  if (v_bottom <= 0.0f || v_bottom >= v_gpio) {
    printf("Invalid voltage reading. Calibration failed.\n");
    return false;
  }

  // Correct formula for R1 calibration (not unknown resistance calculation)
  float r1_calc = (v_gpio * r3_to_use / v_bottom) - R_known - r3_to_use;

  r1_Ax_eff = r1_calc;
  r3_eff = r3_to_use; // Store the one used

  printf("Measured V_BOTTOM = %.3f V\n", v_bottom);
  printf("Estimated R1_Ax_eff = %.2f Ω    Using R3 = %.2f Ω\n", r1_Ax_eff,
         r3_eff);

  if (r1_calc > 1500 || r1_calc < 0) { // Adjusted reasonable range
    printf("The value for Estimated R1_Ax_eff is out of range. Check "
           "connections.\n");
    return false;
  }

  // Verification loop with consistent sampling
  printf("\n[Step 2] Connect another known resistor to verify calibration.\n");
  while (true) {
    char key = wait_for_key();
    if (key == 'q')
      break;

    float v_bottom_test =
        read_voltage_average(channel_bottom, 1000); // Consistent sampling

    if (v_bottom_test <= 0.0f) {
      printf("Invalid reading. Skipping...\n");
      continue;
    }

    // Runtime resistance calculation
    float R_est_single = ((v_gpio / v_bottom_test) - 1.0f) * r3_eff - r1_Ax_eff;

    int threshold = get_adc_threshold_for_resistance_Tip(R_est_single);
    printf("Modelled ADC threshold for %.1f Ohm: %d\n", R_est_single,
           threshold);

    float mean_adc, sdev_adc;
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
  size_t int_size = sizeof(int);
  err |= nvs_get_blob(handle, "CalVersion", &CalVersion, &int_size);

  nvs_close(handle);

  // Logic is inverted: we have to calibrate if there is an nvs error
  // or the calibration version is smaller than the current version
  //
  printf("CalVersion = %d", CalVersion);
  return (err == ESP_OK && !(CALIBRATION_VERSION > CalVersion));
}

bool ResistorDividerCalibrator::save_calibration_to_nvs(
    int version, const char *nvs_namespace) {
  nvs_handle_t handle;
  CalVersion = version;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return false;

  err |= nvs_set_blob(handle, "v_gpio", &v_gpio, sizeof(float));
  err |= nvs_set_blob(handle, "r1_eff", &r1_eff, sizeof(float));
  err |= nvs_set_blob(handle, "r3_eff", &r3_eff, sizeof(float));
  err |= nvs_set_blob(handle, "r1_Ax_eff", &r1_Ax_eff, sizeof(float));
  err |= nvs_set_blob(handle, "CalVersion", &CalVersion, sizeof(int));

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