// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "ExperimentalMode.h"
#include "TimingConstants.h"
#include "esp_log.h"

static const char *EXPERIMENTAL_TAG = "Experimental";

// In-memory only -- no Preferences/NVS access anywhere in this file.
// Zero-initialized, so it reads false from the moment the device boots.
static volatile bool s_ExperimentalMode = false;

bool IsExperimentalModeEnabled() { return s_ExperimentalMode; }

void SetExperimentalMode(bool enabled) {
  s_ExperimentalMode = enabled;
  ESP_LOGI(EXPERIMENTAL_TAG, "Experimental mode %s", enabled ? "ON" : "OFF");
  ApplyExperimentalBlockingTimes(enabled);
  // Future experiments: add their own Apply...(enabled) call here.
}
