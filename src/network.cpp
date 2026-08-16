// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#include "network.h"
#include "AbsoluteTime.h"
#include "AsyncUDP.h"
#include "FlashWriteGuard.h"
#include "MDNSResolver.h"
#include "TierAProvisioning.h"
#include "WiFiConnect.h"
#include "WifiSetupMode.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
// Below is for OTA updates using a webserver
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <ElegantOTA.h>
static const char *NETWORK_TAG = "Network";

// The only AsyncWebServer instance on this device -- do not construct a
// second one anywhere. Confirmed by direct hardware testing (2026-08-12):
// a second concurrent AsyncWebServer instance corrupts/hangs any
// multi-packet response (>1 TCP segment, ~1460 bytes) on BOTH servers,
// including this one, even though nothing here changed -- single-instance
// operation is byte-perfect, two instances is not. All web routes
// (calibration/provision/OTA/WebRemoteHandler/...) go through
// NetWork::GetServer(), which returns this object.
AsyncWebServer server(80);

// Forward declaration for calibration HTML handler
String getCalibrationHtml();

// Tier A provisioning (docs/level2.md §30.5) — this device has no camera to scan a
// QR with, unlike a phone-based Tier B scoresheet, so the operator-relayed ticket
// code is entered here instead, on the device's own existing web server.
String getProvisionHtml(const String &sentParam) {
  bool has = TierAProvisioning::getInstance().HasCertificate();
  String html = "<html><head><meta charset='utf-8'>"
                "<title>Tier A Provisioning</title></head><body>";
  html += "<h2>Pair this device with the CMS</h2>";
  if (sentParam == "1")
    html += "<p><i>Request sent &mdash; reload this page in a few seconds to "
            "check the status below.</i></p>";
  else if (sentParam == "0")
    html += "<p><i>Could not start provisioning &mdash; a request may already "
            "be in flight (see device logs), or key generation failed.</i></p>";
  html += "<p><b>Device id:</b> " +
          String(TierAProvisioning::getInstance().GetOrCreateDeviceId().c_str()) +
          "</p>";
  html += "<p><b>Current status:</b> " +
          String(has ? "provisioned (certificate on file)"
                     : "not provisioned &mdash; connecting anonymously") +
          "</p>";

  TierAProvisioning::StorageDebugInfo dbg = TierAProvisioning::getInstance().GetStorageDebugInfo();
  html += "<p><b>NVS storage (diagnostic):</b><br>";
  html += "cert: " + String((unsigned)dbg.certLen) + " bytes<br>";
  html += "priv_key: " + String((unsigned)dbg.privKeyLen) + " bytes<br>";
  html += "ca_cert: " + String((unsigned)dbg.caCertLen) + " bytes<br>";
  html += "request in flight: " +
          String(dbg.requestPending
                     ? ("yes, sent " + String((unsigned)(dbg.requestAgeMs / 1000)) + "s ago")
                     : "no") +
          "</p>";

  html += "<form method='POST' action='/provision'>";
  html += "<label>Ticket code (from the CMS's \"Pair a scoring device\" "
          "screen): <input name='code' maxlength='6'></label><br>";
  html += "<label>Role: <select name='role'>"
          "<option value='apparatus'>Apparatus (scoring device)</option>"
          "<option value='scoresheet'>Scoresheet</option>"
          "<option value='remote'>Remote control</option>"
          "<option value='var'>Video review</option>"
          "</select></label><br>";
  html += "<button type='submit'>Provision</button>";
  html += "</form>";
  html += "<p>The device generates its own certificate request and reports the "
          "result over MQTT &mdash; this page only sends the ticket code, then "
          "reloads to show the outcome above (safe to refresh from here on, it "
          "won't resend the form).</p>";
  html += "</body></html>";
  return html;
}

AsyncWebServer &NetWork::GetServer() { return server; }

