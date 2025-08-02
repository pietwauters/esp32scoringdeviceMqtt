#include <driver/gpio.h> // Ensure ESP32 GPIO driver is included

// ESP32 Register Addresses
#define GPIO_ENABLE_REG (DR_REG_GPIO_BASE + 0x20)    // GPIO 0-31 direction
#define GPIO_ENABLE1_REG (DR_REG_GPIO_BASE + 0x24)   // GPIO 32-39 direction
#define GPIO_OUT_W1TS_REG (DR_REG_GPIO_BASE + 0x08)  // GPIO 0-31 set HIGH
#define GPIO_OUT_W1TC_REG (DR_REG_GPIO_BASE + 0x0C)  // GPIO 0-31 set LOW
#define GPIO_OUT1_W1TS_REG (DR_REG_GPIO_BASE + 0x10) // GPIO 32-39 set HIGH
#define GPIO_OUT1_W1TC_REG (DR_REG_GPIO_BASE + 0x18) // GPIO 32-39 set LOW

// Precomputed masks for your GPIOs (21,23,25,5,18,19)
constexpr uint32_t LOWER_PINS =
    (1 << 21) | (1 << 23) | (1 << 25) | (1 << 5) | (1 << 18) | (1 << 19);
constexpr uint32_t HIGHER_PIN_33 =
    (1 << 1); // Bit 1 in higher registers (GPIO33 = 32 + 1)

// Disable pull-up/pull-down for GPIO33 (optional)
void disable_gpio33_pull() {
  REG_CLR_BIT(IO_MUX_GPIO33_REG,
              (1 << 7) | (1 << 6)); // Clear FUN_PU (bit7) and FUN_PD (bit6)
}

void Set_IODirectionAndValue(uint8_t direction, uint8_t values) {
  // --- Lower GPIOs (21,23,25,5,18,19) ---
  // 1. Direction (INPUT = 1, OUTPUT = 0)
  uint32_t enable_lower = REG_READ(GPIO_ENABLE_REG);
  enable_lower &= ~LOWER_PINS; // Clear existing bits for your pins
  enable_lower |= ((direction & 0x02) ? 0 : (1 << 21))    // GPIO21
                  | ((direction & 0x04) ? 0 : (1 << 23))  // GPIO23
                  | ((direction & 0x08) ? 0 : (1 << 25))  // GPIO25
                  | ((direction & 0x10) ? 0 : (1 << 5))   // GPIO5
                  | ((direction & 0x20) ? 0 : (1 << 18))  // GPIO18
                  | ((direction & 0x40) ? 0 : (1 << 19)); // GPIO19
  REG_WRITE(GPIO_ENABLE_REG, enable_lower);

  // 2. Output levels (only for OUTPUT pins)
  uint32_t lower_levels = ((values & 0x02) ? (1 << 21) : 0)    // GPIO21
                          | ((values & 0x04) ? (1 << 23) : 0)  // GPIO23
                          | ((values & 0x08) ? (1 << 25) : 0)  // GPIO25
                          | ((values & 0x10) ? (1 << 5) : 0)   // GPIO5
                          | ((values & 0x20) ? (1 << 18) : 0)  // GPIO18
                          | ((values & 0x40) ? (1 << 19) : 0); // GPIO19
  // Atomic writes (only modify your pins)
  REG_WRITE(GPIO_OUT_W1TS_REG, lower_levels);                 // Set HIGH
  REG_WRITE(GPIO_OUT_W1TC_REG, (~lower_levels) & LOWER_PINS); // Set LOW

  // --- Higher GPIO33 (32-39) ---
  if (direction & 0x01) {
    gpio_set_direction(GPIO_NUM_33, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_33, GPIO_FLOATING);
  } else {
    gpio_set_direction(GPIO_NUM_33, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_33, (values & 0x01));
  }
}