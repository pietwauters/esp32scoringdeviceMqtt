// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// platformio.ini sets -DLOG_LOCAL_LEVEL=0 (ESP_LOG_NONE) globally, silencing
// all ESP_LOGx output including warnings/errors. Overridden here (before any
// include that might transitively pull in esp_log.h) so this file's
// blocking_time_ms/blade_contact_block_ms logging is visible while testing.
// Same pattern as Opp2Handler.cpp.
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "ST37_SettingsManagerHandler.h"
#include "ExperimentalMode.h"
#include "TimingConstants.h"
#include "esp_log.h"
#include <ArduinoJson.h>
#include <cstring>

static const char *SETTINGS_MGR_TAG = "x_ST37_SettingsMgr";

// Sanity bound for blocking_time_ms values -- not a fencing-accuracy bound
// (the FIE tolerances are documented in TimingConstants.h), just a guard
// against a malformed/garbage MQTT payload disabling lockout entirely (0)
// or freezing hit detection (an absurdly large value).
static const int kMinBlockingTimeMs = 1;
static const int kMaxBlockingTimeMs = 2000;

static void ApplyBlockingTime(const char *weaponCode, int ms) {
  if (ms < kMinBlockingTimeMs || ms > kMaxBlockingTimeMs) {
    ESP_LOGW(SETTINGS_MGR_TAG,
             "  blocking_time_ms.%s=%d out of range [%d,%d] -- ignored",
             weaponCode, ms, kMinBlockingTimeMs, kMaxBlockingTimeMs);
    return;
  }
  // Always persisted to NVS; only applied to the live variable while
  // Experimental mode is on (see TimingConstants.cpp / ExperimentalMode.h).
  if (!SetExperimentalBlockingTimeMs(weaponCode, ms)) {
    ESP_LOGW(SETTINGS_MGR_TAG,
             "  blocking_time_ms: unknown weapon code '%s' -- ignored",
             weaponCode);
    return;
  }
  ESP_LOGI(SETTINGS_MGR_TAG, "  blocking_time_ms.%s -> %d ms (stored%s)",
           weaponCode, ms,
           IsExperimentalModeEnabled() ? ", applied live"
                                        : ", Experimental mode is off");
}

// blade_contact_block_ms has no accuracy tolerance to guard (unlike
// blocking time, it isn't an FIE-specified value) -- the bound here is
// purely a garbage-input guard. 0 is valid (explicitly disables it).
static const int kMinBladeContactBlockMs = 0;
static const int kMaxBladeContactBlockMs = 2000;

static void ApplyBladeContactBlock(const char *weaponCode, int ms) {
  if (ms < kMinBladeContactBlockMs || ms > kMaxBladeContactBlockMs) {
    ESP_LOGW(SETTINGS_MGR_TAG,
             "  blade_contact_block_ms.%s=%d out of range [%d,%d] -- ignored",
             weaponCode, ms, kMinBladeContactBlockMs, kMaxBladeContactBlockMs);
    return;
  }
  if (!SetExperimentalBladeContactBlockMs(weaponCode, ms)) {
    ESP_LOGW(SETTINGS_MGR_TAG,
             "  blade_contact_block_ms: weapon code '%s' not applicable "
             "(foil/sabre only) -- ignored",
             weaponCode);
    return;
  }
  ESP_LOGI(SETTINGS_MGR_TAG, "  blade_contact_block_ms.%s -> %d ms (stored%s)",
           weaponCode, ms,
           IsExperimentalModeEnabled() ? ", applied live"
                                        : ", Experimental mode is off");
}

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

  JsonVariantConst blockingTime = doc["blocking_time_ms"];
  if (blockingTime.is<JsonObjectConst>()) {
    for (JsonPairConst kv : blockingTime.as<JsonObjectConst>()) {
      ApplyBlockingTime(kv.key().c_str(), kv.value().as<int>());
    }
  }

  JsonVariantConst bladeContactBlock = doc["blade_contact_block_ms"];
  if (bladeContactBlock.is<JsonObjectConst>()) {
    for (JsonPairConst kv : bladeContactBlock.as<JsonObjectConst>()) {
      ApplyBladeContactBlock(kv.key().c_str(), kv.value().as<int>());
    }
  }

  // TODO: settings besides blocking_time_ms/blade_contact_block_ms remain
  // unhandled -- logged only, until the target mechanism for each is
  // designed.
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    if (strcmp(kv.key().c_str(), "blocking_time_ms") != 0 &&
        strcmp(kv.key().c_str(), "blade_contact_block_ms") != 0) {
      ESP_LOGI(SETTINGS_MGR_TAG, "  (unhandled) setting: %s = %s",
               kv.key().c_str(), kv.value().as<String>().c_str());
    }
  }
}