// Register endpoints and start server
//
// KNOWN GAP (2026-08-12, not fixed): server.reset() below wipes every
// registered route, including any WebRemoteHandler routes added after
// boot. This function only runs at boot in normal operation, so this is
// dormant today -- nothing left at runtime calls startCalibrationWebServer()
// again (the old WiFiManager-based WaitForNewSettingsViaPortal() did;
// its eventual replacement, WifiSetupMode, sidesteps this differently --
// it reboots into a separate minimal mode that never starts this
// AsyncWebServer at all, rather than calling into this function while
// the live app is running). Still worth fixing properly if anything else
// ever calls startCalibrationWebServer() at runtime: either a callback
// list this function invokes after re-registering its own routes, or
// moving WebRemoteHandler's route registration into this function
// directly.
void startCalibrationWebServer() {
  server.reset();
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Hi! I am ESP32.");
  });
  server.on("/calibration", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = getCalibrationHtml();
    request->send(200, "text/html", html);
  });
  server.on("/provision", HTTP_GET, [](AsyncWebServerRequest *request) {
    String sentParam = request->hasParam("sent") ? request->getParam("sent")->value() : "";
    request->send(200, "text/html; charset=utf-8", getProvisionHtml(sentParam));
  });
  server.on("/provision", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("code", true) || !request->hasParam("role", true)) {
      request->send(400, "text/plain", "code and role are required");
      return;
    }
    String code = request->getParam("code", true)->value();
    String role = request->getParam("role", true)->value();
    bool started = TierAProvisioning::getInstance().GenerateAndRequest(
        code.c_str(), role.c_str());
    // Redirect (not render) after the POST, so refreshing the resulting page
    // re-does a harmless GET instead of the browser re-submitting the form —
    // that resubmission is exactly what corrupted a real device's in-flight
    // request state before this fix.
    request->redirect(started ? "/provision?sent=1" : "/provision?sent=0");
  });
  server.begin();
}
// Forward declaration for calibration HTML handler
#include "adc_calibrator.h"

String getCalibrationHtml() {
  // Read calibration from NVS using ResistorDividerCalibrator
  ResistorDividerCalibrator calibrator;
  calibrator.load_calibration_from_nvs();
  String html = "<html><head><title>ADC Calibration</title></head><body>";
  html += "<h2>ADC Calibration Parameters</h2>";
  html += "<ul>";
  html +=
      "<li><b>v_gpio:</b> " + String(calibrator.get_v_gpio(), 4) + " V</li>";
  html +=
      "<li><b>r1_eff:</b> " + String(calibrator.get_r1_eff(), 2) + " Ω</li>";
  html +=
      "<li><b>r3_eff:</b> " + String(calibrator.get_r3_eff(), 2) + " Ω</li>";
  html += "<li><b>r1_Ax_eff:</b> " + String(calibrator.get_r1_Ax_eff(), 2) +
          " Ω</li>";
  html +=
      "<li><b>CalVersion:</b> " + String(calibrator.get_CalVersion()) + "</li>";
  html += "</ul>";
  html += "</body></html>";
  return html;
}
/**
   Sets all the channels back to 0.
*/
void NetWork::reset_channels() {
  for (int i = 0; i < CHANNEL_COUNT; i++) {
    channels[i].occupants = 0;
    channels[i].total_strength = 0;
    channels[i].max_strength = -999999;
  }
}

int NetWork::begin() {
  esp_log_level_set("wifi", ESP_LOG_ERROR); // Or ESP_LOG_NONE
  WiFi.disconnect();
  networkpreferences.begin("credentials", false);
  LookForExternalWiFi = networkpreferences.getBool("TryGlobalWiFi", false);
  networkpreferences.end();
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_AP, &conf);

  conf.ap.beacon_interval = 600; // default 100
  esp_wifi_set_config(WIFI_IF_AP, &conf);

  if (!LookForExternalWiFi)
    return 0;
  networks = WiFi.scanNetworks();

  if (WiFiConnect::HasSavedNetwork()) {
    String savedSSID = WiFiConnect::SavedSSID();
    for (int i = 0; i < networks; ++i) {
      if (WiFi.SSID(i) == savedSSID) {
        SavedNetworkExists = true;
        i = networks;
      }
    }
  }
  return networks;
}

int32_t NetWork::FindFirstFreePisteID(uint32_t RequestedPiste) {
  if (networks == -1) {
    WiFi.disconnect();
    networks = WiFi.scanNetworks();
  }
  int32_t TempPisteId = 0;
  int32_t LastPisteId = 0;
  bool RequestedPisteAlreadyInUse = false;
  for (int i = 0; i < networks; ++i) {

    if (sscanf(WiFi.SSID(i).c_str(), "%d", &TempPisteId)) {
      if (RequestedPiste == TempPisteId)
        RequestedPisteAlreadyInUse = true;
      if (TempPisteId > LastPisteId)
        LastPisteId = TempPisteId;
    }
  }
  if (!RequestedPisteAlreadyInUse)
    return RequestedPiste;

  return LastPisteId + 1;
}

