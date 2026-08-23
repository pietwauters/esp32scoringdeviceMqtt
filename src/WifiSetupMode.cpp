// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "WifiSetupMode.h"
#include "RTOSSettings.h"
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "WifiSetupMode";
static const uint32_t kModeTimeoutMs = 5 * 60 * 1000; // idle -> give up, back to normal boot

bool WifiSetupMode::IsPending() {
  Preferences prefs;
  prefs.begin("scoringdevice", true);
  bool pending = prefs.getBool("WifiSetupMode", false);
  prefs.end();
  return pending;
}

void WifiSetupMode::RequestAndReboot() {
  Preferences prefs;
  prefs.begin("scoringdevice", false);
  prefs.putBool("WifiSetupMode", true);
  prefs.end();
  // Deferred, not a synchronous delay()+restart() here -- this is called
  // from deep inside the UDPIOHandler observer-notify chain
  // (registerUiRoute -> InputChanged -> notify() -> NetWork::update()),
  // which runs *before* the HTTP handler that triggered it sends its own
  // response. Rebooting synchronously here would kill the connection
  // before the client ever got a reply. A short-lived task lets that
  // whole call chain unwind (including the response going out) first.
  xTaskCreatePinnedToCore(
      [](void *) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        ESP.restart();
      },
      "wifi_setup_reboot", STACK_WIFI_SETUP_REBOOT, nullptr,
      PRIORITY_WIFI_SETUP_REBOOT, nullptr, CORE_WIFI_SETUP_REBOOT);
}

static void ClearFlagAndRestart() {
  Preferences prefs;
  prefs.begin("scoringdevice", false);
  prefs.putBool("WifiSetupMode", false);
  prefs.end();
  delay(200);
  ESP.restart();
}

struct ScanEntry {
  String ssid;
  int32_t rssi;
};

