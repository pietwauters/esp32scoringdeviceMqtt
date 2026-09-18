// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
//
// platformio.ini sets -DLOG_LOCAL_LEVEL=0 (ESP_LOG_NONE) globally, silencing
// all ESP_LOGx output including errors. Overridden here (before any include
// that transitively pulls in esp_log.h) so this file's logging is visible
// while debugging the 2026-09-18 "never connects after an outage" issue.
// Comment back out once resolved -- see Opp2Handler.cpp's identical toggle.
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "BrokerDiscovery.h"
#include "AtlasAsyncMqttClient.h"
#include "RTOSSettings.h"
#include "TaskDiagnostics.h"
#include <IPAddress.h>
#include <esp_log.h>
#include <mdns.h>

static const char *TAG = "BrokerDiscovery";

extern AtlasAsyncMqttClient &mqttClient; // Shared MQTT client singleton

BrokerDiscovery::BrokerDiscovery()
    : m_Port(0), m_Searching(false), m_TaskHandle(nullptr) {}

void BrokerDiscovery::Begin(const char *mdnsHostname, uint16_t port) {
  m_MdnsHostname = mdnsHostname;
  m_Port = port;
  OnDisconnected(); // nothing connected yet -- start the race immediately
}

void BrokerDiscovery::OnDisconnected() { StartSearching(); }

void BrokerDiscovery::OnConnected() {
  // Connected is connected, full stop -- no periodic re-check of the other
  // candidate while a working connection exists. searchTask() notices
  // m_Searching==false on its next loop iteration and exits on its own; if
  // it's mid-query when this fires, that in-flight result is discarded.
  if (m_Searching) {
    ESP_LOGI(TAG, "Connected -- stopping mDNS search");
  }
  m_Searching = false;
}

void BrokerDiscovery::StartSearching() {
  if (m_Searching || m_TaskHandle != nullptr) {
    ESP_LOGI(TAG, "StartSearching: already racing, ignoring");
    return; // already racing
  }
  ESP_LOGI(TAG, "StartSearching: beginning mDNS race for %s",
           m_MdnsHostname.c_str());
  m_Searching = true;
  xTaskCreatePinnedToCore(searchTask, "broker_mdns", STACK_BROKER_DISCOVERY,
                           this, PRIORITY_BROKER_DISCOVERY, &m_TaskHandle,
                           CORE_BROKER_DISCOVERY);
  TaskDiagnostics::Register(m_TaskHandle, "broker_mdns");
}

void BrokerDiscovery::searchTask(void *param) {
  BrokerDiscovery *self = static_cast<BrokerDiscovery *>(param);

  while (self->m_Searching) {
    esp_ip4_addr_t addr;
    esp_err_t err = mdns_query_a(self->m_MdnsHostname.c_str(), 3000, &addr);

    // The client's own built-in reconnect (esp-mqtt, reconnect_timeout_ms)
    // is racing the static/configured IP in parallel the whole time this
    // query blocks -- if it already won, don't act on a stale mDNS result.
    if (self->m_Searching) {
      if (err != ESP_OK) {
        ESP_LOGI(TAG, "mDNS lookup for %s failed (err=%d), currently using %s",
                 self->m_MdnsHostname.c_str(), err,
                 mqttClient.getHost().c_str());
      } else {
        IPAddress resolved(addr.addr);
        std::string resolvedStr(resolved.toString().c_str());
        if (resolvedStr != mqttClient.getHost()) {
          ESP_LOGI(TAG,
                   "mDNS resolved %s to %s (currently using %s) -- switching",
                   self->m_MdnsHostname.c_str(), resolvedStr.c_str(),
                   mqttClient.getHost().c_str());
          mqttClient.setServer(resolved, self->m_Port);
          mqttClient.reconnectWithNewSettings();
        } else {
          ESP_LOGI(TAG, "mDNS resolved %s to %s -- already using it",
                   self->m_MdnsHostname.c_str(), resolvedStr.c_str());
        }
      }
    }

    if (self->m_Searching) {
      vTaskDelay(pdMS_TO_TICKS(2000)); // pace retries between attempts
    }
  }

  self->m_TaskHandle = nullptr;
  vTaskDelete(nullptr);
}