int NetWork::findBestWifiChannel() {
  if (networks == -1) {
    WiFi.disconnect();
    networks = WiFi.scanNetworks();
  }
  if (networks == 0) {
    // it doesn't matter, we're on an island or a cage of Farady
    return 6;
  }
  reset_channels();
  // Creating a representation of all the networks
  bool bEmptyChannelsExist = false;
  int nonEmptyChannelCount = 0;
  for (int i = 0; i < networks; ++i) {
    int c = WiFi.channel(i);
    if (c < CHANNEL_COUNT) {
      channels[c - 1].occupants += 1;
      if (channels[c - 1].max_strength < WiFi.RSSI(i))
        channels[c - 1].max_strength = WiFi.RSSI(i);
    }
  }
  for (int i = 0; i < CHANNEL_COUNT; i++) {
    if (channels[i].occupants > 0) {
      // add 3dB per access point. This is overkill when the other ap's have
      // lower power than the highest
      channels[i].total_strength =
          channels[i].max_strength + (channels[i].occupants - 1) * 3;
      nonEmptyChannelCount++;
    } else
      bEmptyChannelsExist = true;
  }
  // At this point we know the power per channel
  // If there are "empty channels", we should use one them,
  // if not, we should use the one with the lowest esp_wifi_get_max_tx_power
  // From the empty channels, we should use the one with the least interference
  // from adjacent reset_channels
  int CurrentMinIndex = 0;
  long CurrentMin = 0;
  int proposal = 0;
  if (!bEmptyChannelsExist) {
    for (int i = 0; i < CHANNEL_COUNT - 1; i++) {
      if (channels[i].total_strength < CurrentMin) {
        CurrentMin = channels[i].total_strength;
        CurrentMinIndex = i;
      }
    }
    return CurrentMin;
  }
  // if one of the optimal channels is free, chose that one
  if (channels[0].occupants == 0) // chose 1 or 11
  {
    if (channels[10].occupants != 0) {
      return 0;
    } else {
      // you should chose 1 or 11 based on how "far" they are from other
      // interference for now always return 1
      return 0;
    }
  } else {
    if (channels[10].occupants == 0) {
      return 10;
    } else {
      if (channels[5].occupants == 0) {
        return 5;
      }
    }
  }
  // if we reach this point, none of the main channels are available

  interval_t intervals[10];
  int nr_intervals = 0;
  int max_width = 0;
  int max_width_index = 0;
  long best_interference = -999999999;

  // Find pairs of non-empty channels
  for (int i = 1; i < CHANNEL_COUNT - 1; i++) {
    int j = i;
    while (channels[j].occupants != 0)
      j++;
    intervals[nr_intervals].start = j;
    int k = j + 1;
    while (channels[k].occupants == 0)
      k++;
    intervals[nr_intervals].end = k - 1;

    if (k - j >= max_width) {
      long currentinterference =
          max(channels[j - 1].total_strength, channels[k].total_strength);
      if (k - j == max_width) { // both intervals are equally wide, so chose the
                                // one with the lowest adjacent power

        if (currentinterference < best_interference) {
          max_width = k - j;
          max_width_index = nr_intervals;
          best_interference = currentinterference;
        }
      } else {
        max_width = k - j;
        max_width_index = nr_intervals;
        best_interference = currentinterference;
      }
    }
    nr_intervals++;
    i = k - 1;
  }

  return ((intervals[max_width_index].end - intervals[max_width_index].start) /
              2 +
          intervals[max_width_index].start);
}

// DHCP lease pool start address for this piste's AP. The AP's own IP stays
// fixed at 192.168.4.1 everywhere -- only where the DHCP server starts
// handing out client addresses shifts per piste. Reduces (not eliminates)
// the chance that a remote control which just left a different piste's AP
// still holds a cached lease for 192.168.4.x, tries an INIT-REBOOT
// DHCPREQUEST for that stale address against the new piste's AP, gets
// NAK'd/ignored, and has to time out before falling back to a fresh
// DISCOVER. Pistes are numbered 1-64 in practice (500 is only the
// transient "unset" placeholder from FindFirstFreePisteID), so this is
// just spreading pools out, not guaranteeing uniqueness -- not needed.
static IPAddress DhcpLeaseStartForPiste(int32_t pisteNr) {
  // softAPConfig's lease pool is 10 addresses wide and must stay inside the
  // /24 subnet without overlapping the gateway (192.168.4.1) or running
  // past .254 -- clamp to a safe interior range regardless of pisteNr.
  int32_t octet = pisteNr;
  if (octet < 2)
    octet = 2;
  if (octet > 244)
    octet = 244;
  return IPAddress(192, 168, 4, octet);
}

