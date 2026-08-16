// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// platformio.ini sets -DLOG_LOCAL_LEVEL=0 (ESP_LOG_NONE) globally, silencing
// all ESP_LOGx output including warnings/errors. Overridden here (before any
// include that might transitively pull in esp_log.h) so this file's BLOCK_SIDE/
// UNBLOCK_SIDE logging is visible while testing. Same pattern as Opp2Handler.cpp.
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "ST37_AIHandler.h"
#include "3WeaponSensor.h"
#include "ExperimentalMode.h"
#include "esp_log.h"
#include <ArduinoJson.h>
#include <cstring>

static const char *AI_TAG = "x_ST37_AI";

// Experimental, same gate as blocking_time_ms/blade_contact_block_ms
// (ST37_SettingsManagerHandler.cpp) -- BLOCK_SIDE is refused while
// Experimental mode is off, and ExperimentalMode.cpp forcibly clears any
// active block the moment the flag is turned off, so a block can never
// outlive the mode that permitted it.
static void ApplySideBlock(const char *side, bool blocked) {
  bool isLeft;
  if (strcmp(side, "left") == 0) {
    isLeft = true;
  } else if (strcmp(side, "right") == 0) {
    isLeft = false;
  } else {
    ESP_LOGW(AI_TAG, "  unknown side '%s' -- ignored", side);
    return;
  }
  if (!IsExperimentalModeEnabled()) {
    ESP_LOGW(AI_TAG, "  %s side=%s ignored -- Experimental mode is off",
             blocked ? "BLOCK_SIDE" : "UNBLOCK_SIDE", side);
    return;
  }
  MultiWeaponSensor &sensor = MultiWeaponSensor::getInstance();
  if (isLeft) {
    sensor.SetLeftSideBlocked(blocked);
  } else {
    sensor.SetRightSideBlocked(blocked);
  }
  ESP_LOGI(AI_TAG, "  side=%s -> %s", side, blocked ? "BLOCKED" : "unblocked");
}

void ST37_AIHandler::ProcessMessage(const char *topic, const char *payload,
                                    size_t length) {
  ESP_LOGI(AI_TAG, "[x_ST37_AI] %s (%u bytes)", topic, (unsigned)length);

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    ESP_LOGW(AI_TAG, "JSON parse failed: %s", err.c_str());
    return;
  }

  const char *command = doc["command"] | "";
  const char *side = doc["side"] | "";

  if (strcmp(command, "BLOCK_SIDE") == 0) {
    ApplySideBlock(side, true);
  } else if (strcmp(command, "UNBLOCK_SIDE") == 0) {
    ApplySideBlock(side, false);
  } else {
    // TODO: other commands remain unhandled -- logged only, until defined.
    ESP_LOGI(AI_TAG, "  (unhandled) command=%s side=%s", command, side);
  }
}
