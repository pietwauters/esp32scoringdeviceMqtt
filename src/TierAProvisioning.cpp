// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// platformio.ini sets -DLOG_LOCAL_LEVEL=0 (ESP_LOG_NONE) globally, silencing
// all ESP_LOGx output including errors. Overridden here, before any header that
// would otherwise lock it in first, so this file's Tier A logging is visible
// while debugging — does not affect any other file's log verbosity.
// #undef LOG_LOCAL_LEVEL
// #define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "TierAProvisioning.h"
#include "AbsoluteTime.h"
#include "AtlasAsyncMqttClient.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>
#include <esp_log.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>

static const char *TAG = "TIER_A";
static const char *NVS_NAMESPACE = "tier_a";

extern AtlasAsyncMqttClient
    &mqttClient; // shared singleton, defined in CyranoHandler.cpp

std::string TierAProvisioning::GetOrCreateDeviceId() {
  if (!m_deviceId.empty())
    return m_deviceId;

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  m_deviceId = prefs.getString("device_id", "").c_str();
  if (m_deviceId.empty()) {
    // Derive a stable id from the MAC address once, persist thereafter.
    // Sanitized to match the CMS's own device_id validation
    // (services/provisioning.js:
    // ^[a-zA-Z0-9_-]{1,64}$) — a raw MAC's colons wouldn't pass that.
    std::string mac = WiFi.macAddress().c_str();
    std::string sanitized;
    for (char c : mac)
      if (c != ':')
        sanitized += c;
    m_deviceId = "esp32-" + sanitized;
    prefs.putString("device_id", m_deviceId.c_str());
  }
  prefs.end();
  return m_deviceId;
}

bool TierAProvisioning::HasCertificate() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  bool has = prefs.getString("cert", "").length() > 0;
  prefs.end();
  return has;
}

TierAProvisioning::StorageDebugInfo TierAProvisioning::GetStorageDebugInfo() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  StorageDebugInfo info;
  info.certLen = prefs.getString("cert", "").length();
  info.privKeyLen = prefs.getString("priv_key", "").length();
  info.caCertLen = prefs.getString("ca_cert", "").length();
  prefs.end();
  info.requestPending = m_requestPending;
  info.requestAgeMs = m_requestPending ? (millis() - m_requestSentAtMs) : 0;
  return info;
}

void TierAProvisioning::LoadStoredCertsIntoClient() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  std::string cert = prefs.getString("cert", "").c_str();
  std::string key = prefs.getString("priv_key", "").c_str();
  std::string caCert = prefs.getString("ca_cert", "").c_str();
  prefs.end();

  if (cert.empty() || key.empty()) {
    ESP_LOGI(TAG,
             "No stored Tier A certificate — connecting anonymously as before");
    return;
  }

  ESP_LOGI(TAG, "Loaded Tier A certificate from NVS — enabling mTLS");
  mqttClient.setTlsCerts(cert.c_str(), key.c_str(), caCert.c_str());
  mqttClient.setTLS(true, caCert);
  mqttClient.setServer(mqttClient.getHost(), 8883);
}

// Standard mbedtls EC keypair + PKCS#10 CSR generation — the private key stays
// in keyPemOut/pk the whole time; only the CSR (public information) is ever
// published.
static bool generateKeyAndCsr(std::string &keyPemOut, std::string &csrPemOut) {
  mbedtls_pk_context pk;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_x509write_csr csr;

  mbedtls_pk_init(&pk);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_x509write_csr_init(&csr);

  bool ok = false;
  const char *pers = "tier_a_provisioning";

  do {
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
      ESP_LOGE(TAG, "ctr_drbg_seed failed");
      break;
    }
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) !=
        0) {
      ESP_LOGE(TAG, "pk_setup failed");
      break;
    }
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
      ESP_LOGE(TAG, "ecp_gen_key failed");
      break;
    }

    unsigned char keyBuf[1024] = {0};
    if (mbedtls_pk_write_key_pem(&pk, keyBuf, sizeof(keyBuf)) != 0) {
      ESP_LOGE(TAG, "pk_write_key_pem failed");
      break;
    }
    keyPemOut = (const char *)keyBuf;

    // Subject is irrelevant — the CMS discards it and re-issues with its own
    // CN (role-device_id), same reasoning as services/provisioning.js's
    // signCertificate. Only the public key inside this CSR matters.
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &pk);
    if (mbedtls_x509write_csr_set_subject_name(&csr, "CN=openpiste-device") !=
        0) {
      ESP_LOGE(TAG, "set_subject_name failed");
      break;
    }

    unsigned char csrBuf[1024] = {0};
    if (mbedtls_x509write_csr_pem(&csr, csrBuf, sizeof(csrBuf),
                                  mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
      ESP_LOGE(TAG, "x509write_csr_pem failed");
      break;
    }
    csrPemOut = (const char *)csrBuf;
    ok = true;
  } while (0);

  mbedtls_x509write_csr_free(&csr);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  mbedtls_pk_free(&pk);
  return ok;
}

