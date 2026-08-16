// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "AppSettings.h"
// network.h (not <ESPAsyncWebServer.h> directly) -- same include-order
// reason as WebRemoteHandler.h: network.h already pulls in
// <ESPAsyncWebServer.h> after <WiFiManager.h>, the only include order
// that doesn't collide (both define HTTP_GET/HTTP_POST/etc).
#include "network.h"
#include "ExperimentalMode.h"
#include "esp_log.h"
#include <Preferences.h>
#include <cstdio>
#include <cstring>

static const char *SETTINGS_TAG = "AppSettings";

// Exact same namespace/key/default as the WiFiManager-based version this
// replaces (network.cpp's now-removed WiFiManagerParameter globals) --
// this is a mechanism change, not a data-shape change, so existing NVS
// values keep reading correctly. StartUpWeapon and CyranoPisteName are
// deliberately NOT here -- their form values need real translation
// (F/E/S -> 0/1/2, R/B/Y/G/P -> a full color-name string) that doesn't
// fit the plain-get/put shape every other field has; forcing them into
// this table would need a per-row encode/decode hook for two rows out of
// eighteen, which is more machinery than just handling two special cases
// directly. See HandleStartUpWeapon()/HandleCyranoPisteName() below.
static const SettingDescriptor kSettings[] = {
    {"pisteNr", "credentials", "Piste Number", SettingType::INT32, "-1", 8},
    {"AP_Password", "credentials", "WiFi AP Password", SettingType::STRING,
     "01041967", 64},
    {"TryGlobalWiFi", "credentials", "Look for external network",
     SettingType::BOOL, "N", 1},
    {"CyranoPort", "credentials", "Cyrano Port", SettingType::UINT16,
     "50100", 8},
    {"CyranoBcPort", "credentials", "Cyrano Broadcast Port",
     SettingType::UINT16, "50100", 8},
    {"UseDHCP", "credentials", "Use DHCP", SettingType::BOOL, "N", 1},
    {"BaseAddress", "credentials", "IP Addressing base", SettingType::STRING,
     "172.20.255.1", 16},
    {"MqttBroker", "credentials", "MQTT Broker IP", SettingType::STRING,
     "10.154.1.130", 16},
    {"SmallDE", "scoringdevice", "Have a 2 period DE", SettingType::BOOL,
     "N", 1},
    {"MuteBuzzer", "scoringdevice", "Mute Buzzer", SettingType::BOOL, "N", 1},
    {"RepeaterMode", "scoringdevice", "Is this a repeater", SettingType::BOOL,
     "N", 1},
    {"Powersave", "scoringdevice", "Deep Sleep", SettingType::BOOL, "N", 1},
    {"MasterPiste", "scoringdevice", "Piste to repeat", SettingType::INT32,
     "-1", 8},
    {"MirrorLights", "scoringdevice", "Mirror lights", SettingType::BOOL,
     "N", 1},
    {"DisableBrownout", "scoringdevice", "Disable Brownout detection",
     SettingType::BOOL, "Y", 1},
    {"ForceCal", "scoringdevice", "Force Calibration", SettingType::BOOL,
     "N", 1},
    {"FPA422Enabled", "scoringdevice", "Enable Video/FPA422 output",
     SettingType::BOOL, "N", 1},
};
static const size_t kNumSettings = sizeof(kSettings) / sizeof(kSettings[0]);

static bool ToBool(const String &s) {
  if (s.length() == 0)
    return false;
  char c = s[0];
  return c == 'Y' || c == 'y' || c == '1' || s == "on"; // "on" -- HTML checkbox POST value
}

// ── StartUpWeapon: F/E/S <-> stored uint8 0/1/2 in "scoringdevice" ────────
static const char *ReadStartUpWeapon(Preferences &prefs) {
  switch (prefs.getUChar("START_WEAPON", 99)) {
  case 0:
    return "F";
  case 1:
    return "E";
  case 2:
    return "S";
  default:
    return "E";
  }
}
static void WriteStartUpWeapon(Preferences &prefs, const String &value) {
  uint8_t startweapon = 1; // default E, matches saveParamsCallback()'s prior default
  if (value.length() > 0) {
    switch (value[0]) {
    case 'F':
      startweapon = 0;
      break;
    case 'E':
      startweapon = 1;
      break;
    case 'S':
      startweapon = 2;
      break;
    }
  }
  prefs.putUChar("START_WEAPON", startweapon);
}

