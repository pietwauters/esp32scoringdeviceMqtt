// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "CaptivePortal.h"
#include "WiFiConnect.h"
#include "esp_log.h"
// network.h (not <ESPAsyncWebServer.h> directly) -- same include-order
// reason as WebRemoteHandler.h/AppSettings.cpp.
#include "network.h"

static const char *PORTAL_TAG = "CaptivePortal";
static bool s_routesRegistered = false;

void CaptivePortal::dnsTask(void *pv) {
  CaptivePortal *self = static_cast<CaptivePortal *>(pv);
  // Only this task touches m_dns after start() -- end() and the timeout
  // below both just clear m_dnsRunning/rely on this loop noticing, so
  // there's never a second task calling into DNSServer concurrently.
  while (self->m_dnsRunning && millis() < self->m_deadlineMs) {
    self->m_dns.processNextRequest();
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
  self->m_dns.stop();
  self->m_dnsRunning = false;
  self->m_dnsTask = nullptr;
  ESP_LOGI(PORTAL_TAG, "DNS redirect stopped (timeout or explicit end())");
  vTaskDelete(nullptr);
}

void CaptivePortal::handleRoot(AsyncWebServerRequest *request) {
  // Blocking, like the scans NetWork::begin()/FindAndSetMasterChannel()
  // already do elsewhere in this codebase -- a couple of seconds on a
  // rarely-used admin page, not a new pattern for this project.
  int n = WiFi.scanNetworks();
  String html = "<html><head><title>WiFi Setup</title></head><body>";
  html += "<h2>Connect to WiFi</h2><form method='POST' action='/wifi'>";
  html += "<select name='ssid'>";
  for (int i = 0; i < n; i++) {
    html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" +
            String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  html += "</select><br>Password: <input type='password' name='pass'><br>";
  html += "<input type='submit' value='Connect'>";
  html += "</form><p><a href='/settings'>Device settings</a></p>";
  html += "</body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

void CaptivePortal::handleConnect(AsyncWebServerRequest *request) {
  if (!request->hasParam("ssid", true)) {
    request->send(400, "text/plain", "ssid is required");
    return;
  }
  String ssid = request->getParam("ssid", true)->value();
  String pass =
      request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";
  // Blocks this one request for up to 15s -- runs on AsyncTCP's own
  // service task (16KB stack, see WebRemoteHandler.cpp's own comment on
  // this), so the FSM/scoring/MQTT keep running throughout; only other
  // concurrent HTTP traffic (e.g. the web remote's poll()) could see a
  // brief stall. A world better than the 120s full-device freeze this
  // replaces, and this is a deliberate, rare admin action -- not worth
  // more machinery to make fully async.
  bool ok = WiFiConnect::ConnectAndSave(ssid.c_str(), pass.c_str(), 15000);
  request->send(200, "text/plain",
                ok ? "Connected!" : "Failed to connect -- check the password and try again.");
  ESP_LOGI(PORTAL_TAG, "Connect attempt to '%s': %s", ssid.c_str(),
           ok ? "OK" : "FAILED");
}

void CaptivePortal::begin(uint32_t timeoutMs) {
  if (!s_routesRegistered) {
    NetWork::getInstance().GetServer().on(
        "/wifi", HTTP_GET,
        [this](AsyncWebServerRequest *request) { handleRoot(request); });
    NetWork::getInstance().GetServer().on(
        "/wifi", HTTP_POST,
        [this](AsyncWebServerRequest *request) { handleConnect(request); });
    s_routesRegistered = true;
    ESP_LOGI(PORTAL_TAG, "/wifi routes registered");
  }
  m_deadlineMs = millis() + timeoutMs; // re-triggering extends the timeout
  if (m_dnsRunning)
    return;
  m_dns.start(53, "*", WiFi.softAPIP());
  m_dnsRunning = true;
  xTaskCreatePinnedToCore(dnsTask, "captive_dns", 2048, this, 1, &m_dnsTask, 0);
  ESP_LOGI(PORTAL_TAG, "DNS redirect started (AP IP %s, timeout %ums)",
           WiFi.softAPIP().toString().c_str(), (unsigned)timeoutMs);
}

void CaptivePortal::end() {
  // Just clears the flag -- dnsTask notices within ~20ms and does its own
  // m_dns.stop()/cleanup (see dnsTask's comment on why only that task
  // touches m_dns).
  m_dnsRunning = false;
}
