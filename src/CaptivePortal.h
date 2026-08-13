// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Replaces WiFiManager's captive-portal role (AP + DNS redirect + scan/
// connect form). Rides on the AP this device already runs continuously
// (WIFI_MODE_APSTA, see network.cpp's GlobalStartWiFi()) instead of
// starting its own -- begin()/end() only toggle DNS-redirect hijacking,
// they never touch WiFi mode or the existing STA connection. Routes are
// registered once and left in place permanently (see end()'s comment for
// why), so the settings/remote-control pages keep working the entire
// time the portal is up or down -- unlike the WiFiManager flow it
// replaces, which fully froze the device for up to 120s and wiped every
// other AsyncWebServer route in the process.
#ifndef CAPTIVEPORTAL_H
#define CAPTIVEPORTAL_H
#include "Singleton.h"
#include <DNSServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AsyncWebServerRequest;

class CaptivePortal : public SingletonMixin<CaptivePortal> {
public:
  // Registers /wifi (GET scan+form, POST connect) on NetWork::GetServer()
  // if not already done, and starts DNS-redirect hijacking so phones
  // auto-detect the portal while it's active. Safe to call repeatedly --
  // re-triggering just extends the timeout. Self-stops after timeoutMs
  // (default 120000, matching the WiFiManager portal's own
  // setConfigPortalTimeout(120) this replaces) so an accidental trigger
  // doesn't leave DNS hijacking running indefinitely.
  void begin(uint32_t timeoutMs = 120000);
  // Stops DNS-redirect hijacking only -- /wifi stays reachable directly
  // by URL afterward, it just won't auto-popup. Safe to call repeatedly.
  void end();

private:
  friend class SingletonMixin<CaptivePortal>;
  CaptivePortal() = default;

  void handleRoot(AsyncWebServerRequest *request);
  void handleConnect(AsyncWebServerRequest *request);

  DNSServer m_dns;
  volatile bool m_dnsRunning = false;
  volatile uint32_t m_deadlineMs = 0;
  TaskHandle_t m_dnsTask = nullptr;
  static void dnsTask(void *pv);
};

#endif // CAPTIVEPORTAL_H
