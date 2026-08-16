// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "ST37_SettingsManagerHandler.h"
#include "esp_log.h"
#include <ArduinoJson.h>

static const char *SETTINGS_MGR_TAG = "x_ST37_SettingsMgr";

void ST37_SettingsManagerHandler::ProcessMessage(const char *topic,
                                                  const char *payload,
                                                  size_t length) {
  ESP_LOGI(SETTINGS_MGR_TAG, "[x_ST37_SettingsManager] %s (%u bytes)", topic,
           (unsigned)length);

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    ESP_LOGW(SETTINGS_MGR_TAG, "JSON parse failed: %s", err.c_str());
    return;
  }

  // TODO: apply received settings (e.g. blocking time) to live device
  // behavior. Deferred until the target mechanism is designed -- logging
  // only for now.
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    ESP_LOGI(SETTINGS_MGR_TAG, "  setting: %s = %s", kv.key().c_str(),
             kv.value().as<String>().c_str());
  }
}
