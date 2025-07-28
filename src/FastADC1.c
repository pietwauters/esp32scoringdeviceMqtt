#include "driver/adc.h"
#include "esp32/rom/ets_sys.h"
#include "soc/sens_struct.h"
#include "soc/sens_reg.h"
#include "soc/rtc_io_reg.h"
#include "soc/rtc.h"
#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "soc/sens_reg.h"
#include "soc/sens_struct.h"
#include "esp_rom_sys.h"


void adc1_fast_begin(void)
{
    // Set ADC1 width to 12-bit (max resolution)
    adc1_config_width(ADC_WIDTH_BIT_12);

    // Configure attenuation for all 8 ADC1 channels to 11dB
    for (int ch = 0; ch < 8; ++ch) {
        adc1_config_channel_atten((adc1_channel_t)ch, ADC_ATTEN_DB_11);
    }

    // Tell hardware to use "RTC" mode (instead of digital controller)
    SENS.sar_meas_start1.meas1_start_force = 1;
    SENS.sar_meas_start1.sar1_en_pad_force = 1;

    // Optional: Set ADC clock divider (default is typically fine)
    SENS.sar_read_ctrl.sar1_clk_div = 1;  // 80 MHz / (2 + 1) = ~26.7 MHz

    // Set 12-bit sampling (0–4095 range)
    SENS.sar_read_ctrl.sar1_sample_bit = 3;  // 3 = 12-bit

// Perform some dummy reads to stabilize the ADC

  int test = adc1_get_raw(ADC1_CHANNEL_3);
  test = adc1_get_raw(ADC1_CHANNEL_4);
  test = adc1_get_raw(ADC1_CHANNEL_6);
  test = adc1_get_raw(ADC1_CHANNEL_7);
  test = adc1_get_raw(ADC1_CHANNEL_0);
}

int fast_adc1_get_raw(adc1_channel_t channel)
{
    // Select only the desired ADC1 channel
    SENS.sar_meas_start1.sar1_en_pad = 1 << channel;

    // Start conversion
    SENS.sar_meas_start1.meas1_start_sar = 1;

    // Wait until conversion is done
    while (SENS.sar_meas_start1.meas1_done_sar == 0) {
        // Tight loop, fast polling
    }

    // Clear the start bit (recommended for safe reuse)
    SENS.sar_meas_start1.meas1_start_sar = 0;

    // Return raw ADC result
    return SENS.sar_meas_start1.meas1_data_sar;
}


#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "soc/sens_reg.h"
#include "soc/sens_struct.h"
#include "esp_rom_sys.h"

static uint32_t channel_mask = 0;

void adc1_fast_register_channel(adc1_channel_t channel) {
    if (channel >= ADC1_CHANNEL_0 && channel <= ADC1_CHANNEL_7) {
        channel_mask |= (1 << channel);
    }
}

void adc1_fast_begin_unsafe(void) {
    for (int ch = 0; ch < 8; ch++) {
        if ((channel_mask >> ch) & 1) {
            gpio_num_t gpio = ADC1_CHANNEL_0 + ch;
            rtc_gpio_init(gpio);  // Connect pad to RTC controller
            rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_DISABLED);  // No pullup/down
            adc1_config_channel_atten(ch, ADC_ATTEN_DB_11);  // 11 dB full range
        }
    }

    adc1_config_width(ADC_WIDTH_BIT_12);  // 12-bit resolution

    // Use RTC controller
    SENS.sar_read_ctrl.sar1_dig_force = 0;

    // Fastest safe ADC clock: 80 MHz / (1 + 1) = 40 MHz
    SENS.sar_read_ctrl.sar1_clk_div = 1;
    SENS.sar_read_ctrl.sar1_sample_bit = 3;  // 12-bit

    // Make sure RTC controller is allowed to start manually
    SENS.sar_meas_start1.meas1_start_force = 1;

}

uint16_t fast_adc1_get_raw_unsafe(adc1_channel_t channel) {
    // Select pad
    SENS.sar_meas_start1.sar1_en_pad = 1 << channel;

    // Start conversion
    SENS.sar_meas_start1.meas1_start_sar = 1;

    // Timeout counter (to avoid hanging)
    int timeout = 1000;
    while (SENS.sar_meas_start1.meas1_done_sar == 0 && --timeout > 0) {
        __asm__ __volatile__("nop");
    }

    if (timeout <= 0) {
        return 0xFFFF;  // Error marker
    }

    // Read result
    uint16_t result = SENS.sar_meas_start1.meas1_data_sar & 0xFFF;

    // Clear conversion flag
    SENS.sar_meas_start1.meas1_done_sar = 0;

    return result;
}
