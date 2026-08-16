// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Table-driven settings page -- replaces WiFiManager's params page
// (WiFiManagerParameter + wm.addParameter() + the separate prefill/save
// blocks that used to live in WaitForNewSettingsViaPortal()/
// saveParamsCallback() in network.cpp). Adding a setting is one row in
// kSettings (AppSettings.cpp) instead of touching four separate places.
//
// Two fields (StartUpWeapon, CyranoPisteName) carry real encoding logic
// beyond a plain get/put -- StartUpWeapon maps a single F/E/S char to a
// stored 0/1/2 uint8; CyranoPisteName maps a single R/B/Y/G/P color code
// to a stored full color-name string. Both are handled as explicit
// special cases in AppSettings.cpp rather than forced into the generic
// table -- see the comment there for why.
#ifndef APPSETTINGS_H
#define APPSETTINGS_H
#include "Singleton.h"
#include <cstddef>

class AsyncWebServerRequest;

enum class SettingType { BOOL, STRING, INT32, UINT16 };

struct SettingDescriptor {
  const char *key;        // Preferences key
  const char *ns;         // Preferences namespace ("credentials" or "scoringdevice")
  const char *label;      // Form label
  SettingType type;
  const char *defaultVal; // string form; parsed per type
  size_t maxLen;          // STRING type only: max input length (incl. null)
};

class AppSettings : public SingletonMixin<AppSettings> {
public:
  // Registers GET/POST /settings on NetWork::GetServer(). Call once at
  // boot, same lifetime as WebRemoteHandler -- not started in repeater
  // mode, matching the other handlers there.
  void begin();

private:
  friend class SingletonMixin<AppSettings>;
  AppSettings() = default;

  void handleGet(AsyncWebServerRequest *request);
  void handlePost(AsyncWebServerRequest *request);
  // Separate from handlePost() deliberately -- that path always ends in
  // ESP.restart() (every other row is boot-time config), but "Experimental"
  // must take effect immediately and must never be persisted to NVS (see
  // ExperimentalMode.h) -- a restart would silently reset it back to off.
  void handleExperimentalPost(AsyncWebServerRequest *request);
};

#endif // APPSETTINGS_H