bool TierAProvisioning::GenerateAndRequest(const char *code, const char *role,
                                           const char *deviceLabel) {
  unsigned long now = millis();
  if (m_requestPending && (now - m_requestSentAtMs) < PENDING_TIMEOUT_MS) {
    // A prior request is still awaiting a response — e.g. a browser
    // resubmitting the /provision form on refresh. Reject rather than overwrite
    // m_pendingKeyPem, which would silently corrupt whichever response arrives
    // (this is exactly the failure mode a real device hit: the earlier grant
    // for the first request was discarded because a refresh-triggered second
    // request had already clobbered the pending key).
    ESP_LOGW(TAG,
             "Tier A: a request is already in flight — ignoring this one "
             "(retry once the current one grants/denies, or wait %lus for "
             "it to expire)",
             (PENDING_TIMEOUT_MS - (now - m_requestSentAtMs)) / 1000);
    return false;
  }

  std::string csrPem;
  if (!generateKeyAndCsr(m_pendingKeyPem, csrPem)) {
    ESP_LOGE(TAG, "Key/CSR generation failed — aborting provisioning request");
    return false;
  }

  std::string deviceId = GetOrCreateDeviceId();
  std::string responseTopic = "openpiste/_provision/response/" + deviceId;
  mqttClient.subscribe(responseTopic.c_str(), 1);

  JsonDocument doc;
  doc["protocol"] = "OPP2";
  doc["version"] = "1.0";
  doc["seq"] = 1;
  doc["ts"] = AbsoluteTime::getInstance().getTimestamp();
  doc["code"] = code;
  doc["role"] = role;
  doc["device_id"] = deviceId;
  if (deviceLabel)
    doc["device_label"] = deviceLabel;
  doc["csr"] = csrPem;

  std::string payload;
  serializeJson(doc, payload);
  mqttClient.publish("openpiste/_provision/request", 1, false, payload.c_str());
  m_requestPending = true;
  m_requestSentAtMs = now;
  ESP_LOGI(TAG,
           "Tier A provisioning request published for device_id=%s role=%s",
           deviceId.c_str(), role);
  return true;
}

void TierAProvisioning::HandleResponse(const char *payload, size_t length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    ESP_LOGE(TAG, "Tier A response: JSON parse error: %s", err.c_str());
    return;
  }

  const char *status = doc["status"] | "";
  if (strcmp(status, "granted") != 0) {
    const char *reason = doc["reason"] | "unknown";
    ESP_LOGW(TAG, "Tier A provisioning denied: %s", reason);
    m_pendingKeyPem.clear();
    m_requestPending = false;
    return;
  }

  if (m_pendingKeyPem.empty()) {
    // A granted response with no matching pending request — e.g. a duplicate
    // delivery after this device already finished provisioning. Ignore.
    ESP_LOGW(TAG, "Tier A: granted response with no pending request — ignored");
    m_requestPending = false;
    return;
  }

  std::string cert = doc["cert"] | "";
  std::string caCert = doc["ca_cert"] | "";
  if (cert.empty() || caCert.empty()) {
    ESP_LOGE(TAG, "Tier A: granted response missing cert/ca_cert — ignored");
    m_requestPending = false;
    return;
  }

  ESP_LOGI(TAG, "Tier A provisioning granted — persisting certificate to NVS");
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString("cert", cert.c_str());
  prefs.putString("priv_key", m_pendingKeyPem.c_str());
  prefs.putString("ca_cert", caCert.c_str());
  prefs.end();

  ESP_LOGI(
      TAG,
      "Certificate applied — reconnect staged for the next main-loop tick");
  mqttClient.setTlsCerts(cert.c_str(), m_pendingKeyPem.c_str(), caCert.c_str());
  mqttClient.setTLS(true, caCert);
  mqttClient.setServer(mqttClient.getHost(), 8883);
  m_pendingKeyPem.clear();
  m_requestPending = false;
  // Deliberately NOT calling reconnectWithNewSettings() here — this function
  // runs inside the MQTT client's own event-handler task, and
  // esp_mqtt_client_stop()/_destroy() must not be called from that context. See
  // ApplyReconnectIfPending()'s doc comment.
  m_reconnectPending = true;
}

void TierAProvisioning::ApplyReconnectIfPending() {
  if (!m_reconnectPending)
    return;
  m_reconnectPending = false;
  ESP_LOGI(TAG, "Switching to mTLS on port 8883 and reconnecting...");
  mqttClient.reconnectWithNewSettings();
}