// Recovers the piste number from the AP SSID ("Piste_XXX") for call sites
// that only have soft_ap_ssid in scope, not the originating int.
static int32_t PisteNrFromApSsid(const String &ssid) {
  return ssid.substring(6).toInt(); // "Piste_" is 6 chars; toInt()==0 on
                                     // parse failure, which clamps safely
}

static void ApplyPisteDhcpLease(int32_t pisteNr) {
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                     IPAddress(255, 255, 255, 0),
                     DhcpLeaseStartForPiste(pisteNr));
}

bool NetWork::ConnectToExternalNetwork(long ConnectTimeout) {
  if (bConnectedToExternalNetwork)
    return true;
  if (!WiFiConnect::HasSavedNetwork())
    return false;
  if (!SavedNetworkExists)
    return false;
  if (!LookForExternalWiFi)
    return false;
  WiFi.disconnect();
  WiFi.mode(WIFI_MODE_APSTA);
  bConnectedToExternalNetwork =
      WiFiConnect::ConnectToSaved(ConnectTimeout * 1000);
  if (bConnectedToExternalNetwork) // if connected with saved credentials is
                                   // successful we have to start the local AP
                                   // ourselves
  {
    ApplyPisteDhcpLease(PisteNrFromApSsid(soft_ap_ssid));
    WiFi.softAP(soft_ap_ssid.c_str(), soft_ap_password.c_str());
    ESP_LOGI(NETWORK_TAG, "ESP32 IP on the WiFi network: %s",
             (WiFi.localIP().toString()).c_str());
  }
  return bConnectedToExternalNetwork;
}

void SetIPAddress(int CurrentPisteNr) {
  if (CurrentPisteNr > 254)
    return;
  // Set your Gateway IP address

  Preferences networkpreferences;
  networkpreferences.begin("credentials", false);
  if (networkpreferences.getBool("UseDHCP", false))
    return;
  String strBaseAddress =
      networkpreferences.getString("BaseAddress", "172.20.255.1");
  networkpreferences.end();

  uint8_t octet1 = 172;
  uint8_t octet2 = 20;
  uint8_t octet3 = 255;
  uint8_t octet4 = 1;
  sscanf(strBaseAddress.c_str(), "%d.%d.%d.%d", &octet1, &octet2, &octet3,
         &octet4);
  IPAddress local_IP;
  IPAddress gateway(octet1, octet2, 0,
                    1); // I may want to make that condigurable too
  if (octet3 == 255) {
    local_IP = IPAddress(octet1, octet2, CurrentPisteNr, octet4);
  } else {
    if (octet4 == 255) {
      local_IP = IPAddress(octet1, octet2, octet3, CurrentPisteNr);
    } else
      return;
  }
  IPAddress subnet(255, 255, 0, 0);
  IPAddress primaryDNS(8, 8, 8, 8);   // optional
  IPAddress secondaryDNS(8, 8, 4, 4); // optional
  // Set your Static IP address
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    ESP_LOGE(NETWORK_TAG, "%s", "STA Failed to configure");
  }
}

void NetWork::GlobalStartWiFi() {
  if (m_GlobalWifiStarted)
    return;

  Preferences networkpreferences;
  networkpreferences.begin("credentials", false);
  int32_t PisteNr = networkpreferences.getInt("pisteNr", -1);
  soft_ap_password = networkpreferences.getString("AP_Password", "01041967");
  networkpreferences.end();
  networkpreferences.begin("scoringdevice", false);
  bool bIsrepeater = networkpreferences.getBool("RepeaterMode", false);
  networkpreferences.end();
  if (-1 != PisteNr) {
    char temp[8];
    int CurrentNr = FindFirstFreePisteID(PisteNr);
    sprintf(temp, "%03d", CurrentNr);
    soft_ap_ssid = "Piste_" + (String)temp;
    // Set your Static IP address
    SetIPAddress(CurrentNr);
  } else {
    char temp[8];
    sprintf(temp, "%03d", FindFirstFreePisteID(500));
    soft_ap_ssid = "Piste_" + (String)temp;
  }

  // In repeater mode this part is not needed, because we have to use the same
  // channel as the master
  if (!bIsrepeater) {
    if (!ConnectToExternalNetwork(15)) {
      bestchannel = findBestWifiChannel() + 1;
      WiFi.mode(WIFI_MODE_AP);

      ApplyPisteDhcpLease(PisteNrFromApSsid(soft_ap_ssid));
      WiFi.softAP(soft_ap_ssid.c_str(), soft_ap_password.c_str());
      esp_wifi_set_channel(bestchannel, WIFI_SECOND_CHAN_NONE);
    }
  } else {
    FindAndSetMasterChannel();
    WiFi.mode(WIFI_MODE_AP);
    ApplyPisteDhcpLease(PisteNrFromApSsid(soft_ap_ssid));
    WiFi.softAP(soft_ap_ssid.c_str(), soft_ap_password.c_str());
    esp_wifi_set_channel(bestchannel, WIFI_SECOND_CHAN_NONE);
  }

  m_GlobalWifiStarted = true;

  // Start mDNS
  MDNSResolver::getInstance().begin(soft_ap_ssid.c_str());

  startCalibrationWebServer();
  // esp_wifi_set_max_tx_power(20);
}

