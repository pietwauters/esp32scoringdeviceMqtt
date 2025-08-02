#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"

static uint32_t channel_mask = 0;

// List of all RTC-capable GPIOs on ESP32
static const gpio_num_t rtc_gpio_list[] = {
    GPIO_NUM_0, GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
    GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_32, GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35,
    GPIO_NUM_36, GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39
};
#define NUM_RTC_GPIOS (sizeof(rtc_gpio_list)/sizeof(rtc_gpio_list[0]))

// Helper: get correct GPIO for ADC1 channel
static bool get_adc1_channel_gpio(adc1_channel_t channel, gpio_num_t *gpio) {
    return adc1_pad_get_io_num(channel, gpio) == ESP_OK;
}

void adc1_fast_register_channel(adc1_channel_t channel) {
    if (channel >= ADC1_CHANNEL_0 && channel <= ADC1_CHANNEL_7) {
        channel_mask |= (1 << channel);
    }
}

void adc1_fast_unregister_channel(adc1_channel_t channel) {
    if (channel >= ADC1_CHANNEL_0 && channel <= ADC1_CHANNEL_7) {
        gpio_num_t gpio;
        if (get_adc1_channel_gpio(channel, &gpio)) {
            rtc_gpio_deinit(gpio); // Return pad to digital domain
        }
        channel_mask &= ~(1 << channel);
    }
}

// Helper: check if a GPIO is registered as ADC1 channel
static bool is_adc1_registered_gpio(gpio_num_t gpio) {
    for (int ch = 0; ch < 8; ++ch) {
        if ((channel_mask >> ch) & 1) {
            gpio_num_t adc_gpio;
            if (get_adc1_channel_gpio((adc1_channel_t)ch, &adc_gpio) && adc_gpio == gpio) {
                return true;
            }
        }
    }
    return false;
}

// Save and restore direction for RTC GPIOs (expand as needed)
typedef struct {
    gpio_num_t gpio;
    uint32_t io_mux_reg;
    uint32_t io_mux_val;
    bool is_output;
} rtc_gpio_saved_config_t;

static rtc_gpio_saved_config_t rtc_gpio_saved[NUM_RTC_GPIOS];

// Helper: get correct IO_MUX register for a GPIO
static uint32_t get_io_mux_reg(gpio_num_t gpio) {
    switch (gpio) {
        case 0:  return IO_MUX_GPIO0_REG;
        case 2:  return IO_MUX_GPIO2_REG;
        case 4:  return IO_MUX_GPIO4_REG;
        case 12: return 0x3FF49034; // IO_MUX_MTDI_REG
        case 13: return 0x3FF49038; // IO_MUX_MTCK_REG
        case 14: return 0x3FF4903C; // IO_MUX_MTMS_REG
        case 15: return 0x3FF49040; // IO_MUX_MTDO_REG
        case 25: return IO_MUX_GPIO25_REG;
        case 26: return IO_MUX_GPIO26_REG;
        case 27: return IO_MUX_GPIO27_REG;
        case 32: return IO_MUX_GPIO32_REG;
        case 33: return IO_MUX_GPIO33_REG;
        case 34: return IO_MUX_GPIO34_REG;
        case 35: return IO_MUX_GPIO35_REG;
        case 36: return IO_MUX_GPIO36_REG;
        case 37: return IO_MUX_GPIO37_REG;
        case 38: return IO_MUX_GPIO38_REG;
        case 39: return IO_MUX_GPIO39_REG;
        default: return 0;
    }
}

// Helper: get direction (true = output, false = input)
static bool my_gpio_is_output(gpio_num_t gpio) {
    if (gpio < 32) {
        return REG_READ(GPIO_ENABLE_REG) & (1 << gpio);
    } else if (gpio < 40) {
        return REG_READ(GPIO_ENABLE1_REG) & (1 << (gpio - 32));
    }
    return false;
}

