
// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
// platformio.ini sets -DLOG_LOCAL_LEVEL=0 (ESP_LOG_NONE) globally, silencing
// all ESP_LOGx output including errors. Overridden here (before
// AtlasAsyncMqttClient.h, which pulls in esp_log.h) so this file's own logging
// — including the new Tier A fragment-reassembly/mTLS logging — is visible
// while debugging.
// #undef LOG_LOCAL_LEVEL
// #define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "AtlasAsyncMqttClient.h"
#include "esp_task_wdt.h"
#include <Preferences.h>
#include <cstring>
#include <iostream>

bool AtlasAsyncMqttClient::isConnected() { return m_connected; }

static const char *TAG = "ATLAS_MQTT";

AtlasAsyncMqttClient::AtlasAsyncMqttClient() {}

AtlasAsyncMqttClient::~AtlasAsyncMqttClient() {
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
}

void AtlasAsyncMqttClient::setServer(IPAddress ip, uint16_t port) {
  m_host = std::string(ip.toString().c_str());
  m_port = port;
}

void AtlasAsyncMqttClient::setServer(const std::string host, uint16_t port) {
  m_host = host;
  m_port = port;
}

void AtlasAsyncMqttClient::setCredentials(const char *username,
                                          const char *password) {
  m_username = username ? username : "";
  m_password = password ? password : "";
}

void AtlasAsyncMqttClient::setClientId(const char *clientId) {
  m_clientId = clientId ? clientId : "";
}
void AtlasAsyncMqttClient::onConnect(mqtt_connect_cb_t cb) {
  connectCb = std::move(cb);
}

void AtlasAsyncMqttClient::onDisconnect(mqtt_disconnect_cb_t cb) {
  disconnectCb = std::move(cb);
}

void AtlasAsyncMqttClient::onMessage(mqtt_message_cb_t cb) {
  messageCb = std::move(cb);
}

void AtlasAsyncMqttClient::onSubscribe(mqtt_subscribe_cb_t cb) {
  subscribeCb = std::move(cb);
}

void AtlasAsyncMqttClient::publish(const char *topic, int qos, bool retain,
                                   const char *payload) {
  if (!client || !isConnected())
    return;
  esp_mqtt_client_publish(client, topic, payload, 0, qos, retain);
}

void AtlasAsyncMqttClient::publish(const char *topic, int qos, bool retain,
                                   const void *payload, size_t length) {
  if (!client || !isConnected())
    return;
  esp_mqtt_client_publish(client, topic, static_cast<const char *>(payload),
                          length, qos, retain);
}

void AtlasAsyncMqttClient::publishString(const char *topic, int qos,
                                         bool retain,
                                         const std::string &payload) {
  publish(topic, qos, retain, payload.c_str(), payload.length());
}

// Tier A (docs/level2.md §30.5) mTLS. cert_pem/key_pem are this device's own
// provisioned certificate + private key (TierAProvisioning persists both in
// NVS); ca_pem, if given, overrides whatever setTLS() already set — both are
// equally valid ways to supply the same broker CA, kept as separate parameters
// only because that's the signature already declared in the header.
void AtlasAsyncMqttClient::setTlsCerts(const char *cert_pem,
                                       const char *key_pem,
                                       const char *ca_pem) {
  m_client_cert = cert_pem ? cert_pem : "";
  m_client_key = key_pem ? key_pem : "";
  if (ca_pem)
    m_ca_cert = ca_pem;
}

void AtlasAsyncMqttClient::setWill(const char *topic, const char *message,
                                   int qos, bool retain) {
  m_lwtTopic = topic;
  m_lwtMessage = message;
  m_lwtQos = qos;
  m_lwtRetain = retain;
  m_lwtEnabled = true;
}

void AtlasAsyncMqttClient::subscribe(const char *topic, int qos) {
  if (!client || !isConnected())
    return;
  esp_mqtt_client_subscribe(client, topic, qos);
}

esp_err_t
AtlasAsyncMqttClient::mqttEventHandlerCb(esp_mqtt_event_handle_t event) {
  auto *instance = static_cast<AtlasAsyncMqttClient *>(event->user_context);
  if (instance) {
    instance->handleEvent(event);
  }
  return ESP_OK;
}

void AtlasAsyncMqttClient::handleEvent(esp_mqtt_event_handle_t event) {
  switch (event->event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    m_connected = true;
    if (connectCb)
      connectCb(event->session_present);
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    m_connected = false;
    if (disconnectCb)
      disconnectCb();
    break;
  case MQTT_EVENT_DATA:
    // A message larger than esp-mqtt's internal buffer (default 1024 bytes)
    // arrives split across several MQTT_EVENT_DATA events — current_data_offset
    // == 0 marks the first fragment (the only one carrying the topic);
    // reassemble until current_data_offset + data_len reaches total_data_len.
    if (event->current_data_offset == 0) {
      m_incomingTopic.assign(event->topic, event->topic_len);
      m_incomingPayload.clear();
      m_incomingPayload.reserve(event->total_data_len);
    }
    m_incomingPayload.append(event->data, event->data_len);

    if (event->current_data_offset + event->data_len >= event->total_data_len) {
      ESP_LOGI(TAG, "MQTT_EVENT_DATA complete (%d bytes, topic %s)",
               event->total_data_len, m_incomingTopic.c_str());
      if (messageCb)
        messageCb(m_incomingTopic.c_str(), m_incomingPayload.c_str(),
                  m_incomingPayload.size());
    }
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    if (subscribeCb)
      subscribeCb(event->msg_id, 1); // assume QoS 1
    break;
  default:
    break;
  }
}