void NetWork::FindAndSetMasterChannel(int soft_retries,
                                      bool restart_on_timeout) {
  WiFi.disconnect();
  String MasterSSID;
  Preferences networkpreferences;
  networkpreferences.begin("scoringdevice", false);
  int32_t PisteNr = networkpreferences.getInt("MasterPiste", -1);

  networkpreferences.end();
  int tempchannel = -1;
  if (-1 != PisteNr) {
    char temp[8];
    sprintf(temp, "%03d", PisteNr);
    MasterSSID = "Piste_" + (String)temp;
    for (int j = soft_retries; j > 0; j--) {
      int networks = WiFi.scanNetworks();
      for (int i = 0; i < networks; ++i) {
        if (WiFi.SSID(i) == MasterSSID) {
          tempchannel = WiFi.channel(i);
          i = networks;
        }
      }
      if (tempchannel != -1)
        j = 0;
      yield();
    }
  }
  if (tempchannel != -1) {
    bestchannel = tempchannel;
    ESP_ERROR_CHECK(esp_wifi_set_channel(bestchannel, WIFI_SECOND_CHAN_NONE));
  } else if (restart_on_timeout)
    esp_restart();
}

void NetWork::update(UDPIOHandler *subject, uint32_t eventtype) {
  uint32_t maineventtype = eventtype & MAIN_TYPE_MASK;
  uint32_t subtype = eventtype & UI_SUB_TYPE_MASK;

  if (UI_CONNECT_TO_WIFI == subtype)
    ConnectToExternalNetwork(45);

  if (UI_START_WIFI_PORTAL == subtype) {
    // WifiSetupMode::RequestAndReboot() -- sets a flag and reboots into a
    // dedicated minimal mode (see WifiSetupMode.h). Replaces
    // CaptivePortal, which tried to scan WiFi networks while the live app
    // (this AsyncWebServer included) kept running -- real-world testing
    // 2026-08-14 confirmed that approach was fundamentally unreliable,
    // not just buggy in the specific fix attempts tried.
    WifiSetupMode::RequestAndReboot();
  }

  if (UI_START_OTA_PORTAL == subtype) {

    ElegantOTA.begin(&server); // Start ElegantOTA
    server.begin();

    ESP_LOGI(NETWORK_TAG, "%s", "HTTP OTA Update server started");
    return;
  }
  if (UI_FULL_RESET == subtype) {
    ESP.restart();
  }

  switch (subtype) {
  case UI_SWAP_FENCERS:
  case UI_RESERVE_LEFT:
  case UI_RESERVE_RIGHT:
  case UI_INPUT_CYRANO_NEXT:
  case UI_INPUT_CYRANO_PREV:
  case UI_INPUT_CYRANO_BEGIN:
  case UI_INPUT_CYRANO_END:
    if (!ConnectToExternalNetwork(45)) {
      WiFi.disconnect();
      WiFi.mode(WIFI_MODE_AP);
      ApplyPisteDhcpLease(PisteNrFromApSsid(soft_ap_ssid));
      WiFi.softAP(soft_ap_ssid.c_str(), soft_ap_password.c_str());
    }

    break;
  }
}

void NetWork::DoFactoryReset() {
  Preferences networkpreferences;
  networkpreferences.begin("credentials", false);
  networkpreferences.clear();
  networkpreferences.end();

  Preferences mypreferences;
  mypreferences.begin("scoringdevice", false);
  mypreferences.clear();
  mypreferences.end();
  nvs_flash_erase(); // erase the NVS partition and...
  nvs_flash_init();  // initialize the NVS partition.
}

NetWork::NetWork() {
  // ctor
  m_GlobalWifiStarted = false;
}

NetWork::~NetWork() {
  // dtor
}
