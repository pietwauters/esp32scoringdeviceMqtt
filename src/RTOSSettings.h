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
// ESP-IDF 4.4. esp_timer.c is automatically patched by patch_esp_timer.py
// (executed as a pre-build script) to use the macro ESP_TIMER_TASK_CORE
// instead of a hardcoded value. This macro is injected via build_flags in
// platformio.ini — change it there to move the sensor task between cores.
//
// Patched file:
//   ~/.platformio/packages/framework-espidf/components/esp_timer/src/esp_timer.c
//   xTaskCreatePinnedToCore(..., &s_timer_task, ESP_TIMER_TASK_CORE);
// The patch is applied automatically on first build. If the ESP-IDF package
// is updated, the patch will be re-applied automatically on the next build.

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
#define CORE_UI_EVENT 0        // Opp2Handler::uiEventTask — UI event queue drain
#define CORE_FPA422 0        // FPA422Handler::fpa422Task — FPA422/RS422 output queue drain
#define CORE_WIFI_SETUP_REBOOT 0 // WifiSetupMode's deferred-restart task
#define CORE_MQTT_PUBLISH 0 // Opp2Handler::mqttPublishTask — control-message publish drain

// ---------------------------------------------------------------------------
// Task priorities  (higher number = higher priority)
// FreeRTOS idle = 0, typical app range 1-10, keep below configMAX_PRIORITIES
// ---------------------------------------------------------------------------
#define PRIORITY_LED_ANIMATOR 0    // LedStripAnimator  — purely cosmetic
#define PRIORITY_AUTOREF 1         // AutoRefHandler    — queue-driven
#define PRIORITY_STARTUP_DISPLAY 0 // StartupDisplayTask — one-shot startup
#define PRIORITY_LED_HANDLER 4     // LedStripHandler   — display updates
#define PRIORITY_STATE_MACHINE 6   // StateMachineHandler — 10 ms tick
#define PRIORITY_ARDUINO_TASK 3    // setup() + loop()  — below FSM/LED tasks
#define PRIORITY_UI_EVENT 2          // Opp2Handler::uiEventTask
#define PRIORITY_FPA422 2            // FPA422Handler::fpa422Task
#define PRIORITY_WIFI_SETUP_REBOOT 1 // WifiSetupMode's deferred-restart task
#define PRIORITY_MQTT_PUBLISH 2      // Opp2Handler::mqttPublishTask

// ---------------------------------------------------------------------------
// Stack sizes (bytes)
// Real measurements via ENABLE_STACK_HWM_LOGGING, a full session with a CMS
// attached including an END/match-completion cycle (2026-08-15) --
// uxTaskGetStackHighWaterMark, in bytes, i.e. free remaining at the deepest
// point seen, not bytes used:
//   LedStripAnimator   : 792 B used  (16384 → 4096, ~20x margin before, ~4x after)
//   LedStripHandler    : 568 B used  (16384 → 4096, ~7x margin after)
//   StateMachineHandler: 2800 B used (32768 → 8192, ~2.9x margin after,
//                        matches this file's own prior 8192 estimate)
//   arduino_task       : 3688 B used (16384 → 12288 -- cut less aggressively
//                        than the three above: loop() has branches this one
//                        session didn't exercise, e.g. repeater mode)
//   opp2_ui_evt        : 2692 B used, only 1404 B (34%) free at 4096 -- too
//                        tight, not oversized; raised to 8192 instead of
//                        shrunk. See Opp2Handler.cpp's UI_INPUT_CYRANO_END.
// opp2_mqtt_pub/AutoRefHandler/fpa422_upd already had healthy margins
// (3-9x) at their existing sizes, left unchanged.
// ---------------------------------------------------------------------------
#define STACK_AUTOREF 4096
#define STACK_LED_ANIMATOR 4096
#define STACK_LED_HANDLER 4096
#define STACK_STATE_MACHINE 8192
#define STACK_STARTUP_DISPLAY 2048
#define STACK_ARDUINO_TASK 12288 // setup() + loop()
#define STACK_UI_EVENT 8192          // Opp2Handler::uiEventTask
#define STACK_FPA422 4096            // FPA422Handler::fpa422Task
#define STACK_WIFI_SETUP_REBOOT 2048 // vTaskDelay(500ms) + ESP.restart() only
// Opp2Handler::mqttPublishTask — drains m_MqttPublishQueue, calls
// mqttClient.publish() with pre-built topic/payload buffers only (no JSON,
// no string building). Sized the same as STACK_UI_EVENT since the work
// shape is nearly identical, minus the mutex/notify overhead.
#define STACK_MQTT_PUBLISH 4096

// esp-mqtt's own internal "mqtt_task" (esp_mqtt_client_config_t::task_stack,
// mqtt_client.h) -- NOT one of ours, but never explicitly set either, so it
// was silently running at ESP-IDF's documented default of 6144 bytes.
// Confirmed overflowing that default on real hardware (2026-08-15):
// "***ERROR*** A stack overflow in task mqtt_task has been detected",
// reproducible on ending a match with a CMS attached (Tier A mTLS handshake
// + control publish, both stack-heavy, on the same task). Bumped well past
// the observed failure point rather than tuned to a measured minimum --
// unlike the STACK_* values above, there is no in-app TaskDiagnostics
// visibility into this task (it's not ours to Register()), so there's no
// cheap way to get a real headroom number here the way we did for the others.
#define STACK_MQTT_TASK 12288

// ---------------------------------------------------------------------------
// Stack high-water mark logging — set to 1 to enable, 0 to disable.
// TaskDiagnostics.h/.cpp implements this: every task created above calls
// TaskDiagnostics::Register(handle, name) right after creation (a no-op
// when this is 0, so zero cost to leave the calls in place); main.cpp
// calls TaskDiagnostics::Begin() once, at the end of setup(), to start a
// dedicated low-priority logger task that reports each registered task's
// uxTaskGetStackHighWaterMark() (minimum free stack ever seen, in bytes)
// every ~5 seconds via printf/Serial — deliberately not ESP_LOGI, so this
// stays visible regardless of CORE_DEBUG_LEVEL/LOG_LOCAL_LEVEL (both 0 in
// normal builds). Was enabled 2026-08-14/15 to size the STACK_* values
// above from real measurements (see that section's comment) -- settled now,
// back to 0. Flip back to 1 any time those need re-verifying.
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
// Small on purpose -- carries whole {topic,payload} structs (~230 bytes
// each), not bare uint32_t events, and NEXT/PREV/END are human-paced
// button presses, never realistically faster than this can drain even at
// 5s/publish worst case. Sender uses a short bounded timeout (not 0 like
// the queues above) since dropping a control command is a real missed
// user action, not a superseded stale value -- see EnqueueControlPublish().
#define QUEUE_DEPTH_MQTT_PUBLISH 4

#endif // RTOS_SETTINGS_H
