// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Opt-in stack high-water-mark logging, gated by
// RTOSSettings.h's ENABLE_STACK_HWM_LOGGING. Register() and Begin() are
// real no-ops (empty function bodies) when that flag is 0, so every call
// site can leave them in place unconditionally at zero runtime cost.
namespace TaskDiagnostics {

// Call once, right after creating a task you want tracked. `name` must
// outlive the program (pass a string literal).
void Register(TaskHandle_t handle, const char *name);

// Call once, after every Register() call has happened (end of setup()).
// Starts a low-priority background task that printf()s each registered
// task's uxTaskGetStackHighWaterMark() every ~5 seconds. printf, not
// ESP_LOGI -- this needs to stay visible regardless of
// CORE_DEBUG_LEVEL/LOG_LOCAL_LEVEL.
void Begin();

} // namespace TaskDiagnostics
