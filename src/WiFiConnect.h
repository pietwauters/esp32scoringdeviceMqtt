// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Thin wrapper around the ESP32 WiFi driver's own native credential
// persistence (esp_wifi_get_config()/WiFi.begin()) -- replaces
// WiFiManager's getWiFiIsSaved()/getWiFiSSID()/getWiFiPass(), which
// themselves just called straight into esp_wifi_get_config(WIFI_IF_STA,
// ...) (confirmed by reading WiFiManager's own source). No separate
// storage of its own: a successful WiFi.begin(ssid, pass) already
// persists the credentials in the driver's own NVS-backed config,
// independent of this project's Preferences namespaces -- nothing here
// needs to duplicate that.
//
// Used by network.cpp's ConnectToExternalNetwork() (boot-time reconnect)
// and CaptivePortal (connect+save from the scan/connect form).
#ifndef WIFICONNECT_H
#define WIFICONNECT_H
#include <WiFi.h>

namespace WiFiConnect {

// True if the WiFi driver has a non-empty persisted STA SSID.
bool HasSavedNetwork();

// The persisted STA SSID, or an empty string if none.
String SavedSSID();

// Resumes the last successful connection (WiFi.begin() with no
// arguments -- the driver reconnects using its own persisted config).
// Blocks up to timeoutMs. Returns true if connected before the timeout.
bool ConnectToSaved(uint32_t timeoutMs);

// Connects with explicit credentials. On success, the driver persists
// them automatically -- nothing else needs to be saved. Blocks up to
// timeoutMs. Returns true if connected before the timeout.
bool ConnectAndSave(const char *ssid, const char *pass, uint32_t timeoutMs);

} // namespace WiFiConnect

#endif // WIFICONNECT_H