// ── CyranoPisteName: single color code <-> stored full color name ─────────
// Kept from the original: the stored string's first letter always matches
// the input code (Red/Blue/Yellow/Green/Podium), so re-submitting the
// prefilled full name still encodes correctly -- not accidental, the
// original relied on the same property.
static void WriteCyranoPisteName(Preferences &prefs, const String &value) {
  String pistename = "";
  if (value.length() > 0) {
    switch (value[0]) {
    case 'R':
    case 'r':
      pistename = "Red";
      break;
    case 'B':
    case 'b':
      pistename = "Blue";
      break;
    case 'Y':
    case 'y':
      pistename = "Yellow";
      break;
    case 'G':
    case 'g':
      pistename = "Green";
      break;
    case 'P':
    case 'p':
      pistename = "Podium";
      break;
    }
  }
  prefs.putString("Pistename", pistename.c_str());
}

static String ReadValue(const SettingDescriptor &d) {
  Preferences prefs;
  prefs.begin(d.ns, true); // read-only
  String result;
  switch (d.type) {
  case SettingType::BOOL:
    result = prefs.getBool(d.key, ToBool(d.defaultVal)) ? "Y" : "N";
    break;
  case SettingType::STRING:
    result = prefs.getString(d.key, d.defaultVal);
    break;
  case SettingType::INT32:
    result = String(prefs.getInt(d.key, atoi(d.defaultVal)));
    break;
  case SettingType::UINT16:
    result = String(prefs.getUShort(d.key, (uint16_t)atoi(d.defaultVal)));
    break;
  }
  prefs.end();
  return result;
}

static void WriteValue(const SettingDescriptor &d, const String &value) {
  Preferences prefs;
  prefs.begin(d.ns, false);
  switch (d.type) {
  case SettingType::BOOL:
    prefs.putBool(d.key, ToBool(value));
    break;
  case SettingType::STRING:
    prefs.putString(d.key, value.c_str());
    break;
  case SettingType::INT32:
    prefs.putInt(d.key, atoi(value.c_str()));
    break;
  case SettingType::UINT16:
    prefs.putUShort(d.key, (uint16_t)atoi(value.c_str()));
    break;
  }
  prefs.end();
}

// Same visual language as the rest of the remote (/style.css's
// .atlas-panel/.atlas-btn, dark theme, centered/responsive layout) --
// reachable via the same AsyncWebServer that already serves /style.css,
// so this just links it rather than duplicating anything. .settings-row
// (style.css) is the one addition that page didn't need before this --
// plain form inputs, nothing else on the remote has any.
void AppSettings::handleGet(AsyncWebServerRequest *request) {
  String html = "<html><head><title>Settings</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<link rel='stylesheet' type='text/css' href='/style.css'></head><body>";
  html += "<div class='atlas-panel' style='padding-bottom:24px'>";
  html += "<h2>Device Settings</h2><form method='POST' action='/settings'>";
  for (size_t i = 0; i < kNumSettings; i++) {
    const SettingDescriptor &d = kSettings[i];
    String value = ReadValue(d);
    html += "<div class='settings-row'><label>" + String(d.label) + "</label>";
    if (d.type == SettingType::BOOL) {
      html += "<input type='checkbox' name='" + String(d.key) + "'" +
              (value == "Y" ? " checked" : "") + ">";
    } else {
      html += "<input type='text' name='" + String(d.key) + "' value='" +
              value + "' maxlength='" + String((unsigned)d.maxLen - 1) + "'>";
    }
    html += "</div>";
  }
  // Two special-case fields, rendered the same way the generic ones are,
  // just sourced/saved through their own encode/decode instead of
  // ReadValue()/WriteValue().
  {
    Preferences prefs;
    prefs.begin("scoringdevice", true);
    String weapon = ReadStartUpWeapon(prefs);
    prefs.end();
    html += "<div class='settings-row'><label>Default Weapon at start-up (F/E/S)</label>"
            "<input type='text' name='StartUpWeapon' value='" +
            weapon + "' maxlength='8'></div>";
  }
  {
    // Pre-fills with the stored full color name (e.g. "Red"), not the
    // single-char code -- matches the original WiFiManager-based
    // behavior exactly (network.cpp's old WaitForNewSettingsViaPortal()
    // did the same). Resubmitting untouched still encodes correctly
    // since every color name's first letter matches its own code -- see
    // WriteCyranoPisteName()'s comment.
    Preferences prefs;
    prefs.begin("credentials", true);
    String pistename = prefs.getString("Pistename", "");
    prefs.end();
    html += "<div class='settings-row'><label>Cyrano Piste Colour (R/B/Y/G/P)</label>"
            "<input type='text' name='CyranoPisteName' value='" +
            pistename + "' maxlength='8'></div>";
  }
  html += "<button type='submit' class='atlas-btn atlas-wide'>Save and Restart</button>";
  html += "</form>";

  // Experimental: its own form/endpoint, deliberately outside the one
  // above -- that form always restarts the device on submit, which would
  // immediately reset this flag back to off (see ExperimentalMode.h and
  // handleExperimentalPost()'s comment in AppSettings.h). Applies
  // immediately, no restart, and is never written to NVS.
  //
  // Route is /experimental, NOT /settings/experimental -- this
  // ESPAsyncWebServer version's default URI matcher treats "/settings" as
  // matching "/settings" OR anything under "/settings/*" (WebHandlers.cpp:
  // `_uri != url && !url.startsWith(_uri + "/")`), so a nested path would
  // collide with the /settings POST handler above and route here into
  // handlePost() instead -- which happened once already and reset every
  // other field on the page to blank/default. A sibling path with no
  // shared prefix can't collide.
  html += "<form method='POST' action='/experimental'>";
  html += "<div class='settings-row'><label>Experimental</label>"
          "<input type='checkbox' name='Experimental'" +
          String(IsExperimentalModeEnabled() ? " checked" : "") + "></div>";
  html += "<button type='submit' class='atlas-btn atlas-wide'>Apply (no restart)</button>";
  html += "</form>";

  html += "<a class='atlas-btn atlas-wide' style='text-decoration:none;display:block;box-sizing:border-box' href='/remote'>&larr; Back to Remote</a>";
  html += "</div></body></html>";
  request->send(200, "text/html; charset=utf-8", html);
}