void WifiSetupMode::Run() {
  Serial.begin(115200);
  ESP_LOGI(TAG, "Entering dedicated WiFi setup mode -- nothing else starts this boot");

  Preferences prefs;
  prefs.begin("credentials", true);
  int32_t pisteNr = prefs.getInt("pisteNr", -1);
  String apPassword = prefs.getString("AP_Password", "01041967");
  prefs.end();
  char temp[8];
  sprintf(temp, "%03d", pisteNr >= 0 ? (int)pisteNr : 500);
  String apSsid = "Piste_" + String(temp);

  // WiFi.persistent() defaults to true in the Arduino core -- every
  // WiFi.mode()/softAP()/begin() call writes straight through to the
  // driver's own NVS-backed config as a side effect of being called, not
  // only on a successful connect. Left on, just entering this mode (AP
  // only, no STA) was silently blanking the previously-saved STA
  // credentials with nothing ever submitted, and a submitted-but-wrong
  // password was overwriting a previously-good saved network before the
  // connect attempt below even ran (confirmed 2026-08-23). Off for the
  // whole session; the POST handler explicitly turns it back on only
  // after confirming the new connection actually succeeded.
  WiFi.persistent(false);

  // AP only, deliberately -- no STA connect attempt (nothing else is
  // running that needs external connectivity, and the whole point of
  // this mode is picking a *different* network), and no channel
  // coordination with a repeater/master piste (nothing else running to
  // coordinate for).
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAP(apSsid.c_str(), apPassword.c_str());
  ESP_LOGI(TAG, "AP '%s' up at %s", apSsid.c_str(),
           WiFi.softAPIP().toString().c_str());

  DNSServer dns;
  dns.start(53, "*", WiFi.softAPIP());

  // Plain synchronous WebServer, not AsyncWebServer/AsyncTCP -- see
  // WifiSetupMode.h's top comment for why.
  WebServer server(80);

  int scanCount = -1; // -1 = not yet scanned this session
  ScanEntry results[20];
  uint32_t lastScanMs = 0; // debounce guard, see the GET handler below

  // Self-contained, not linked from /style.css -- this mode is a
  // completely separate server (no AsyncWebServer, see this file's top
  // comment) with nothing to link to. Reuses the same visual language
  // (dark theme, centered/max-width panel, the .atlas-btn look) rather
  // than the same bytes: this page is far simpler than the real remote
  // (no bottom-nav, no swipe, no live polling), so a small inline block
  // covering just what it needs is more in keeping with this mode's own
  // "nothing else running" philosophy than pulling in the full stylesheet.
  const char *kStyle =
      "<style>"
      "*{box-sizing:border-box}"
      "body{margin:0;padding:20px 16px;min-height:100vh;min-height:100dvh;"
      "background-color:#0d0d0d;color:#fff;text-align:center;"
      "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif}"
      ".panel{max-width:420px;margin:0 auto;text-align:left}"
      "h2{text-align:center;font-size:20px}"
      "label{display:block;margin:14px 0 6px;font-size:14px;color:#ccc}"
      "select,input[type=password]{width:100%;background-color:#1a1a1a;color:#fff;"
      "border:1px solid #444;border-radius:8px;padding:10px;font-size:15px}"
      ".btn{display:block;width:100%;background-color:#17375E;color:#fff;border:none;"
      "border-radius:12px;padding:14px;margin:14px 0;font-size:16px;cursor:pointer;"
      "text-align:center;text-decoration:none;box-sizing:border-box}"
      ".btn.disabled{opacity:0.5;pointer-events:none}"
      "p{text-align:center}"
      ".hint{color:#aaa;font-size:13px}"
      "</style>";

  // Rendered two ways depending on whether a scan has happened yet this
  // session -- scanCount < 0 means never. Scanning used to run
  // unconditionally on the very first GET, which made the page itself
  // (not just "Rescan") take as long as WiFi.scanNetworks() to appear --
  // confusing when the whole point of showing something fast here is so
  // the user isn't left staring at a blank load after the reboot. Now the
  // page always renders instantly; scanning only ever happens because the
  // user explicitly asked for it (Scan/Rescan link), same call either way.
  auto renderPage = [&]() {
    String html = "<html><head><title>WiFi Setup</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += kStyle;
    html += "</head><body><div class='panel'>";
    html += "<h2>Connect " + apSsid + " to WiFi</h2>";
    // Disables itself on click (className -> 'btn disabled', text swapped
    // to a wait message) -- the debounce guard in the GET handler below
    // is what actually stops a second scan from running, but this stops
    // an impatient extra tap from even looking like it did nothing.
    const char *kScanOnClick =
        " onclick=\"this.className='btn disabled';"
        "this.textContent='Scanning\\u2026';\"";
    if (scanCount < 0) {
      html += "<p>Tap Scan to list nearby WiFi networks.</p>";
      html += String("<a class='btn' href='/wifi?rescan=1'") + kScanOnClick +
              ">Scan for networks</a>";
      html += "<p class='hint'>This can take several seconds -- please "
              "wait, don't tap again.</p>";
    } else {
      html += "<form method='POST' action='/wifi'>";
      html += "<label>Network</label><select name='ssid'>";
      if (scanCount == 0) {
        html += "<option value=''>No networks found</option>";
      }
      for (int i = 0; i < scanCount && i < 20; i++) {
        html += "<option value='" + results[i].ssid + "'>" + results[i].ssid +
                " (" + String(results[i].rssi) + " dBm)</option>";
      }
      html += "</select>";
      html += "<label>Password</label><input type='password' name='pass'>";
      html += "<button type='submit' class='btn'>Connect</button>";
      html += "</form>";
      html += String("<a class='btn' href='/wifi?rescan=1'") + kScanOnClick +
              ">Rescan</a>";
      html += "<p class='hint'>Rescanning can take several seconds -- "
              "please wait, don't tap again.</p>";
    }
    html += "</div></body></html>";
    return html;
  };

  server.on("/wifi", HTTP_GET, [&]() {
    // This WebServer is synchronous/single-client (see this file's top
    // comment) -- while WiFi.scanNetworks() blocks below, a second tap's
    // request just sits queued at the TCP level, then gets handled for
    // real right after, indistinguishable from a fresh request. Without
    // this guard that meant every extra impatient tap queued up its own
    // full rescan, one after another, silently multiplying the wait.
    // kScanDebounceMs skips re-scanning (serving the just-fetched results
    // instead) for anything that arrives within it of the previous scan's
    // completion -- long enough to absorb a burst of taps, short enough
    // that a genuine "scan again a bit later" still gets a real rescan.
    const uint32_t kScanDebounceMs = 3000;
    if (server.hasArg("rescan") &&
        (scanCount < 0 || millis() - lastScanMs > kScanDebounceMs)) {
      ESP_LOGI(TAG, "Scanning...");
      // Ordinary blocking scan -- safe here specifically because nothing
      // else (no AsyncWebServer, no MQTT, no FSM) is running to conflict
      // with it. Same call NetWork::begin() already makes successfully
      // at normal boot. Only reached on an explicit user request now, so
      // this blocking wait is expected rather than surprising.
      int n = WiFi.scanNetworks();
      scanCount = 0;
      for (int i = 0; i < n && scanCount < 20; i++) {
        results[scanCount].ssid = WiFi.SSID(i);
        results[scanCount].rssi = WiFi.RSSI(i);
        scanCount++;
      }
      WiFi.scanDelete();
      lastScanMs = millis();
      ESP_LOGI(TAG, "Scan complete: %d networks", scanCount);
    }
    server.send(200, "text/html; charset=utf-8", renderPage());
  });

  server.on("/wifi", HTTP_POST, [&]() {
    if (!server.hasArg("ssid")) {
      server.send(400, "text/plain", "ssid is required");
      return;
    }
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    server.send(200, "text/plain",
                "Connecting -- device will restart shortly regardless of outcome.");
    delay(300); // let the response above flush before WiFi.begin() disrupts the AP
    // Still non-persistent here (Run() set that up) -- this attempt, and
    // any earlier one this session, cannot touch flash no matter how it
    // ends.
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t deadline = millis() + 15000;
    while (millis() < deadline && WiFi.status() != WL_CONNECTED)
      delay(200);
    bool connected = WiFi.status() == WL_CONNECTED;
    ESP_LOGI(TAG, "Connect to '%s': %s", ssid.c_str(), connected ? "OK" : "FAILED");
    if (connected) {
      // Only now commit the new credentials to flash, and only because
      // they're verified working -- re-issuing begin() with persistent
      // back on writes the config through; already-associated with this
      // AP, so this is just a flash write; no interruption before restart
      // below.
      WiFi.persistent(true);
      WiFi.begin(ssid.c_str(), pass.c_str());
      delay(100);
    }
    ClearFlagAndRestart();
  });

  server.on("/", HTTP_GET, [&]() {
    String html = "<html><head><title>WiFi Setup</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += kStyle;
    html += "</head><body><div class='panel'><h2>WiFi Setup Mode</h2>"
            "<a class='btn' href='/wifi'>Configure WiFi</a></div></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.begin();
  ESP_LOGI(TAG, "Setup web server started");

  uint32_t modeStart = millis();
  while (true) {
    server.handleClient();
    dns.processNextRequest();
    if (millis() - modeStart > kModeTimeoutMs) {
      ESP_LOGW(TAG, "Setup mode timed out with no activity, returning to normal operation");
      ClearFlagAndRestart();
    }
    delay(2);
  }
}
