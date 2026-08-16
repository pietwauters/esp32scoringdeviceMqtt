// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "ST37_AIHandler.h"
#include "esp_log.h"
#include <ArduinoJson.h>

static const char *AI_TAG = "x_ST37_AI";

void ST37_AIHandler::ProcessMessage(const char *topic, const char *payload,
                                    size_t length) {
  ESP_LOGI(AI_TAG, "[x_ST37_AI] %s (%u bytes)", topic, (unsigned)length);

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    ESP_LOGW(AI_TAG, "JSON parse failed: %s", err.c_str());
    return;
  }

  // TODO: act on received commands (e.g. block signals from one side).
  // Deferred until the command set and apply mechanism are designed --
  // logging only for now.
  const char *command = doc["command"] | "";
  const char *side = doc["side"] | "";
  ESP_LOGI(AI_TAG, "  command=%s side=%s", command, side);
}
