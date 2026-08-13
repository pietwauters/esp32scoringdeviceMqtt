// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "WiFiConnect.h"
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace WiFiConnect {

String SavedSSID() {
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  return String(reinterpret_cast<const char *>(conf.sta.ssid));
}

bool HasSavedNetwork() { return SavedSSID().length() > 0; }

static bool WaitForConnection(uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (WiFi.status() == WL_CONNECTED)
      return true;
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool ConnectToSaved(uint32_t timeoutMs) {
  if (!HasSavedNetwork())
    return false;
  WiFi.begin();
  return WaitForConnection(timeoutMs);
}

bool ConnectAndSave(const char *ssid, const char *pass, uint32_t timeoutMs) {
  WiFi.begin(ssid, pass);
  return WaitForConnection(timeoutMs);
}

} // namespace WiFiConnect