void AtlasAsyncMqttClient::begin() {
  if (m_HasBegun)
    return;

  esp_mqtt_client_config_t mqtt_cfg = {};

  mqtt_cfg.host = m_host.c_str();
  mqtt_cfg.port = m_port;
  mqtt_cfg.transport =
      m_tlsEnabled ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;

  mqtt_cfg.username = m_username.empty() ? nullptr : m_username.c_str();
  mqtt_cfg.password = m_password.empty() ? nullptr : m_password.c_str();
  mqtt_cfg.client_id = m_clientId.empty() ? nullptr : m_clientId.c_str();
  mqtt_cfg.event_handle = mqttEventHandlerCb;
  mqtt_cfg.user_context = this;
  if (m_lwtEnabled) {
    mqtt_cfg.lwt_topic = m_lwtTopic.c_str();
    mqtt_cfg.lwt_msg = m_lwtMessage.c_str();
    mqtt_cfg.lwt_qos = m_lwtQos;
    mqtt_cfg.lwt_retain = m_lwtRetain;
  }
  // Fast disconnect/reconnect settings
  mqtt_cfg.keepalive = 5;               // seconds, fast dead peer detection
  mqtt_cfg.reconnect_timeout_ms = 2000; // ms, fast reconnect
  mqtt_cfg.network_timeout_ms = 5000;   // ms, fast socket timeout

  // TLS / CA certificate (optional, for server cert verification)
  if (m_tlsEnabled && !m_ca_cert.empty()) {
    mqtt_cfg.cert_pem = m_ca_cert.c_str();
    ESP_LOGE(TAG, "certificate set");

    // KNOWN TRADEOFF — decided deliberately 2026-07-14, revisit if this ever
    // needs tightening:
    //
    // This device connects via a resolved IP address (m_host, set from
    // MDNSResolver::resolveHostname()'s IPAddress result in
    // CyranoHandler::Begin()), never a hostname string. The broker's TLS
    // certificate (scripts/generate-tls-cert.sh, on the Atlas side) only lists
    // "openpiste.local" / "localhost" / "127.0.0.1" in its SAN — never an
    // arbitrary LAN IP. Confirmed on real hardware: without the line below,
    // every mTLS connection attempt fails the handshake with
    // "Failed to verify peer certificate!" (mbedtls -0x2700,
    // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED), unconditionally, regardless of
    // whether the certificate is otherwise entirely valid.
    //
    // skip_cert_common_name_check disables ONLY the hostname/CN-vs-SAN match —
    // the certificate CHAIN is still fully verified against m_ca_cert above
    // (Atlas's own locally-generated CA; nothing else is trusted). A rogue
    // device still cannot pass this handshake without a certificate actually
    // signed by that CA. This was a deliberate choice, not an oversight: full
    // hostname verification would need CyranoHandler::Begin() to connect via
    // the "openpiste.local" hostname string instead of a pre-resolved IP —
    // that touches shared connection-setup code the existing anonymous/legacy
    // Cyrano path also depends on, judged a bigger/riskier change than this
    // narrower one for now. Revisit if that ever changes, or if this
    // trust-model tradeoff (docs/level2.md §30.1: "physically-secured local
    // network... not bank-grade") stops being acceptable for a given
    // deployment.
    mqtt_cfg.skip_cert_common_name_check = true;
  }

  // Tier A (docs/level2.md §30.5) mTLS — this device's own provisioned client
  // certificate, presented to the broker at the TLS handshake. Additive: a
  // never-provisioned device leaves these empty and connects exactly as before
  // (anonymous, whatever setCredentials()/username-password may separately
  // add).
  if (m_tlsEnabled && !m_client_cert.empty() && !m_client_key.empty()) {
    mqtt_cfg.client_cert_pem = m_client_cert.c_str();
    mqtt_cfg.client_key_pem = m_client_key.c_str();
    ESP_LOGI(TAG, "Tier A client certificate set");
  }

  client = esp_mqtt_client_init(&mqtt_cfg);
  if (client == nullptr) {
    return;
  }
  // Try to start the MQTT client and check the return value
  esp_err_t err = esp_mqtt_client_start(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start MQTT client: %d", err);
    return;
  }
  esp_mqtt_client_register_event(
      client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
      &AtlasAsyncMqttClient::eventHandler, this);

  m_HasBegun = true;
}

void AtlasAsyncMqttClient::clearWill() {
  m_lwtTopic.clear();
  m_lwtMessage.clear();
  m_lwtQos = 0;
  m_lwtRetain = false;
  m_lwtEnabled = false;
}

void AtlasAsyncMqttClient::disconnect() {
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
    m_HasBegun = false;
    ESP_LOGI(TAG, "MQTT client disconnected and destroyed");
  }
}
void AtlasAsyncMqttClient::reconnectWithNewSettings() {
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
  m_HasBegun = false;
  begin();
}

void AtlasAsyncMqttClient::eventHandler(void *handler_args,
                                        esp_event_base_t base, int32_t event_id,
                                        void *event_data) {
  auto *instance = static_cast<AtlasAsyncMqttClient *>(handler_args);
  instance->handleEvent(static_cast<esp_mqtt_event_handle_t>(event_data));
}
