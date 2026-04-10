// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
//
// RTOSSettings.h — Central definition of FreeRTOS task core affinities and
// priorities. Edit here to change scheduling; do not scatter magic numbers
// through source files.
//
// ESP32 dual-core layout:
//   Core 0 (PRO_CPU): WiFi driver, lwIP tcpip_thread
//   Core 1 (APP_CPU): sensor (ESP_TIMER_TASK) + all application tasks
//
// NOTE: ESP_TIMER_TASK core affinity is NOT configurable via sdkconfig in
// ESP-IDF 4.4. esp_timer.c has been patched to use the macro
// ESP_TIMER_TASK_CORE instead of a hardcoded value. This macro is injected via
// build_flags in platformio.ini — change it there to move the sensor task
// between cores.
//
// Patched file:
//   ~/.platformio/packages/framework-espidf@3.40406.240122/components/esp_timer/src/esp_timer.c
//   xTaskCreatePinnedToCore(..., &s_timer_task, ESP_TIMER_TASK_CORE);
// If the patch is lost (package update), the build will fail with:
//   #error "ESP_TIMER_TASK_CORE is not defined"
// Re-apply the patch and the #error/#endif guard (see esp_timer.c).

#ifndef RTOS_SETTINGS_H
#define RTOS_SETTINGS_H

// ---------------------------------------------------------------------------
// Core assignments — one define per task for easy experimentation
// Valid values: 0 (PRO_CPU/WiFi core) or 1 (APP_CPU)
// ---------------------------------------------------------------------------
// CORE_SENSOR is driven by -DESP_TIMER_TASK_CORE in platformio.ini build_flags.
// Change it there — the same value reaches the patched esp_timer.c.
#ifndef ESP_TIMER_TASK_CORE
#error                                                                         \
    "ESP_TIMER_TASK_CORE is not defined. Add -DESP_TIMER_TASK_CORE=0 or 1 to build_flags in platformio.ini, and ensure esp_timer.c is patched."
#endif
#define CORE_SENSOR                                                            \
  ESP_TIMER_TASK_CORE          // ESP_TIMER_TASK — controlled via build_flags
#define CORE_STATE_MACHINE 0   // StateMachineHandler  — 10 ms FSM tick
#define CORE_AUTOREF 0         // AutoRefHandler       — long/double hit queue
#define CORE_LED_HANDLER 0     // LedStripHandler      — display updates
#define CORE_LED_ANIMATOR 0    // LedStripAnimator     — animations
#define CORE_STARTUP_DISPLAY 0 // StartupDisplayTask   — one-shot startup
#define CORE_ARDUINO_TASK 0    // setup() + loop()     — main Arduino task

// ---------------------------------------------------------------------------
// Task priorities  (higher number = higher priority)
// FreeRTOS idle = 0, typical app range 1-10, keep below configMAX_PRIORITIES
// ---------------------------------------------------------------------------
#define PRIORITY_LED_ANIMATOR 0    // LedStripAnimator  — purely cosmetic
#define PRIORITY_AUTOREF 1         // AutoRefHandler    — queue-driven
#define PRIORITY_STARTUP_DISPLAY 0 // StartupDisplayTask — one-shot startup
#define PRIORITY_LED_HANDLER 4     // LedStripHandler   — display updates
#define PRIORITY_STATE_MACHINE 6   // StateMachineHandler — 10 ms tick
#define PRIORITY_ARDUINO_TASK 1    // setup() + loop()  — below FSM/LED tasks

// ---------------------------------------------------------------------------
// Stack sizes (bytes)
// Measured worst-case usage (uxTaskGetStackHighWaterMark returns bytes here):
//   LedStripAnimator  : ~1592 B used  → 4096 B (2.5 KB headroom)
//   LedStripHandler   : not yet measured → 8192 B (conservative)
//   StateMachineHandler: ~2412 B used  → 8192 B (5.8 KB headroom)
//   AutoRefHandler    : ~1768 B used  → 4096 B (2.3 KB headroom)
// ---------------------------------------------------------------------------
#define STACK_AUTOREF 4096
#define STACK_LED_ANIMATOR 16384 // was 16384 — verified headroom OK
#define STACK_LED_HANDLER 16384  // was 16384 — not yet measured, conservative
#define STACK_STATE_MACHINE 32768 // was 32768 — verified headroom OK
#define STACK_STARTUP_DISPLAY 2048
#define STACK_ARDUINO_TASK 8192 // setup() + loop() — conservative

// ---------------------------------------------------------------------------
// Stack high-water mark logging — set to 1 to enable, 0 to disable
// Reports minimum free stack ever seen for each task every ~5 seconds.
// Disable once stack sizes are tuned.
// ---------------------------------------------------------------------------
#define ENABLE_STACK_HWM_LOGGING 0

// ---------------------------------------------------------------------------
// Queue depths (number of items)
// All queues carry uint32_t events. Senders use timeout=0 (drop-on-full)
// because events carry absolute state — the next event reflects the truth.
// ---------------------------------------------------------------------------
#define QUEUE_DEPTH_TIME_SCORE_DISPLAY 64 // TimeScoreDisplay event queue
#define QUEUE_DEPTH_LED_STRIP 64          // WS2812B_LedStrip main event queue
#define QUEUE_DEPTH_LED_ANIMATION 64      // WS2812B_LedStrip animation queue
#define QUEUE_DEPTH_AUTOREF 64            // AutoRef hit-event queue

#endif // RTOS_SETTINGS_H