void AppSettings::handlePost(AsyncWebServerRequest *request) {
  for (size_t i = 0; i < kNumSettings; i++) {
    const SettingDescriptor &d = kSettings[i];
    String value =
        request->hasParam(d.key, true) ? request->getParam(d.key, true)->value()
        : (d.type == SettingType::BOOL ? "N" : ""); // unchecked checkbox: absent from POST body
    WriteValue(d, value);
  }
  {
    Preferences prefs;
    prefs.begin("scoringdevice", false);
    WriteStartUpWeapon(prefs, request->hasParam("StartUpWeapon", true)
                                   ? request->getParam("StartUpWeapon", true)->value()
                                   : "E");
    prefs.end();
  }
  {
    Preferences prefs;
    prefs.begin("credentials", false);
    WriteCyranoPisteName(prefs, request->hasParam("CyranoPisteName", true)
                                     ? request->getParam("CyranoPisteName", true)->value()
                                     : "");
    prefs.end();
  }
  request->send(200, "text/plain", "Saved. Restarting...");
  ESP_LOGI(SETTINGS_TAG, "Settings saved, restarting");
  delay(200); // let the response actually flush before the restart
  ESP.restart();
}

void AppSettings::handleExperimentalPost(AsyncWebServerRequest *request) {
  // Unchecked checkbox: absent from POST body -- same convention as the
  // BOOL rows above.
  bool enabled = request->hasParam("Experimental", true);
  SetExperimentalMode(enabled);
  ESP_LOGI(SETTINGS_TAG, "Experimental mode set to %s (runtime only, not persisted)",
           enabled ? "ON" : "OFF");
  request->redirect("/remote");
}

void AppSettings::begin() {
  NetWork::getInstance().GetServer().on(
      "/settings", HTTP_GET,
      [this](AsyncWebServerRequest *request) { handleGet(request); });
  NetWork::getInstance().GetServer().on(
      "/settings", HTTP_POST,
      [this](AsyncWebServerRequest *request) { handlePost(request); });
  // /experimental, NOT /settings/experimental -- see the comment on this
  // form in handleGet() for why a nested path would collide with the
  // /settings POST handler above (this ESPAsyncWebServer version's default
  // URI matcher treats "/settings" as matching anything under
  // "/settings/*" too).
  NetWork::getInstance().GetServer().on(
      "/experimental", HTTP_POST,
      [this](AsyncWebServerRequest *request) { handleExperimentalPost(request); });
  ESP_LOGI(SETTINGS_TAG, "Settings routes registered");
}
