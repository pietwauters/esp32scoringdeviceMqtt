// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// platformio.ini sets -DLOG_LOCAL_LEVEL=0 (ESP_LOG_NONE) globally, silencing
// all ESP_LOGx output. Overridden here (before any include that might
// transitively pull in esp_log.h) so the ON/OFF toggle log is visible while
// testing. Same pattern as Opp2Handler.cpp.
// #undef LOG_LOCAL_LEVEL
// #define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "ExperimentalMode.h"
#include "3WeaponSensor.h"
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
  ApplyExperimentalBladeContactBlock(enabled);
  if (!enabled) {
    // A side-block (x_ST37_AI BLOCK_SIDE) has no NVS-stored default to
    // revert to -- it's an ephemeral referee/AI action, not a preference --
    // so turning Experimental off is what guarantees it can't outlive the
    // flag that permitted it in the first place.
    MultiWeaponSensor::getInstance().ClearSideBlocks();
  }
  // Future experiments: add their own Apply...(enabled) call here.
}
