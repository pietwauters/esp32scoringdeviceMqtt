// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "TaskDiagnostics.h"
#include "RTOSSettings.h"
#include <cstdio>

#if ENABLE_STACK_HWM_LOGGING

namespace {
constexpr int kMaxTracked = 12;
TaskHandle_t s_handles[kMaxTracked];
const char *s_names[kMaxTracked];
int s_count = 0;

void LoggerTask(void *) {
  while (true) {
    printf("[TaskDiag] ---- min free stack ever seen ----\n");
    for (int i = 0; i < s_count; i++) {
      UBaseType_t freeBytes = uxTaskGetStackHighWaterMark(s_handles[i]);
      printf("[TaskDiag] %-20s %u bytes\n", s_names[i], (unsigned)freeBytes);
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}
} // namespace

void TaskDiagnostics::Register(TaskHandle_t handle, const char *name) {
  if (!handle || s_count >= kMaxTracked)
    return;
  s_handles[s_count] = handle;
  s_names[s_count] = name;
  s_count++;
}

void TaskDiagnostics::Begin() {
  if (s_count == 0)
    return;
  xTaskCreatePinnedToCore(LoggerTask, "task_diag", 2560, nullptr, 1, nullptr,
                          0);
}

#else

void TaskDiagnostics::Register(TaskHandle_t, const char *) {}
void TaskDiagnostics::Begin() {}

#endif