// Save config for all RTC-capable pins
void save_rtc_gpio_configs(void) {
    for (int i = 0; i < NUM_RTC_GPIOS; ++i) {
        gpio_num_t gpio = rtc_gpio_list[i];
        rtc_gpio_saved[i].gpio = gpio;
        rtc_gpio_saved[i].io_mux_reg = get_io_mux_reg(gpio);
        rtc_gpio_saved[i].io_mux_val = REG_READ(rtc_gpio_saved[i].io_mux_reg);
        rtc_gpio_saved[i].is_output = my_gpio_is_output(gpio);
    }
}

// Restore config for all RTC-capable pins except those used for ADC
void restore_rtc_gpio_configs(void) {
    for (int i = 0; i < NUM_RTC_GPIOS; ++i) {
        gpio_num_t gpio = rtc_gpio_saved[i].gpio;
        // Only restore if NOT registered as ADC
        if (!is_adc1_registered_gpio(gpio)) {
            // Restore IO_MUX register (pull-up/down, function, etc.)
            REG_WRITE(rtc_gpio_saved[i].io_mux_reg, rtc_gpio_saved[i].io_mux_val);
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, rtc_gpio_saved[i].is_output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
        }
        // If registered as ADC, do NOT restore—leave as is for ADC operation
    }
}

void adc1_fast_begin(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);

    for (int ch = 0; ch < 8; ++ch) {
        if ((channel_mask >> ch) & 1) {
            gpio_num_t gpio;
            if (get_adc1_channel_gpio((adc1_channel_t)ch, &gpio)) {
                rtc_gpio_init(gpio);  // Connect pad to RTC controller
                rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_DISABLED);  // No pullup/down
                adc1_config_channel_atten((adc1_channel_t)ch, ADC_ATTEN_DB_11);
            }
        }
        // If not registered, do nothing: leave pad untouched for other uses
    }

    //SENS.sar_meas_start1.meas1_start_force = 1;
    SENS.sar_meas_start1.sar1_en_pad_force = 1;
    SENS.sar_read_ctrl.sar1_clk_div = 1;
    SENS.sar_read_ctrl.sar1_sample_bit = 3;
}

void adc1_fast_begin_unsafe(void) {
    //save_rtc_gpio_configs();

    for (int ch = 0; ch < 8; ch++) {
        if ((channel_mask >> ch) & 1) {
            gpio_num_t gpio;
            if (get_adc1_channel_gpio((adc1_channel_t)ch, &gpio)) {
                rtc_gpio_init(gpio);  // Connect pad to RTC controller
                rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_DISABLED);  // No pullup/down
                adc1_config_channel_atten((adc1_channel_t)ch, ADC_ATTEN_DB_11);  // 11 dB full range
            }
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
    SENS.sar_meas_start1.sar1_en_pad_force = 1;

    //restore_rtc_gpio_configs();
}

int fast_adc1_get_raw(adc1_channel_t channel)
{
    SENS.sar_meas_start1.sar1_en_pad = 1 << channel;
    SENS.sar_meas_start1.meas1_start_sar = 1;

    while (SENS.sar_meas_start1.meas1_done_sar == 0) {
        // Tight loop, fast polling
    }

    SENS.sar_meas_start1.meas1_start_sar = 0;
    return SENS.sar_meas_start1.meas1_data_sar;
}

static inline uint16_t IRAM_ATTR fast_adc1_get_raw_unsafe(adc1_channel_t channel) {
    SENS.sar_meas_start1.sar1_en_pad = 1 << channel;
    SENS.sar_meas_start1.meas1_start_sar = 1;

    int timeout = 1000;
    while (SENS.sar_meas_start1.meas1_done_sar == 0 && --timeout > 0) {
        __asm__ __volatile__("nop");
    }

    if (timeout <= 0) {
        return 0xFFFF;  // Error marker
    }

    uint16_t result = SENS.sar_meas_start1.meas1_data_sar & 0xFFF;
    SENS.sar_meas_start1.meas1_done_sar = 0;
    return result;
}
